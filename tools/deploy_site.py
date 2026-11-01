#!/usr/bin/env python3
"""Publish the installer site on Cloudflare Pages (outrigger.music).

    tools/deploy_site.py guard <channel>             refuse early, before make check spends minutes
    tools/deploy_site.py deploy <channel> <site dir>  stage, check and upload a site make check built
                                                     (build/checked/site; its commit in build/checked/commit)

Channels: production (outrigger.music, from origin/master only) and beta
(beta.outrigger.music, any pushed commit). `make deploy-site` runs both steps
around `make check`, so only a site built from a clean checkout of a committed,
pushed HEAD goes live. Each upload carries its commit, so the Pages dashboard
answers "which commit is live?".

Credentials: ~/.config/cloudflare/outrigger-pages.env (CLOUDFLARE_API_TOKEN with
Account > Cloudflare Pages > Edit, and CLOUDFLARE_ACCOUNT_ID). Never printed.
"""
import json
import os
import shutil
import subprocess
import sys
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
STAGE = ROOT / "build/pages"
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


def deploy(channel, site):
    head = guard(channel)
    problem = built_from_problem(site, head)
    if problem:
        fail(problem)
    stage(site, STAGE)
    problems = site_problems(STAGE, load_wl82_blob())
    if problems:
        fail("refusing to upload:\n  " + "\n  ".join(problems))
    env = load_credentials()
    upload = subprocess.run(["npx", "--yes", WRANGLER, "pages", "deploy", str(STAGE),
                    "--project-name", PROJECT, "--branch", BRANCHES[channel],
                    "--commit-hash", head, "--commit-message", git("log", "-1", "--format=%s", head),
                    "--commit-dirty=false"], env=env)
    if upload.returncode:
        fail(f"wrangler failed (exit {upload.returncode}); its output is above")
    url = "https://outrigger.music" if channel == "production" else "https://beta.outrigger.music"
    print(f"deployed {head[:7]} to {channel}: {url}")


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "guard":
        guard(args[1])
    elif len(args) == 3 and args[0] == "deploy":
        deploy(args[1], Path(args[2]).resolve())
    else:
        fail(__doc__.strip().splitlines()[2].strip())


if __name__ == "__main__":
    main()
