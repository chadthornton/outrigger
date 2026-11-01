#!/usr/bin/env python3
"""make check: the worktree's last commit, checked out clean in one clone shared by every worktree.

    tools/check.py [--home DIR] [--key KEY] [--cmd CMD]

The merge gate's step 2 (README.md). Every worktree of this repo checks in the
same clone, ~/Library/Caches/outrigger-check/clone, one check at a time. A
worktree is a folder for editing; a check edits nothing, it only asks whether a
commit passes, so one clone at one path serves them all, and because the path
never changes arduino-cli's compiled files stay valid from branch to branch.

  1. The commit is read once, at the start: a commit made during the run is not
     credited with its pass.
  2. The clone is reset to that commit (checkout -f, then clean -ffdx removes
     every untracked and ignored file), so nothing uncommitted can help, and git
     rewrites only the files that differ, so only those recompile.
  3. The compiled objects live in <home>/cache/<key>, outside the clone. The key
     hashes arduino-cli, the ESP32 core and the Arduino libraries, so updating
     any of them starts a fresh cache instead of a stale pass.
  4. make test, ui-shots and site run in the clone; the log is copied to the
     worktree's build/check.log.
  5. On a pass, the site and its commit are copied to the worktree's build/checked/
     while the clone is still this check's, for make deploy-site to upload.

A second check waits for the first (flock on <home>/lock, held by check.py and
every process of its build; the kernel releases it when the last one exits) and
says whose check it is waiting for.
--key and --cmd are for tests/test_check.py.
"""
import argparse
import fcntl
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

HOME = Path.home() / "Library/Caches/outrigger-check"
CMD = "make -C {clone} test ui-shots site CACHE_ROOT={cache}"
SUMMARY = re.compile(r"passed|keeps the install|Sketch uses")


def fail(msg):
    sys.exit(f"check: {msg}")


def git(*args, cwd):
    run = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if run.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {run.stderr.strip()}")
    return run.stdout.strip()


def remove(path):
    """rm -rf, after deleting the .DS_Store files Finder can write while it runs
    ("Directory not empty"); tries once more."""
    for attempt in range(2):
        if not path.exists() and not path.is_symlink():
            return
        for ds in path.rglob(".DS_Store") if path.is_dir() else ():
            ds.unlink(missing_ok=True)
        try:
            shutil.rmtree(path) if path.is_dir() and not path.is_symlink() else path.unlink()
            return
        except OSError:
            if attempt:
                raise
            time.sleep(1)


def cache_key():
    out = b""
    for args in (["version"], ["core", "list"], ["lib", "list"]):
        run = subprocess.run(["arduino-cli", *args], capture_output=True)
        if run.returncode:
            fail(f"arduino-cli {' '.join(args)} failed: {run.stderr.decode().strip()}")
        out += run.stdout
    return hashlib.sha1(out).hexdigest()[:12]


def lock(home, worktree, sha):
    """Take the queue's lock (blocking) and record who holds it."""
    home.mkdir(parents=True, exist_ok=True)
    handle = open(home / "lock", "a")
    holder = home / "holder.json"
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        try:
            other = json.loads(holder.read_text())
            who = f"{other['commit'][:7]} from {other['worktree']} (pid {other['pid']}, since {other['started']})"
        except (OSError, ValueError, KeyError):
            who = "another worktree"
        print(f"check: waiting for the check of {who}", flush=True)
        fcntl.flock(handle, fcntl.LOCK_EX)
    holder.write_text(json.dumps({"pid": os.getpid(), "worktree": str(worktree), "commit": sha,
                                  "started": time.strftime("%H:%M:%S")}))
    return handle


def reset_clone(clone, source, sha):
    """The clone at sha with nothing else in it; a fresh clone if it can't be reused."""
    try:
        if git("rev-parse", "--show-toplevel", cwd=clone) != str(clone) \
                or git("config", "remote.origin.url", cwd=clone) != str(source):
            raise RuntimeError("not a clone of this repo")
        git("checkout", "-q", "-f", "--detach", sha, cwd=clone)
        git("clean", "-q", "-ffdx", cwd=clone)
        print(f"check: reusing {clone}", flush=True)
    except (RuntimeError, OSError):
        print(f"check: fresh clone into {clone}", flush=True)
        try:
            remove(clone)
        except OSError as e:
            raise RuntimeError(f"can't remove the old clone ({e}); close what has it open and run make check again")
        git("clone", "-q", "--shared", str(source), str(clone), cwd=clone.parent)
        git("checkout", "-q", "--detach", sha, cwd=clone)
    extra = [line for line in git("status", "--porcelain", "--ignored", cwd=clone).splitlines()
             if not line.endswith(".DS_Store")]
    if git("rev-parse", "HEAD", cwd=clone) != sha or extra:
        fail(f"the clone isn't clean at {sha[:7]}: {extra[:5]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--home", type=Path, default=HOME)
    ap.add_argument("--key")
    ap.add_argument("--cmd", default=CMD)
    args = ap.parse_args()
    home = args.home.resolve()

    worktree = Path(git("rev-parse", "--show-toplevel", cwd=Path.cwd()))
    source = Path(git("rev-parse", "--path-format=absolute", "--git-common-dir", cwd=worktree))
    sha = git("rev-parse", "HEAD", cwd=worktree)
    short = git("rev-parse", "--short", sha, cwd=worktree)
    build = worktree / "build"
    log, checked = build / "check.log", build / "checked"
    print(f"check: testing {short}", flush=True)
    # No earlier pass may stay deployable while this one runs; the per-worktree
    # clone and cache that make check used before the shared one go too.
    for old in (checked, log, build / "check", build / "check-cache"):
        remove(old)
    key = args.key or cache_key()

    handle = lock(home, worktree, sha)
    try:
        for old in (checked, log):  # again: a check from this worktree may have finished while this one waited
            remove(old)
        clone = home / "clone"
        try:
            reset_clone(clone, source, sha)
        except RuntimeError as e:
            fail(str(e))
        fixtures = worktree / "tests/fixtures"
        if fixtures.is_dir():
            shutil.copytree(fixtures, clone / "tests/fixtures", dirs_exist_ok=True)
        caches = home / "cache"
        caches.mkdir(exist_ok=True)
        for old in caches.iterdir():
            if old.name != key:
                remove(old)
        cache = caches / key
        print(f"check: cache {cache}", flush=True)
        cmd = args.cmd.format(clone=shlex.quote(str(clone)), cache=shlex.quote(str(cache)))
        with open(home / "check.log", "w") as out:
            # The build inherits the lock, so it stays held until every process of
            # this check exits, even if check.py itself is killed: an orphaned
            # compile can't write into the clone or cache under the next check.
            rc = subprocess.run(shlex.split(cmd), stdout=out, stderr=subprocess.STDOUT,
                                pass_fds=(handle.fileno(),)).returncode
        build.mkdir(exist_ok=True)
        shutil.copyfile(home / "check.log", log)
        lines = log.read_text(errors="replace").splitlines()
        if rc:
            print("\n".join(lines[-30:]))
            print(f"check FAILED at {short}; log: {log.relative_to(worktree)}")
            return 1
        if (clone / "site").is_dir():
            shutil.copytree(clone / "site", checked / "site", symlinks=True)
            (checked / "commit").write_text(sha + "\n")
        print("\n".join(line for line in lines if SUMMARY.search(line)))
        print(f"check passed at {short} (clean checkout; log: {log.relative_to(worktree)})")
        return 0
    finally:
        (home / "holder.json").unlink(missing_ok=True)
        handle.close()


if __name__ == "__main__":
    sys.exit(main())
