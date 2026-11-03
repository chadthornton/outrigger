#!/usr/bin/env python3
"""Publish the installer site on Cloudflare Pages (outrigger.music).

    tools/deploy_site.py guard <channel>             refuse early, before make check spends minutes
    tools/deploy_site.py deploy <channel> <site dir>  stage, check and upload a site make check built
                                                     (build/checked/site; its commit in build/checked/commit)
    tools/deploy_site.py rollback <channel> [commit]  put the deploy before the live one back (or that commit's)

Channels: production (outrigger.music, from origin/master only) and beta
(beta.outrigger.music, any pushed commit). `make deploy-site` runs both steps
around `make check`, so only a site built from a clean checkout of a committed,
pushed HEAD goes live. Each upload carries its commit, so the Pages dashboard
answers "which commit is live?". The Pages Functions (functions/) and their
config (wrangler.toml, with the D1 bindings) go up with the site, from the same
checked commit: wrangler reads them from its working directory, so it runs in
the staging folder, never in the worktree.

Rollback: every deploy and rollback is a line in the deploy log (DEPLOYS, shared
by every worktree), with its commit, Pages deployment ID, time and session.
Production rolls back through Cloudflare's own rollback, which only takes
production deployments: seconds, no upload, and the old Functions come back with
the old site. Beta can't, so each deploy keeps what it uploaded (site, functions/,
wrangler.toml, with SHA-256 sums) and a beta rollback uploads that copy again.
Neither re-runs make check: what goes back up already passed it.

Credentials: ~/.config/cloudflare/outrigger-pages.env (CLOUDFLARE_API_TOKEN with
Account > Cloudflare Pages > Edit and Account > D1 > Edit, and
CLOUDFLARE_ACCOUNT_ID). Never printed.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_site import APP_SETS, WL82_SHA256, load_wl82_blob, wl82_chunks, wl82_problem  # noqa: E402

PROJECT = "outrigger"
# Pages branch per channel. The project's production branch is master; beta.outrigger.music
# is a custom domain on the beta branch alias (beta.outrigger.pages.dev).
BRANCHES = {"production": "master", "beta": "beta"}
# At least two weeks old: npm here refuses releases younger than a week.
WRANGLER = "wrangler@4.151.1"
ENV_FILE = Path.home() / ".config/cloudflare/outrigger-pages.env"
# The upload's working directory: site/, functions/ and wrangler.toml, as in the repo.
STAGE = ROOT / "build/pages"
# The deploy log and beta's kept copies, outside build/ so every worktree sees the same ones.
DEPLOYS = Path.home() / "Library/Application Support/outrigger/deploys"
KEEP = 3  # kept copies per channel
SUMS = "SHA256SUMS"
API = "https://api.cloudflare.com/client/v4/accounts/{account}/pages/projects/" + PROJECT
URLS = {"production": "https://outrigger.music", "beta": "https://beta.outrigger.music"}
# Cloudflare Pages limits (developers.cloudflare.com/pages/platform/limits/).
MAX_FILE = 25 * 1024 * 1024
MAX_FILES = 20000


def fail(msg):
    sys.exit(f"deploy_site: {msg}")


def refusal(channel, head, origin_master, pushed, dirty):
    """Why this commit may not go to this channel, or None."""
    if channel not in BRANCHES:
        return f"unknown channel {channel!r}; use production or beta"
    if dirty:
        return "the tree has uncommitted changes; commit them first"
    if channel == "production" and head != origin_master:
        return "production deploys only origin/master's commit; merge and push first, or deploy to beta"
    if not pushed:
        return "HEAD is on no remote branch; push it first"
    return None


def site_problems(site, blob, sha=WL82_SHA256, max_file=MAX_FILE, max_files=MAX_FILES):
    """Everything wrong with a staged site. blob: the local wl82loader.bin, or None."""
    files = sorted(p for p in site.rglob("*") if p.is_file())
    problems = []
    if not (site / "index.html").is_file():
        problems.append("no index.html")
    boards = [d for d in (site / "firmware").glob("*") if d.is_dir()]
    if not boards:
        problems.append("no firmware under firmware/; run make site")
    for board in boards:
        for app_set in APP_SETS:
            manifest = board / app_set / "manifest.json"
            if not manifest.is_file():
                problems.append(f"no {manifest.relative_to(site)}; make site stopped partway?")
                continue
            for build in json.loads(manifest.read_text())["builds"]:
                for part in build["parts"]:
                    if not (manifest.parent / part["path"]).is_file():
                        problems.append(f"{manifest.relative_to(site)} names {part['path']}, which is missing")
    if len(files) > max_files:
        problems.append(f"{len(files)} files, Pages takes {max_files}")
    if blob is None:
        problems.append("no local wl82loader.bin to scan against (tests/fixtures/ or $WL82_BLOB); run make fixtures")
    chunks = wl82_chunks(blob)
    for path in files:
        name = path.relative_to(site)
        data = path.read_bytes()
        if len(data) > max_file:
            problems.append(f"{name} is {len(data)} bytes, Pages takes {max_file} per file")
        problem = wl82_problem(path.name, data, chunks, sha)
        if problem:
            problems.append(f"{name} {problem}; it must not be published")
    return problems


def built_from_problem(site, head):
    """Why site (make check's build/checked/site) is not HEAD's, or None."""
    commit = site.parent / "commit"
    if not commit.is_file():
        return f"no {commit}; run make check"
    built = commit.read_text().strip()
    if built != head:
        return f"{site} was built from {built[:7]}, HEAD is {head[:7]}; run make check again"
    return None


def stage(src, dst):
    """Copy src to an emptied dst, leaving out dotfiles (.DS_Store and the like)."""
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst, ignore=lambda _, names: [n for n in names if n.startswith(".")])


def stage_root(checked, dst):
    """build/checked's site, functions/ and wrangler.toml into an emptied dst. Returns what's missing."""
    shutil.rmtree(dst, ignore_errors=True)
    stage(checked / "site", dst / "site")
    missing = []
    if (checked / "functions").is_dir():
        stage(checked / "functions", dst / "functions")
    else:
        missing.append("no functions/ beside the site; run make check")
    if (checked / "wrangler.toml").is_file():
        shutil.copyfile(checked / "wrangler.toml", dst / "wrangler.toml")
    else:
        missing.append("no wrangler.toml beside the site; run make check")
    return missing


def live_history(entries, channel):
    """The channel's deploys that are still in line to be live, oldest first; the last one is live.

    A deploy goes on top (replacing the top if it is the same commit). A rollback takes
    off everything above its target, so a second rollback goes further back instead of
    forward to the build the first one rolled away from."""
    stack = []
    for e in entries:
        if e["channel"] != channel:
            continue
        if e["action"] == "rollback":
            while stack and stack[-1]["commit"] != e["commit"]:
                stack.pop()
            if stack:
                stack.pop()
        elif stack and stack[-1]["commit"] == e["commit"]:
            stack.pop()
        stack.append(e)
    return stack


def rollback_target(entries, channel, live, commit=None):
    """(entry to put back, None) or (None, why not). live: the commit Cloudflare says is live;
    commit: a full or 7+ character commit to go back to, instead of the one before live."""
    if channel not in BRANCHES:
        return None, f"unknown channel {channel!r}; use production or beta"
    stack = live_history(entries, channel)
    if stack and stack[-1]["commit"] != live:
        return None, (f"{channel} serves {live[:7]}, but the deploy log's last {channel} deploy is "
                      f"{stack[-1]['commit'][:7]}; something deployed around the log. Roll back in the Pages dashboard")
    if commit is None:
        if len(stack) < 2:
            return None, f"no previous {channel} deployment in the deploy log to roll back to"
        return stack[-2], None
    if len(commit) < 7 or not any(e["commit"].startswith(commit) for e in stack):
        return None, f"unknown commit {commit!r}: not a {channel} deploy the log can go back to"
    if stack[-1]["commit"].startswith(commit):
        return None, f"{commit} is already live on {channel}"
    return next(e for e in reversed(stack[:-1]) if e["commit"].startswith(commit)), None


def needs_baseline(entries, channel, live):
    """Whether the log doesn't know the live commit (a first deploy, or one made around the log)."""
    stack = live_history(entries, channel)
    return not stack or stack[-1]["commit"] != live


def beta_first_problem(channel, head, beta):
    """Production only takes the commit beta serves (README, "Shipping to production")."""
    if channel == "production" and head != beta:
        return f"beta serves {beta[:7]}, not {head[:7]}; deploy it to beta and check it there first"
    return None


def read_log(path):
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def append_log(path, entry):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as f:
        f.write(json.dumps(entry, sort_keys=True) + "\n")


def tree_sums(root):
    """SHA-256 of every file under root, leaving out SUMS and dotfiles (wrangler's .wrangler/ cache)."""
    files = (p for p in sorted(root.rglob("*")) if p.is_file() and p.name != SUMS)
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in files if not any(part.startswith(".") for part in p.relative_to(root).parts)}


def keep(staged, channel_dir, commit, count=KEEP):
    """Copy what was uploaded (site/, functions/, wrangler.toml) to channel_dir/<commit>, with
    its SHA-256 sums; keep the newest count copies. Returns the copy."""
    dst = channel_dir / commit
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(staged, dst, ignore=lambda _, names: [n for n in names if n.startswith(".")])
    (dst / SUMS).write_text(json.dumps({"kept": time.time_ns(), "files": tree_sums(dst)}, indent=1))
    copies = sorted((json.loads((d / SUMS).read_text())["kept"], d) for d in channel_dir.iterdir()
                    if (d / SUMS).is_file())
    for _, old in copies[:-count]:
        shutil.rmtree(old)
    return dst


def kept_problems(kept):
    """Everything that differs between a kept copy and the sums taken when it was kept."""
    if not (kept / SUMS).is_file():
        return [f"no kept copy at {kept}"]
    want = {name: sha for name, sha in json.loads((kept / SUMS).read_text())["files"].items()
            if not any(part.startswith(".") for part in Path(name).parts)}
    have = tree_sums(kept)
    problems = [f"{name} is missing" for name in sorted(want.keys() - have.keys())]
    problems += [f"{name} was not in the deploy" for name in sorted(have.keys() - want.keys())]
    problems += [f"{name} has changed" for name in sorted(want.keys() & have.keys()) if want[name] != have[name]]
    return problems


def git(*args, cwd=ROOT):
    run = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if run.returncode:
        fail(f"git {' '.join(args)} failed: {run.stderr.strip()}")
    return run.stdout.strip()


def guard(channel):
    git("fetch", "-q", "origin")
    head = git("rev-parse", "HEAD")
    problem = refusal(channel, head, git("rev-parse", "origin/master"),
                      bool(git("branch", "-r", "--contains", head)),
                      bool(git("status", "--porcelain", "--untracked-files=no")))
    if not problem and channel == "production":
        problem = beta_first_problem(channel, head, commit_of(live_deployment(load_credentials(), "beta")))
    if problem:
        fail(problem)
    return head


def read_env(path):
    """KEY=value lines of a shell env file (optionally `export`ed and quoted)."""
    env = {}
    for line in path.read_text().splitlines():
        key, sep, value = line.strip().removeprefix("export ").partition("=")
        if sep and not key.startswith("#"):
            env[key.strip()] = value.strip().strip('"\'')
    return env


def load_credentials():
    if not ENV_FILE.is_file():
        fail(f"missing {ENV_FILE}; see README, 'Publishing the site'")
    env = dict(os.environ, **read_env(ENV_FILE))
    if not env.get("CLOUDFLARE_API_TOKEN") or not env.get("CLOUDFLARE_ACCOUNT_ID"):
        fail(f"{ENV_FILE} needs CLOUDFLARE_API_TOKEN and CLOUDFLARE_ACCOUNT_ID")
    return env


def api(env, path="", method="GET"):
    """A Pages API call for the project; its result."""
    req = urllib.request.Request(API.format(account=env["CLOUDFLARE_ACCOUNT_ID"]) + path, method=method,
                                 headers={"Authorization": f"Bearer {env['CLOUDFLARE_API_TOKEN']}"})
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            body = json.load(resp)
    except urllib.error.HTTPError as e:
        try:
            body = json.loads(e.read() or b"{}")
        except ValueError:
            body = {"errors": f"HTTP {e.code}"}
    except (urllib.error.URLError, TimeoutError, ValueError) as e:
        body = {"errors": str(e)}
    if not body.get("success"):
        fail(f"Pages API {method} {path or '/'} failed: {body.get('errors')}")
    return body["result"]


def commit_of(deployment):
    return deployment["deployment_trigger"]["metadata"]["commit_hash"]


def live_deployment(env, channel):
    """The deployment the channel serves: production's canonical one, or the beta branch's newest."""
    if channel == "production":
        return api(env)["canonical_deployment"]
    for d in api(env, "/deployments?env=preview&per_page=25"):
        if d["deployment_trigger"]["metadata"].get("branch") == BRANCHES[channel] \
                and d["latest_stage"]["status"] == "success":
            return d
    fail(f"no successful {channel} deployment on Pages")


def log_entry(action, channel, deployment, **extra):
    return dict(action=action, channel=channel, commit=commit_of(deployment), deployment=deployment["id"],
                time=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                session=os.environ.get("CLAUDE_CODE_SESSION_ID", "unknown"), **extra)


def wait_live(env, channel, ok, tries=10):
    """The channel's live deployment once ok(it) holds, or the last one seen (the API can lag)."""
    for _ in range(tries):
        live = live_deployment(env, channel)
        if ok(live):
            break
        time.sleep(3)
    return live


def upload(env, staged, channel, commit, message):
    run = subprocess.run(["npx", "--yes", WRANGLER, "pages", "deploy", "site",
                          "--project-name", PROJECT, "--branch", BRANCHES[channel],
                          "--commit-hash", commit, "--commit-message", message,
                          "--commit-dirty=false"], env=env, cwd=staged)
    if run.returncode:
        fail(f"wrangler failed (exit {run.returncode}); its output is above")
    live = wait_live(env, channel, lambda d: commit_of(d) == commit)
    if commit_of(live) != commit:
        fail(f"uploaded {commit[:7]}, but {channel} serves {commit_of(live)[:7]}; check the Pages dashboard")
    return live


def deploy(channel, site):
    head = guard(channel)
    problem = built_from_problem(site, head)
    if problem:
        fail(problem)
    problems = stage_root(site.parent, STAGE) + site_problems(STAGE / "site", load_wl82_blob())
    if problems:
        fail("refusing to upload:\n  " + "\n  ".join(problems))
    env = load_credentials()
    log = DEPLOYS / "deploys.jsonl"
    before = live_deployment(env, channel)
    if needs_baseline(read_log(log), channel, commit_of(before)):
        # Note what this deploy replaces, so a rollback can go back to it.
        append_log(log, log_entry("baseline", channel, before))
    live = upload(env, STAGE, channel, head, git("log", "-1", "--format=%s", head))
    append_log(log, log_entry("deploy", channel, live))
    if channel != "production":  # production rolls back through Cloudflare, with no copy
        keep(STAGE, DEPLOYS / channel, head)
    print(f"deployed {head[:7]} to {channel}: {URLS[channel]} (deployment {live['id']}; logged in {log})")


def rollback(channel, commit=None):
    """Put the deploy before the live one (or commit's) back on channel."""
    if channel not in BRANCHES:
        fail(f"unknown channel {channel!r}; use production or beta")
    env = load_credentials()
    log = DEPLOYS / "deploys.jsonl"
    live = live_deployment(env, channel)
    target, problem = rollback_target(read_log(log), channel, commit_of(live), commit)
    if problem:
        fail(problem)
    if channel == "production":
        # Cloudflare points production back at that deployment: no upload, its Functions with it.
        api(env, f"/deployments/{target['deployment']}/rollback", method="POST")
        now = wait_live(env, channel, lambda d: d["id"] == target["deployment"])
        if now["id"] != target["deployment"]:
            fail(f"rolled back, but production serves deployment {now['id']}; check the Pages dashboard")
    else:
        kept = DEPLOYS / channel / target["commit"]
        problems = kept_problems(kept)
        if problems:
            fail(f"can't roll {channel} back to {target['commit'][:7]}:\n  " + "\n  ".join(problems))
        now = upload(env, kept, channel, target["commit"], f"Rollback to {target['commit'][:7]}")
    append_log(log, log_entry("rollback", channel, now, **{"from": commit_of(live)}))
    print(f"rolled {channel} back from {commit_of(live)[:7]} to {target['commit'][:7]}: {URLS[channel]} "
          f"serves {target['commit'][:7]} (deployment {now['id']}; logged in {log})")


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "guard":
        guard(args[1])
    elif len(args) == 3 and args[0] == "deploy":
        deploy(args[1], Path(args[2]).resolve())
    elif len(args) in (2, 3) and args[0] == "rollback":
        rollback(*args[1:])
    else:
        fail("usage:\n" + "\n".join(__doc__.strip().splitlines()[2:6]))


if __name__ == "__main__":
    main()
