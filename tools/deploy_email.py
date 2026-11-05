#!/usr/bin/env python3
"""Deploy the daily install email Worker (workers/install-email/) to Cloudflare.

    tools/deploy_email.py deploy       upload origin/master's workers/install-email/ (checked out clean, pushed)
    tools/deploy_email.py to ADDRESS   set the one recipient, the TO secret (kept by Cloudflare across deploys)

Same rules as a production site deploy (tools/deploy_site.py refusal): only
origin/master's commit, from a clean tree. The Worker version carries the commit
(wrangler --message), and each deploy is a line in the shared deploy log with
channel "install-email", so "which commit is live?" has an answer. The token
needs Account > Workers Scripts > Edit on top of the site's permissions.
"""
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from deploy_site import DEPLOYS, ROOT, WRANGLER, append_log, git, load_credentials, refusal  # noqa: E402

CONFIG = ROOT / "workers/install-email/wrangler.toml"


def fail(msg):
    sys.exit(f"deploy_email: {msg}")


def wrangler(env, *args, **kw):
    return subprocess.run(["npx", "--yes", WRANGLER, *args, "--config", str(CONFIG)], cwd=ROOT, env=env, **kw).returncode


def email_refusal(problem):
    """Production's refusal, said of the install email, without the advice to deploy the site to preview."""
    return problem.replace(", or deploy to preview", "").replace("production deploys", "the install email deploys")


def deploy():
    git("fetch", "-q", "origin")
    head = git("rev-parse", "HEAD")
    problem = refusal("production", head, git("rev-parse", "origin/master"), bool(git("branch", "-r", "--contains", head)),
                      bool(git("status", "--porcelain", "--untracked-files=no")))
    if problem:
        fail(email_refusal(problem))
    env = load_credentials()
    if wrangler(env, "deploy", "--message", f"commit {head}"):
        fail("wrangler deploy failed; nothing logged")
    append_log(DEPLOYS / "deploys.jsonl", dict(action="deploy", channel="install-email", commit=head,
               time=datetime.now(timezone.utc).isoformat(timespec="seconds"),
               session=os.environ.get("CLAUDE_CODE_SESSION_ID", "unknown")))
    print(f"deployed {head[:7]} as outrigger-install-email")


def set_to(address):
    if "@" not in address or any(c.isspace() for c in address):
        fail(f"{address!r} isn't an email address")
    if wrangler(load_credentials(), "secret", "put", "TO", input=address, text=True):
        fail("wrangler secret put failed")
    print("recipient set (it must be a confirmed destination address in Cloudflare Email Routing)")


def main():
    args = sys.argv[1:]
    if args == ["deploy"]:
        deploy()
    elif len(args) == 2 and args[0] == "to":
        set_to(args[1])
    else:
        fail(__doc__.strip().splitlines()[2].strip())


if __name__ == "__main__":
    main()
