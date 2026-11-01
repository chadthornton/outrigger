"""Tests for tools/check.py: one shared clone checks every worktree's commits, one at a time.

    python3 -I tests/test_check.py

Each test builds a throwaway repo with two worktrees and runs check.py in a
subprocess, with a stand-in for `make test ui-shots site` (--cmd) and a fixed
cache key (--key), so no arduino-cli is needed and a run takes well under a second.
"""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

CHECK = Path(__file__).resolve().parent.parent / "tools" / "check.py"

# The stand-in for make: logs a pass line, writes site/index.html, and appends
# start and end times to $CHECK_TIMES (for the one-at-a-time test). FAIL in the
# commit's tree makes it fail; SLEEP holds it for that long.
FAKE_MAKE = r"""
import os, sys, time
from pathlib import Path
clone, cache = Path(sys.argv[1]), Path(sys.argv[2])
times = os.environ.get("CHECK_TIMES")
if times:
    with open(times, "a") as f: f.write(f"start {time.time()}\n")
time.sleep(float(os.environ.get("SLEEP", "0")))
cache.mkdir(parents=True, exist_ok=True)
(cache / "obj").write_text("compiled")
print("all fake tests passed")
if (clone / "FAIL").exists():
    print("error: FAIL is in the tree"); sys.exit(2)
(clone / "site").mkdir(exist_ok=True)
(clone / "site" / "index.html").write_text((clone / "VERSION").read_text())
if times:
    with open(times, "a") as f: f.write(f"end {time.time()}\n")
"""


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


class CheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        tmp = Path(self.tmp.name).resolve()
        self.home = tmp / "check-home"
        self.fake = tmp / "fake_make.py"
        self.fake.write_text(FAKE_MAKE)
        self.repo = tmp / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q", "-b", "master")
        git(self.repo, "config", "user.email", "t@example.com")
        git(self.repo, "config", "user.name", "t")
        (self.repo / ".gitignore").write_text("build/\ntests/fixtures/\n")
        self.commit(self.repo, "VERSION", "one")
        self.other = tmp / "other"
        git(self.repo, "worktree", "add", "-q", "-b", "other", str(self.other))

    def tearDown(self):
        self.tmp.cleanup()

    def commit(self, tree, name, text):
        (tree / name).write_text(text)
        git(tree, "add", "-A")
        git(tree, "commit", "-q", "-m", name)
        return git(tree, "rev-parse", "HEAD")

    def start(self, tree, key="k1", **env):
        cmd = [sys.executable, "-I", str(CHECK), "--home", str(self.home), "--key", key,
               "--cmd", f"{sys.executable} -I {self.fake} {{clone}} {{cache}}"]
        return subprocess.Popen(cmd, cwd=tree, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True,
                                env=dict(os.environ, **{k: str(v) for k, v in env.items()}))

    def run_check(self, tree, **kw):
        proc = self.start(tree, **kw)
        out = proc.communicate(timeout=60)[0]
        return proc.returncode, out

    def test_pass_names_the_commit_and_hands_the_site_to_the_worktree(self):
        sha = git(self.repo, "rev-parse", "HEAD")
        rc, out = self.run_check(self.repo)
        self.assertEqual(rc, 0, out)
        self.assertIn(f"check passed at {sha[:7]}", out)
        self.assertIn("all fake tests passed", out)
        checked = self.repo / "build" / "checked"
        self.assertEqual((checked / "commit").read_text().strip(), sha)
        self.assertEqual((checked / "site" / "index.html").read_text(), "one")
        self.assertIn("all fake tests passed", (self.repo / "build" / "check.log").read_text())

    def test_failure_names_the_commit_and_leaves_no_site_to_deploy(self):
        self.assertEqual(self.run_check(self.repo)[0], 0)
        sha = self.commit(self.repo, "FAIL", "x")
        rc, out = self.run_check(self.repo)
        self.assertNotEqual(rc, 0)
        self.assertIn(f"check FAILED at {sha[:7]}", out)
        self.assertIn("error: FAIL is in the tree", out)
        self.assertFalse((self.repo / "build" / "checked").exists())

    def test_worktrees_share_one_clone_and_cache(self):
        self.assertEqual(self.run_check(self.repo)[0], 0)
        clone_git = (self.home / "clone" / ".git").stat().st_ino
        sha = self.commit(self.other, "VERSION", "two")
        rc, out = self.run_check(self.other)
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.home / "clone" / ".git").stat().st_ino, clone_git, "the clone was made again")
        self.assertEqual((self.other / "build" / "checked" / "commit").read_text().strip(), sha)
        self.assertEqual((self.other / "build" / "checked" / "site" / "index.html").read_text(), "two")
        self.assertTrue((self.home / "cache" / "k1" / "obj").exists())

    def test_a_new_key_starts_a_fresh_cache_and_drops_the_old(self):
        self.assertEqual(self.run_check(self.repo, key="k1")[0], 0)
        self.assertEqual(self.run_check(self.repo, key="k2")[0], 0)
        self.assertEqual(sorted(p.name for p in (self.home / "cache").iterdir()), ["k2"])

    def test_leftovers_in_the_clone_are_removed_before_the_run(self):
        self.assertEqual(self.run_check(self.repo)[0], 0)
        clone = self.home / "clone"
        (clone / "VERSION").write_text("edited")
        (clone / "stray.txt").write_text("untracked")
        (clone / "build").mkdir(exist_ok=True)
        (clone / "build" / "stale.o").write_text("ignored")
        rc, out = self.run_check(self.repo)
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.repo / "build" / "checked" / "site" / "index.html").read_text(), "one")
        self.assertFalse((clone / "stray.txt").exists())
        self.assertFalse((clone / "build" / "stale.o").exists())

    def test_fixtures_are_copied_in(self):
        (self.repo / "tests" / "fixtures").mkdir(parents=True)
        (self.repo / "tests" / "fixtures" / "pkg.fwsc").write_text("pinned")
        self.assertEqual(self.run_check(self.repo)[0], 0)
        self.assertEqual((self.home / "clone" / "tests" / "fixtures" / "pkg.fwsc").read_text(), "pinned")

    def test_a_clone_of_another_repo_is_replaced(self):
        stranger = Path(self.tmp.name).resolve() / "stranger"
        stranger.mkdir()
        git(stranger, "init", "-q")
        self.home.mkdir()
        git(self.home, "clone", "-q", str(stranger), "clone")
        rc, out = self.run_check(self.repo)
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.repo / "build" / "checked" / "site" / "index.html").read_text(), "one")

    def test_a_commit_made_during_the_run_is_not_credited(self):
        sha = git(self.repo, "rev-parse", "HEAD")
        proc = self.start(self.repo, SLEEP=1.5)
        time.sleep(0.7)
        self.commit(self.repo, "VERSION", "later")
        out = proc.communicate(timeout=60)[0]
        self.assertEqual(proc.returncode, 0, out)
        self.assertIn(f"check passed at {sha[:7]}", out)
        self.assertEqual((self.repo / "build" / "checked" / "commit").read_text().strip(), sha)
        self.assertEqual((self.repo / "build" / "checked" / "site" / "index.html").read_text(), "one")

    def test_checks_from_two_worktrees_take_turns(self):
        times = Path(self.tmp.name).resolve() / "times"
        self.commit(self.other, "VERSION", "two")
        first = self.start(self.repo, SLEEP=1, CHECK_TIMES=times)
        time.sleep(0.3)
        second = self.start(self.other, SLEEP=1, CHECK_TIMES=times)
        out1, out2 = first.communicate(timeout=60)[0], second.communicate(timeout=60)[0]
        self.assertEqual((first.returncode, second.returncode), (0, 0), out1 + out2)
        events = [line.split()[0] for line in times.read_text().splitlines()]
        self.assertEqual(events, ["start", "end", "start", "end"], "the two checks overlapped")
        self.assertIn("waiting", out2)
        self.assertIn(str(self.repo), out2)
        self.assertEqual((self.repo / "build" / "checked" / "site" / "index.html").read_text(), "one")
        self.assertEqual((self.other / "build" / "checked" / "site" / "index.html").read_text(), "two")

    def wait_for_holder(self):
        deadline = time.time() + 10
        while not (self.home / "holder.json").exists() and time.time() < deadline:
            time.sleep(0.05)

    def test_a_killed_check_does_not_hold_the_queue(self):
        proc = self.start(self.repo, SLEEP=30)
        self.wait_for_holder()
        os.killpg(proc.pid, signal.SIGKILL)  # what Ctrl-C or a tool timeout does: the whole check
        proc.communicate()
        start = time.time()
        rc, out = self.run_check(self.other)
        self.assertEqual(rc, 0, out)
        self.assertLess(time.time() - start, 10)

    def test_a_build_orphaned_by_a_killed_check_py_still_holds_the_queue(self):
        times = Path(self.tmp.name).resolve() / "times"
        proc = self.start(self.repo, SLEEP=2, CHECK_TIMES=times)
        self.wait_for_holder()
        time.sleep(0.3)
        proc.kill()  # check.py only; its build runs on
        proc.communicate()
        rc, out = self.run_check(self.other, CHECK_TIMES=times)
        self.assertEqual(rc, 0, out)
        self.assertIn("waiting", out)
        events = [line.split()[0] for line in times.read_text().splitlines()]
        self.assertEqual(events, ["start", "end", "start", "end"], "the next check ran beside the orphaned build")

    def test_holder_names_the_running_check(self):
        proc = self.start(self.repo, SLEEP=1)
        self.wait_for_holder()
        holder = json.loads((self.home / "holder.json").read_text())
        proc.communicate(timeout=60)
        self.assertEqual(holder["worktree"], str(self.repo))
        self.assertEqual(holder["commit"], git(self.repo, "rev-parse", "HEAD"))


if __name__ == "__main__":
    unittest.main()
