"""Tests for tools/deploy_site.py: which commits may go live, and what may be uploaded.

    python3 -I tests/test_deploy_site.py
"""
import contextlib
import hashlib
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import deploy_site  # noqa: E402

HEAD = "a" * 40
OTHER = "b" * 40


class RefusalTest(unittest.TestCase):
    def refusal(self, channel, head=HEAD, master=HEAD, pushed=True, dirty=False):
        return deploy_site.refusal(channel, head, master, pushed, dirty)

    def test_production_from_origin_master(self):
        self.assertIsNone(self.refusal("production"))

    def test_production_refuses_a_commit_not_on_origin_master(self):
        self.assertIn("origin/master", self.refusal("production", master=OTHER))

    def test_preview_takes_any_pushed_commit(self):
        self.assertIsNone(self.refusal("preview", master=OTHER))

    def test_preview_refuses_an_unpushed_commit(self):
        self.assertIn("push", self.refusal("preview", master=OTHER, pushed=False))

    def test_dirty_tree_refused_on_both_channels(self):
        for channel in ("production", "preview"):
            self.assertIn("uncommitted", self.refusal(channel, dirty=True))

    def test_unknown_channel_refused(self):
        self.assertIn("channel", self.refusal("staging"))


class BuiltFromTest(unittest.TestCase):
    """make check leaves build/checked/{site,commit}; only HEAD's site may go up."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.site = Path(self.tmp.name) / "site"
        self.site.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_site_built_from_head_passes(self):
        (self.site.parent / "commit").write_text(HEAD + "\n")
        self.assertIsNone(deploy_site.built_from_problem(self.site, HEAD))

    def test_site_from_another_commit_refused(self):
        (self.site.parent / "commit").write_text(OTHER + "\n")
        self.assertIn("make check again", deploy_site.built_from_problem(self.site, HEAD))

    def test_site_with_no_commit_refused(self):
        self.assertIn("make check", deploy_site.built_from_problem(self.site, HEAD))


class SiteProblemsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.site = Path(self.tmp.name)
        (self.site / "index.html").write_text("<!doctype html>")
        for app_set in deploy_site.APP_SETS:
            out = self.site / "firmware" / "board" / app_set
            out.mkdir(parents=True)
            (out / "app.bin").write_bytes(b"\x01" * 4096)
            (out / "manifest.json").write_text('{"builds": [{"parts": [{"path": "app.bin", "offset": 65536}]}]}')
        self.app = self.site / "firmware" / "board" / "hub" / "app.bin"
        self.blob = bytes(range(256)) * 8  # 2 KiB stand-in for wl82loader.bin

    def tearDown(self):
        self.tmp.cleanup()

    def problems(self, **kw):
        return deploy_site.site_problems(self.site, kw.pop("blob", self.blob), **kw)

    def test_a_plain_site_passes(self):
        self.assertEqual(self.problems(), [])

    def test_missing_index_refused(self):
        (self.site / "index.html").unlink()
        self.assertTrue(any("index.html" in p for p in self.problems()))

    def test_missing_firmware_refused(self):
        shutil.rmtree(self.site / "firmware")
        self.assertTrue(any("firmware" in p for p in self.problems()))

    def test_manifest_missing_a_part_refused(self):
        self.app.unlink()
        self.assertTrue(any("app.bin" in p for p in self.problems()))

    def test_missing_app_set_refused(self):
        shutil.rmtree(self.site / "firmware" / "board" / "everything")
        self.assertTrue(any("everything" in p for p in self.problems()))

    def test_file_over_the_per_file_limit_refused(self):
        self.app.write_bytes(b"\0" * 2048)
        self.assertTrue(any("app.bin" in p for p in self.problems(max_file=1024)))

    def test_too_many_files_refused(self):
        self.assertTrue(any("files" in p for p in self.problems(max_files=3)))

    def test_file_with_the_blob_sha_refused(self):
        sha = hashlib.sha256(self.blob).hexdigest()
        (self.site / "firmware" / "x.bin").write_bytes(self.blob)
        self.assertTrue(any("x.bin" in p for p in deploy_site.site_problems(self.site, None, sha=sha)))

    def test_file_containing_a_piece_of_the_blob_refused(self):
        self.app.write_bytes(b"\x01" * 100 + self.blob[512:1536] + b"\x02" * 100)
        self.assertTrue(any("app.bin" in p for p in self.problems()))

    def test_file_named_like_the_blob_refused(self):
        (self.site / "firmware" / "WL82Loader.bin").write_bytes(b"\x03")
        self.assertTrue(any("WL82Loader.bin" in p for p in self.problems()))

    def test_no_local_blob_refused(self):
        self.assertTrue(any("wl82loader.bin" in p for p in deploy_site.site_problems(self.site, None)))


class CredentialsTest(unittest.TestCase):
    def test_reads_quoted_and_exported_values(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "pages.env")
            path.write_text('# comment\nexport CLOUDFLARE_API_TOKEN="tok"\nCLOUDFLARE_ACCOUNT_ID=acct\n')
            env = deploy_site.read_env(path)
            self.assertEqual((env["CLOUDFLARE_API_TOKEN"], env["CLOUDFLARE_ACCOUNT_ID"]), ("tok", "acct"))


class StageTest(unittest.TestCase):
    def test_stage_copies_the_site_without_dotfiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp, "site"), Path(tmp, "out")
            (src / "vendor").mkdir(parents=True)
            (src / "index.html").write_text("page")
            (src / "vendor" / "lib.js").write_text("js")
            (src / ".DS_Store").write_text("junk")
            (src / "vendor" / ".DS_Store").write_text("junk")
            dst.mkdir()
            (dst / "stale.txt").write_text("from the last deploy")
            deploy_site.stage(src, dst)
            files = sorted(str(p.relative_to(dst)) for p in dst.rglob("*") if p.is_file())
            self.assertEqual(files, ["index.html", "vendor/lib.js"])

    def test_stage_root_lays_out_site_functions_and_config_as_in_the_repo(self):
        with tempfile.TemporaryDirectory() as tmp:
            checked, dst = Path(tmp, "checked"), Path(tmp, "pages")
            (checked / "site").mkdir(parents=True)
            (checked / "site" / "index.html").write_text("page")
            (checked / "functions" / "api").mkdir(parents=True)
            (checked / "functions" / "api" / "report.js").write_text("fn")
            (checked / "wrangler.toml").write_text("toml")
            (checked / "commit").write_text("abc")
            self.assertEqual(deploy_site.stage_root(checked, dst), [])
            files = sorted(str(p.relative_to(dst)) for p in dst.rglob("*") if p.is_file())
            self.assertEqual(files, ["functions/api/report.js", "site/index.html", "wrangler.toml"])

    def test_stage_root_names_missing_functions_and_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            checked = Path(tmp, "checked")
            (checked / "site").mkdir(parents=True)
            missing = deploy_site.stage_root(checked, Path(tmp, "pages"))
            self.assertEqual(len(missing), 2)
            self.assertIn("functions/", missing[0])
            self.assertIn("wrangler.toml", missing[1])


def entry(action, commit, channel="production", deployment=None, **kw):
    return dict(action=action, channel=channel, commit=commit * 40 if len(commit) == 1 else commit,
                deployment=deployment or f"dep-{commit}", time="2026-11-03T00:00:00Z", session="s", **kw)


class RollbackTargetTest(unittest.TestCase):
    """The deploy log's live history per channel, and which entry a rollback goes back to."""
    def target(self, entries, live, channel="production", commit=None):
        return deploy_site.rollback_target(entries, channel, live * 40 if len(live) == 1 else live, commit)

    def test_goes_back_to_the_deploy_before_the_live_one(self):
        log = [entry("deploy", "a"), entry("deploy", "b")]
        target, problem = self.target(log, "b")
        self.assertIsNone(problem)
        self.assertEqual(target["deployment"], "dep-a")

    def test_a_baseline_counts_as_a_previous_deployment(self):
        log = [entry("baseline", "a"), entry("deploy", "b")]
        self.assertEqual(self.target(log, "b")[0]["commit"], "a" * 40)

    def test_no_previous_deployment_refused(self):
        self.assertIn("no previous", self.target([entry("deploy", "a")], "a")[1])
        self.assertIn("no previous", self.target([], "a")[1])

    def test_other_channels_deploys_do_not_count(self):
        log = [entry("deploy", "a", channel="preview"), entry("deploy", "b")]
        self.assertIn("no previous", self.target(log, "b")[1])

    def test_unknown_channel_refused(self):
        self.assertIn("channel", self.target([entry("deploy", "a"), entry("deploy", "b")], "b", channel="staging")[1])

    def test_a_second_rollback_goes_further_back_not_forward_to_the_bad_build(self):
        log = [entry("deploy", "a"), entry("deploy", "b"), entry("deploy", "c"),
               entry("rollback", "b", **{"from": "c" * 40})]
        self.assertEqual(self.target(log, "b")[0]["commit"], "a" * 40)

    def test_a_deploy_after_a_rollback_rolls_back_to_the_good_build(self):
        log = [entry("deploy", "a"), entry("deploy", "b"), entry("rollback", "a", **{"from": "b" * 40}),
               entry("deploy", "c")]
        self.assertEqual(self.target(log, "c")[0]["commit"], "a" * 40)

    def test_redeploying_the_live_commit_does_not_make_it_its_own_previous(self):
        log = [entry("deploy", "a"), entry("deploy", "b"), entry("deploy", "b", deployment="dep-b2")]
        self.assertEqual(self.target(log, "b")[0]["commit"], "a" * 40)

    def test_live_commit_that_is_not_the_logs_last_refused(self):
        """Someone deployed around the log (the dashboard, an old checkout): don't guess."""
        log = [entry("deploy", "a"), entry("deploy", "b")]
        self.assertIn("deploy log", self.target(log, "c")[1])

    def test_named_commit_rolls_back_past_several(self):
        log = [entry("deploy", "a"), entry("deploy", "b"), entry("deploy", "c")]
        self.assertEqual(self.target(log, "c", commit="aaaaaaa")[0]["deployment"], "dep-a")

    def test_unknown_commit_refused(self):
        log = [entry("deploy", "a"), entry("deploy", "b")]
        self.assertIn("unknown commit", self.target(log, "b", commit="ddddddd")[1])

    def test_rolled_away_from_commit_is_no_longer_a_target(self):
        log = [entry("deploy", "a"), entry("deploy", "b"), entry("deploy", "c"),
               entry("rollback", "a", **{"from": "c" * 40})]
        self.assertIn("no previous", self.target(log, "a")[1])
        self.assertIn("unknown commit", self.target(log, "a", commit="bbbbbbb")[1])

    def test_naming_the_live_commit_refused(self):
        log = [entry("deploy", "a"), entry("deploy", "b")]
        self.assertIn("already live", self.target(log, "b", commit="bbbbbbb")[1])

    def test_too_short_a_commit_refused(self):
        log = [entry("deploy", "a"), entry("deploy", "b")]
        self.assertIn("unknown commit", self.target(log, "b", commit="a")[1])


class LiveFirmwareTest(unittest.TestCase):
    """The firmware commit a channel serves: a pinned deploy's pages are newer than its firmware."""
    def test_a_deploy_from_before_pinning_serves_its_own_firmware(self):
        self.assertEqual(deploy_site.live_firmware([entry("deploy", "a")], "production", "a" * 40), "a" * 40)

    def test_a_pinned_deploy_serves_the_firmware_it_pinned(self):
        log = [entry("deploy", "a"), entry("deploy", "b", firmware="a" * 40)]
        self.assertEqual(deploy_site.live_firmware(log, "production", "b" * 40), "a" * 40)

    def test_a_rollback_brings_back_its_targets_firmware(self):
        log = [entry("deploy", "a"), entry("deploy", "b", firmware="a" * 40), entry("deploy", "c"),
               entry("rollback", "b", firmware="a" * 40, **{"from": "c" * 40})]
        self.assertEqual(deploy_site.live_firmware(log, "production", "b" * 40), "a" * 40)

    def test_a_live_commit_the_log_does_not_know_serves_its_own_firmware(self):
        log = [entry("deploy", "b", firmware="a" * 40)]
        self.assertEqual(deploy_site.live_firmware(log, "production", "z" * 40), "z" * 40)

    def test_the_same_pages_with_other_firmware_are_another_deploy(self):
        """A rollback from b over firmware b goes back to b over firmware a, not past it."""
        log = [entry("deploy", "a"), entry("deploy", "b", firmware="a" * 40),
               entry("deploy", "b", deployment="dep-b2", firmware="b" * 40)]
        target = deploy_site.rollback_target(log, "production", "b" * 40)[0]
        self.assertEqual((target["deployment"], target["firmware"]), ("dep-b", "a" * 40))


class BaselineTest(unittest.TestCase):
    """Before each deploy, the log notes what is live if it doesn't already know it."""
    def test_empty_log_needs_a_baseline(self):
        self.assertTrue(deploy_site.needs_baseline([], "production", "a" * 40))

    def test_log_that_knows_the_live_commit_needs_none(self):
        self.assertFalse(deploy_site.needs_baseline([entry("deploy", "a")], "production", "a" * 40))

    def test_deploy_around_the_log_gets_a_baseline(self):
        """A dashboard rollback to z: a rollback after the next deploy must come back to z, not a."""
        log = [entry("deploy", "a")]
        self.assertTrue(deploy_site.needs_baseline(log, "production", "z" * 40))
        log += [entry("baseline", "z"), entry("deploy", "c")]
        self.assertEqual(deploy_site.rollback_target(log, "production", "c" * 40)[0]["commit"], "z" * 40)


class PreviewFirstTest(unittest.TestCase):
    def test_production_needs_preview_on_the_same_commit(self):
        self.assertIsNone(deploy_site.preview_first_problem("production", HEAD, HEAD))
        self.assertIn("preview", deploy_site.preview_first_problem("production", HEAD, OTHER))

    def test_preview_itself_needs_nothing(self):
        self.assertIsNone(deploy_site.preview_first_problem("preview", HEAD, OTHER))


class DeployLogTest(unittest.TestCase):
    def test_appends_and_reads_back_in_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp, "sub", "deploys.jsonl")
            deploy_site.append_log(log, entry("deploy", "a"))
            deploy_site.append_log(log, entry("deploy", "b"))
            self.assertEqual([e["commit"][0] for e in deploy_site.read_log(log)], ["a", "b"])

    def test_a_missing_log_reads_empty(self):
        self.assertEqual(deploy_site.read_log(Path(tempfile.gettempdir(), "no-such-deploys.jsonl")), [])


class KeptCopyTest(unittest.TestCase):
    """Preview can't use Cloudflare's rollback, so each deploy keeps what it uploaded, with SHA-256 sums."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.staged = self.root / "pages"
        (self.staged / "site").mkdir(parents=True)
        (self.staged / "site" / "index.html").write_text("page")
        (self.staged / "functions" / "api").mkdir(parents=True)
        (self.staged / "functions" / "api" / "report.js").write_text("fn")
        (self.staged / "wrangler.toml").write_text("toml")

    def tearDown(self):
        self.tmp.cleanup()

    def keep(self, commit):
        return deploy_site.keep(self.staged, self.root / "kept" / "preview", commit * 40)

    def test_kept_copy_checks_out(self):
        kept = self.keep("a")
        self.assertEqual(deploy_site.kept_problems(kept), [])
        self.assertTrue((kept / "functions" / "api" / "report.js").is_file())
        self.assertTrue((kept / "wrangler.toml").is_file())

    def test_changed_file_refused(self):
        kept = self.keep("a")
        (kept / "site" / "index.html").write_text("tampered")
        self.assertTrue(any("index.html" in p for p in deploy_site.kept_problems(kept)))

    def test_missing_file_refused(self):
        kept = self.keep("a")
        (kept / "wrangler.toml").unlink()
        self.assertTrue(any("wrangler.toml" in p for p in deploy_site.kept_problems(kept)))

    def test_extra_file_refused(self):
        kept = self.keep("a")
        (kept / "site" / "extra.js").write_text("x")
        self.assertTrue(any("extra.js" in p for p in deploy_site.kept_problems(kept)))

    def test_no_kept_copy_refused(self):
        self.assertTrue(deploy_site.kept_problems(self.root / "kept" / "preview" / ("c" * 40)))

    def test_wranglers_cache_is_neither_kept_nor_checked(self):
        """wrangler writes .wrangler/ into the folder it uploads from, and again on a rollback upload."""
        (self.staged / ".wrangler" / "tmp").mkdir(parents=True)
        (self.staged / ".wrangler" / "tmp" / "bundle.js").write_text("cache")
        kept = self.keep("a")
        self.assertFalse((kept / ".wrangler").exists())
        (kept / ".wrangler" / "cache").mkdir(parents=True)
        (kept / ".wrangler" / "cache" / "x.json").write_text("written by the rollback upload")
        self.assertEqual(deploy_site.kept_problems(kept), [])

    def test_sums_naming_wranglers_cache_still_check_out(self):
        """Copies kept before the cache was left out list .wrangler/ files in their sums."""
        kept = self.keep("a")
        sums = json.loads((kept / deploy_site.SUMS).read_text())
        sums["files"][".wrangler/tmp/bundle.js"] = "0" * 64
        (kept / deploy_site.SUMS).write_text(json.dumps(sums))
        self.assertEqual(deploy_site.kept_problems(kept), [])

    def test_keeps_only_the_newest_few(self):
        for c in "abcd":
            self.keep(c)
        kept = sorted(p.name[0] for p in (self.root / "kept" / "preview").iterdir())
        self.assertEqual(kept, ["b", "c", "d"])

    def test_keeping_a_commit_again_replaces_its_copy(self):
        self.keep("a")
        (self.staged / "site" / "index.html").write_text("page 2")
        kept = self.keep("a")
        self.assertEqual((kept / "site" / "index.html").read_text(), "page 2")
        self.assertEqual(deploy_site.kept_problems(kept), [])


def run(head=HEAD, event="push", status="completed", conclusion="success", name="CI", id=7, attempt=1):
    return dict(head_sha=head, event=event, status=status, conclusion=conclusion, name=name, id=id,
                run_attempt=attempt)


def check(conclusion="success", run_id=7):
    return dict(name="ci-ok", status="completed", conclusion=conclusion,
                details_url=f"https://github.com/o/r/actions/runs/{run_id}/job/1")


class CiRunTest(unittest.TestCase):
    """Which CI run's build may go up: a green push run of exactly HEAD, with ci-ok green."""
    def pick(self, runs, checks=None):
        return deploy_site.ci_run(runs, [check()] if checks is None else checks, HEAD)

    def test_green_push_run_of_head(self):
        self.assertEqual(self.pick([run()]), (run(), None))

    def test_run_of_another_commit_is_no_build(self):
        self.assertEqual(self.pick([run(head=OTHER)]), (None, None))

    def test_hand_started_run_of_head_counts(self):
        """gh workflow run CI --ref <branch> builds exactly the branch's commit."""
        self.assertEqual(self.pick([run(event="workflow_dispatch")])[0]["id"], 7)

    def test_pr_run_is_no_build_of_head(self):
        """A pull_request run builds HEAD merged into master, not HEAD."""
        self.assertEqual(self.pick([run(event="pull_request")]), (None, None))

    def test_no_run_is_no_build(self):
        self.assertEqual(self.pick([]), (None, None))

    def test_red_run_refused(self):
        self.assertIn("failure", self.pick([run(conclusion="failure")])[1])

    def test_running_ci_refused(self):
        self.assertIn("still running", self.pick([run(status="in_progress", conclusion=None)])[1])

    def test_red_ci_ok_refused(self):
        self.assertIn("ci-ok", self.pick([run()], [check("failure")])[1])

    def test_missing_ci_ok_refused(self):
        self.assertIn("ci-ok", self.pick([run()], [])[1])

    def test_ci_ok_of_another_run_refused(self):
        self.assertIn("ci-ok", self.pick([run()], [check(run_id=8)])[1])

    def test_newest_run_decides(self):
        """A re-run that went green after a red one; the API lists newest first."""
        self.assertEqual(self.pick([run(id=9), run(id=7, conclusion="failure")], [check(run_id=9)])[0]["id"], 9)

    def test_other_workflows_ignored(self):
        self.assertEqual(self.pick([run(name="Pages", id=3), run()])[0]["id"], 7)


class ChooseSourceTest(unittest.TestCase):
    """Where the site comes from, once CI's run of HEAD is known green (or absent)."""
    def choose(self, channel="production", full="", ran=True, crit=(), preview=None):
        return deploy_site.choose_source(channel, full, run() if ran else None, list(crit), preview)

    def test_production_reuses_previews_checked_build(self):
        """A firmware ship runs make check once, for preview, not again for production."""
        self.assertEqual(self.choose(crit=["firmware/x.cpp"], preview="check")[0], "preview")

    def test_production_reuses_previews_ci_build_of_a_plain_batch(self):
        self.assertEqual(self.choose(preview="ci")[0], "preview")

    def test_critical_batch_does_not_reuse_a_ci_build_on_preview(self):
        self.assertEqual(self.choose(crit=["firmware/x.cpp"], preview="ci")[0], "check")

    def test_full_1_does_not_reuse_preview(self):
        self.assertEqual(self.choose(full="1", preview="check")[0], "check")

    def test_preview_never_reuses_itself(self):
        self.assertEqual(self.choose("preview", crit=["firmware/x.cpp"], preview="check")[0], "check")

    def test_green_ci_build_of_a_plain_batch_goes_up(self):
        self.assertEqual(self.choose()[0], "ci")
        self.assertEqual(self.choose("preview")[0], "ci")

    def test_critical_batch_gets_make_check_on_both_channels(self):
        """Preview then carries the same Mac build production will ship."""
        for channel in ("production", "preview"):
            kind, why = self.choose(channel, crit=["functions/api/report.js"])
            self.assertEqual(kind, "check")
            self.assertIn("functions/api/report.js", why)

    def test_full_1_gets_make_check(self):
        self.assertEqual(self.choose(full="1")[0], "check")

    def test_other_full_values_refused(self):
        self.assertEqual(self.choose(full="no")[0], None)

    def test_production_without_a_ci_run_refused(self):
        kind, why = self.choose(ran=False)
        self.assertIsNone(kind)
        self.assertIn("CI", why)

    def test_production_without_a_ci_run_refused_even_with_full(self):
        self.assertIsNone(self.choose(ran=False, full="1")[0])

    def test_preview_branch_commit_without_a_ci_build_gets_make_check(self):
        self.assertEqual(self.choose("preview", ran=False)[0], "check")


class ChoosePinnedTest(unittest.TestCase):
    """With no firmware change since production's firmware, the pages go up over the live firmware."""
    def choose(self, channel="preview", full="", ran=True, crit=(), preview=None, fw=(), pinned=Path("kept")):
        return deploy_site.choose_source(channel, full, run() if ran else None, list(crit), preview,
                                         None if fw is None else list(fw), pinned)

    def test_no_firmware_change_goes_up_pinned_on_both_channels(self):
        for channel in ("preview", "production"):
            self.assertEqual(self.choose(channel)[0], "pinned")

    def test_server_code_and_deploy_tooling_ride_pinned(self):
        """Chad, 2026-11-05: make check would only rebuild firmware for them."""
        self.assertEqual(self.choose(crit=["functions/api/report.js", "tools/deploy_site.py"])[0], "pinned")

    def test_a_firmware_change_builds_as_before(self):
        self.assertEqual(self.choose(crit=["lib/x.cpp"], fw=["lib/x.cpp"])[0], "check")
        self.assertEqual(self.choose(fw=["site/installer/install.js"])[0], "ci")

    def test_no_kept_copy_of_the_live_firmware_builds_as_before(self):
        self.assertEqual(self.choose(pinned=None)[0], "ci")

    def test_unknown_firmware_changes_build_as_before(self):
        self.assertEqual(self.choose(fw=None)[0], "ci")

    def test_full_1_builds_everything(self):
        self.assertEqual(self.choose(full="1")[0], "check")

    def test_a_commit_ci_never_built_builds_as_before(self):
        """Pinning skips make check, so CI's green run of HEAD is its test run."""
        self.assertEqual(self.choose(ran=False)[0], "check")

    def test_production_reuses_previews_pinned_build(self):
        self.assertEqual(self.choose("production", preview="pinned")[0], "preview")


class PreviewBuildTest(unittest.TestCase):
    """Preview's kept copy of HEAD, if production may upload it instead of building again."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        staged = Path(self.tmp.name) / "pages"
        (staged / "site").mkdir(parents=True)
        (staged / "site" / "index.html").write_text("page")
        (staged / "wrangler.toml").write_text("toml")
        self.root = Path(self.tmp.name)
        self.preview = Path(self.tmp.name) / "preview"
        self.kept = deploy_site.keep(staged, self.preview, HEAD)
        self.log = [entry("deploy", "b", channel="preview", built="check"),
                    entry("deploy", "a", channel="preview", built="check")]

    def tearDown(self):
        self.tmp.cleanup()

    def built(self, log=None):
        return deploy_site.preview_build(self.log if log is None else log, self.root, HEAD)

    def test_kept_build_of_head_on_preview_is_reusable(self):
        self.assertEqual(self.built(), ("check", []))

    def test_how_preview_built_it_comes_from_the_log(self):
        self.log[-1]["built"] = "ci"
        self.assertEqual(self.built()[0], "ci")

    def test_preview_serving_another_commit_refused(self):
        self.assertIn("preview", self.built(self.log[:1])[1][0])

    def test_preview_rolled_back_to_head_still_reusable(self):
        log = self.log + [entry("deploy", "c", channel="preview", built="ci"),
                          entry("rollback", "a", channel="preview", **{"from": "c" * 40})]
        self.assertEqual(self.built(log), ("check", []))

    def test_changed_kept_copy_refused(self):
        (self.kept / "site" / "index.html").write_text("tampered")
        self.assertTrue(any("index.html" in p for p in self.built()[1]))

    def test_no_kept_copy_refused(self):
        shutil.rmtree(self.kept)
        self.assertTrue(self.built()[1])

    def test_deploy_from_before_the_log_said_how_refused(self):
        del self.log[-1]["built"]
        self.assertTrue(self.built()[1])

    def test_pinned_build_over_productions_firmware_is_reusable(self):
        self.log[-1].update(built="pinned", firmware=OTHER)
        self.assertEqual(deploy_site.preview_build(self.log, self.root, HEAD, OTHER), ("pinned", []))

    def test_pinned_build_over_other_firmware_refused(self):
        """Production's firmware moved since preview pinned it: production would ship firmware preview never had."""
        self.log[-1].update(built="pinned", firmware="c" * 40)
        self.assertIn("firmware", deploy_site.preview_build(self.log, self.root, HEAD, OTHER)[1][0])


def kept_firmware(channel_dir, commit, firmware, dirty=False, readers=False):
    """A kept copy of a deploy of commit whose firmware was built from firmware; readers: with this commit's
    FIRMWARE_READERS, each under a first line saying "// production's <name>"."""
    with tempfile.TemporaryDirectory() as tmp:
        staged = Path(tmp)
        out = staged / "site" / "firmware" / "b1" / "hub"
        out.mkdir(parents=True)
        (staged / "site" / "index.html").write_text("page")
        (out / "app.bin").write_bytes(b"app")
        manifest = {"version": firmware[:7], "build": {"commit": firmware[:7], "dirty": dirty},
                    "builds": [{"parts": [{"path": "app.bin", "offset": 65536,
                                           "sha256": hashlib.sha256(b"app").hexdigest()}]}]}
        (out / "manifest.json").write_text(json.dumps(manifest))
        if readers:
            (staged / "site" / "installer").mkdir()
            for path in deploy_site.FIRMWARE_READERS:
                (staged / path).write_text(f"// production's {Path(path).name}\n" + (deploy_site.ROOT / path).read_text())
        return deploy_site.keep(staged, channel_dir, commit)


class PinnedCopyTest(unittest.TestCase):
    """The kept copy whose firmware a pinned deploy puts up: production's live firmware, byte for byte."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.kept = kept_firmware(self.root / "preview", OTHER, OTHER)
        self.live = {"b1/hub/manifest.json": (self.kept / "site/firmware/b1/hub/manifest.json").read_bytes()}

    def tearDown(self):
        self.tmp.cleanup()

    def pick(self, firmware=OTHER, dirs=None):
        dirs = [self.root / "production", self.root / "preview"] if dirs is None else dirs
        return deploy_site.pinned_copy(dirs, firmware, self.live.get)

    def test_a_kept_copy_of_the_live_firmware(self):
        self.assertEqual(self.pick(), (self.kept, None))

    def test_a_copy_of_other_firmware_is_no_pin(self):
        copy, why = self.pick(firmware=HEAD)
        self.assertIsNone(copy)
        self.assertIn(OTHER[:7], why)

    def test_a_copy_of_pages_newer_than_their_firmware_counts(self):
        """A pinned deploy's copy holds the firmware it pinned."""
        shutil.rmtree(self.kept)
        kept = kept_firmware(self.root / "production", HEAD, OTHER)
        self.assertEqual(self.pick(), (kept, None))

    def test_a_changed_copy_refused(self):
        (self.kept / "site/firmware/b1/hub/app.bin").write_bytes(b"tampered")
        self.assertIn("app.bin", self.pick()[1])

    def test_a_dirty_build_refused(self):
        shutil.rmtree(self.kept)
        self.kept = kept_firmware(self.root / "preview", OTHER, OTHER, dirty=True)
        self.live = {"b1/hub/manifest.json": (self.kept / "site/firmware/b1/hub/manifest.json").read_bytes()}
        self.assertIn("dirty", self.pick()[1])

    def test_a_manifest_production_does_not_serve_refused(self):
        self.live["b1/hub/manifest.json"] = b'{"other": 1}'
        self.assertIn("outrigger.music", self.pick()[1])

    def test_a_manifest_production_has_no_copy_of_refused(self):
        del self.live["b1/hub/manifest.json"]
        self.assertIn("outrigger.music", self.pick()[1])

    def test_a_copy_with_no_firmware_refused(self):
        shutil.rmtree(self.kept / "site" / "firmware")
        self.assertIsNone(self.pick()[0])

    def test_no_copies_at_all(self):
        self.assertIn("no kept copy", self.pick(dirs=[self.root / "nowhere"])[1])

    def test_a_good_copy_wins_over_a_bad_one(self):
        good = kept_firmware(self.root / "production", HEAD, OTHER)
        (self.kept / "site/firmware/b1/hub/app.bin").write_bytes(b"tampered")
        self.assertEqual(self.pick(), (good, None))


class KeptFirmwareTest(unittest.TestCase):
    """A preview rollback uploads the kept copy of a commit; it must hold the firmware the target deploy had."""
    def kept(self, firmware=None):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        staged = Path(tmp.name, "pages")
        (staged / "site").mkdir(parents=True)
        if firmware:
            (staged / "site" / "version.json").write_text(json.dumps({"site": HEAD, "firmware": firmware}))
        return deploy_site.keep(staged, Path(tmp.name, "preview"), HEAD)

    def test_the_copy_of_the_targets_firmware(self):
        self.assertIsNone(deploy_site.kept_firmware_problem(self.kept(OTHER), OTHER))

    def test_a_copy_of_the_same_pages_over_other_firmware_refused(self):
        self.assertIn(OTHER[:7], deploy_site.kept_firmware_problem(self.kept(OTHER), HEAD))

    def test_a_copy_from_before_version_json_built_its_own_firmware(self):
        self.assertIsNone(deploy_site.kept_firmware_problem(self.kept(), HEAD))


class ArtifactTest(unittest.TestCase):
    def test_the_site_firmware_artifact(self):
        art = dict(name="site-firmware", id=5, expired=False, digest="sha256:" + "0" * 64)
        self.assertEqual(deploy_site.site_artifact([dict(name="ui-shots", id=4, expired=False), art]), (art, None))

    def test_missing_artifact_refused(self):
        self.assertIn("site-firmware", deploy_site.site_artifact([])[1])

    def test_expired_artifact_refused(self):
        self.assertIn("expired", deploy_site.site_artifact([dict(name="site-firmware", id=5, expired=True)])[1])

    def test_artifact_without_digest_refused(self):
        self.assertIn("digest", deploy_site.site_artifact([dict(name="site-firmware", id=5, expired=False)])[1])

    def test_download_must_match_githubs_digest(self):
        data = b"zip bytes"
        good = "sha256:" + hashlib.sha256(data).hexdigest()
        self.assertIsNone(deploy_site.digest_problem(data, good))
        self.assertIn("digest", deploy_site.digest_problem(data + b"x", good))


class CiFirmwareTest(unittest.TestCase):
    """CI's site/firmware: SHA256SUMS from CI, and manifests that name HEAD."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fw = Path(self.tmp.name) / "firmware"
        for board in ("b1", "b2"):
            out = self.fw / board / "hub"
            out.mkdir(parents=True)
            (out / "app.bin").write_bytes(board.encode() * 100)
            self.manifest(out, HEAD[:7])
        self.sums()

    def tearDown(self):
        self.tmp.cleanup()

    def manifest(self, out, commit, dirty=False, sha=None):
        sha = sha or hashlib.sha256((out / "app.bin").read_bytes()).hexdigest()
        (out / "manifest.json").write_text(json.dumps({"version": commit, "build": {"commit": commit, "dirty": dirty},
                                                       "builds": [{"parts": [{"path": "app.bin", "sha256": sha}]}]}))

    def sums(self):
        lines = [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  ./{p.relative_to(self.fw)}"
                 for p in sorted(self.fw.rglob("*")) if p.is_file() and p.name != "SHA256SUMS"]
        (self.fw / "SHA256SUMS").write_text("\n".join(lines) + "\n")

    def problems(self):
        return deploy_site.ci_firmware_problems(self.fw, HEAD)

    def test_ci_build_of_head_passes(self):
        self.assertEqual(self.problems(), [])

    def test_changed_file_refused(self):
        (self.fw / "b1" / "hub" / "app.bin").write_bytes(b"changed")
        self.assertTrue(any("b1/hub/app.bin" in p for p in self.problems()))

    def test_missing_file_refused(self):
        (self.fw / "b2" / "hub" / "app.bin").unlink()
        self.assertTrue(any("b2/hub/app.bin" in p for p in self.problems()))

    def test_extra_file_refused(self):
        (self.fw / "b1" / "hub" / "extra.bin").write_bytes(b"x")
        self.assertTrue(any("extra.bin" in p for p in self.problems()))

    def test_no_sums_refused(self):
        (self.fw / "SHA256SUMS").unlink()
        self.assertTrue(any("SHA256SUMS" in p for p in self.problems()))

    def test_manifest_of_another_commit_refused(self):
        self.manifest(self.fw / "b2" / "hub", OTHER[:7])
        self.sums()
        self.assertTrue(any("b2/hub/manifest.json" in p and OTHER[:7] in p for p in self.problems()))

    def test_dirty_build_refused(self):
        self.manifest(self.fw / "b1" / "hub", HEAD[:7], dirty=True)
        self.sums()
        self.assertTrue(any("dirty" in p for p in self.problems()))

    def test_part_not_matching_its_manifest_refused(self):
        self.manifest(self.fw / "b1" / "hub", HEAD[:7], sha="0" * 64)
        self.sums()
        self.assertTrue(any("app.bin" in p for p in self.problems()))

    def test_part_missing_from_the_build_refused(self):
        (self.fw / "b1" / "hub" / "app.bin").unlink()
        self.sums()
        self.assertTrue(any("b1/hub/manifest.json" in p and "app.bin" in p for p in self.problems()))

    def test_no_manifests_refused(self):
        shutil.rmtree(self.fw)
        self.fw.mkdir()
        self.sums()
        self.assertTrue(any("manifest" in p for p in self.problems()))


class CriticalPathsTest(unittest.TestCase):
    """Which changed files make a production deploy run the full make check."""
    def test_site_copy_needs_no_check(self):
        self.assertEqual(deploy_site.critical_paths(["site/index.html", "docs/beta/x.md", "README.md",
                                                     "tools/bench.py", "tests/test_bench.py"]), [])

    def test_any_firmware_change_gets_the_mac_build(self):
        """Store code hides under many names (board_store.h, board_blob.cpp, add_firmware.cpp's LittleFS):
        every firmware change ships the build the hardware smoke check installs."""
        for path in ("firmware/waveshare_169/ui_play.cpp", "firmware/waveshare_169/board_store.h",
                     "lib/board/src/board_blob.cpp", "lib/web_core/src/bench.cpp", "firmware/boards.json",
                     "tools/store_template.py", "tools/boards.py"):
            self.assertEqual(deploy_site.critical_paths([path]), [path], path)

    def test_critical_code_needs_the_check(self):
        for path in ("lib/fm1_loader/src/installer.cpp", "lib/EspUsbHost/src/EspUsbHost.cpp",
                     "firmware/waveshare_169/loader_app.cpp", "firmware/waveshare_169/store_json.h",
                     "firmware/waveshare_169/partitions.csv", "firmware/waveshare_169/switch_chain.h",
                     "functions/api/report.js", "wrangler.toml", "migrations/0002_x.sql",
                     "tools/deploy_site.py", "tools/check.py", "tools/build_site.py", "Makefile",
                     ".github/workflows/ci.yml"):
            self.assertEqual(deploy_site.critical_paths([path]), [path], path)


class FirmwarePathsTest(unittest.TestCase):
    """Which changes since production's firmware need new firmware; any other change can go up pinned."""
    def test_site_server_and_doc_changes_keep_the_live_firmware(self):
        for path in ("site/index.html", "site/style.css", "site/beta/index.html", "site/installer/area.js",
                     "site/installer/add.js", "functions/api/report.js", "wrangler.toml", "migrations/0002_x.sql",
                     "docs/publishing.md", "CONTRIBUTING.md", "firmware/README.md", "tests/test_deploy_site.py",
                     "workers/install-email/index.js", "tools/deploy_site.py", "tools/check_pinned.mjs",
                     "tools/ecosystem_check.py"):
            self.assertEqual(deploy_site.firmware_paths([path]), [], path)

    def test_firmware_and_what_builds_it_need_new_firmware(self):
        for path in ("firmware/waveshare_169/loader_app.cpp", "lib/fm1_loader/src/families_gen.h",
                     "firmware/waveshare_169/partitions.csv", "Makefile", ".github/workflows/ci.yml",
                     "tools/build_site.py", "tools/store_template.py", "tools/boards.py", "tools/gen_families.py",
                     "tools/fm1_families.json", "tools/check.py"):
            self.assertEqual(deploy_site.firmware_paths([path]), [path], path)

    def test_installer_code_that_reads_the_firmware_needs_new_firmware(self):
        """These read the manifests, the store template, the build tag and the loader's block list."""
        for name in ("install.js", "plan.js", "store.js", "board.js", "families_gen.js"):
            path = "site/installer/" + name
            self.assertEqual(deploy_site.firmware_paths([path]), [path], path)

    def test_a_path_nobody_listed_needs_new_firmware(self):
        self.assertEqual(deploy_site.firmware_paths(["sdkconfig.defaults", "newdir/x.c"]),
                         ["sdkconfig.defaults", "newdir/x.c"])


class BatchTest(unittest.TestCase):
    """What a deploy ships: firmware since production's firmware commit, everything else since its pages'."""
    def test_firmware_a_pinned_deploy_left_out_is_still_unshipped(self):
        crit, fw = deploy_site.batch(["lib/x.cpp", "site/index.html"], ["site/index.html"])
        self.assertEqual((crit, fw), (["lib/x.cpp"], ["lib/x.cpp"]))

    def test_server_code_a_pinned_deploy_shipped_is_no_longer_in_the_batch(self):
        crit, fw = deploy_site.batch(["functions/api/report.js", "tools/fm1_families.json"],
                                     ["tools/fm1_families.json"])
        self.assertEqual((crit, fw), ([], ["tools/fm1_families.json"]))

    def test_unshipped_server_code_is_critical(self):
        self.assertEqual(deploy_site.batch(["functions/api/report.js"], ["functions/api/report.js"]),
                         (["functions/api/report.js"], []))


class ChoosePinOverrideTest(unittest.TestCase):
    """PIN=1 (Chad, 2026-11-05, option A): HEAD's pages over production's live firmware even when firmware
    changed since it, so the pages can ship while unshipped firmware waits."""
    def choose(self, channel="preview", full="", ran=True, crit=("lib/ota/x.cpp",), preview=None,
               fw=("lib/ota/x.cpp",), pinned=Path("kept"), pin="1"):
        return deploy_site.choose_source(channel, full, run() if ran else None, list(crit), preview,
                                         None if fw is None else list(fw), pinned, pin)

    def test_firmware_changes_go_up_pinned_on_both_channels(self):
        for channel in ("preview", "production"):
            self.assertEqual(self.choose(channel)[0], "pinned")

    def test_without_pin_a_firmware_change_still_builds(self):
        self.assertEqual(self.choose(pin="")[0], "check")

    def test_pin_with_full_1_refused(self):
        kind, why = self.choose(full="1")
        self.assertIsNone(kind)
        self.assertIn("PIN=1", why)
        self.assertIn("FULL=1", why)

    def test_other_pin_values_refused(self):
        kind, why = self.choose(pin="yes")
        self.assertIsNone(kind)
        self.assertIn("PIN=yes", why)

    def test_no_kept_copy_of_the_live_firmware_refused(self):
        """Building instead would ship the firmware PIN=1 holds back."""
        self.assertIsNone(self.choose(pinned=None)[0])

    def test_a_commit_ci_never_built_refused(self):
        self.assertIsNone(self.choose(ran=False)[0])

    def test_production_never_reuses_previews_new_firmware(self):
        for preview in ("check", "ci"):
            self.assertEqual(self.choose("production", preview=preview)[0], "pinned")

    def test_production_reuses_previews_pinned_build(self):
        self.assertEqual(self.choose("production", preview="pinned")[0], "preview")


class SourceCommandTest(unittest.TestCase):
    """make deploy-site runs deploy-ci for "ci": a pinned deploy goes that way, without make check."""
    def source(self, kind):
        real, argv, out = deploy_site.plan, sys.argv, io.StringIO()
        deploy_site.plan = lambda *a: (kind, HEAD, None, HEAD, None)
        sys.argv = ["deploy_site.py", "source", "preview"]
        try:
            with contextlib.redirect_stdout(out):
                deploy_site.main()
        finally:
            deploy_site.plan, sys.argv = real, argv
        return out.getvalue().strip()

    def test_pinned_goes_to_deploy_ci(self):
        self.assertEqual(self.source("pinned"), "ci")

    def test_the_others_say_themselves(self):
        for kind in ("ci", "check", "preview"):
            self.assertEqual(self.source(kind), kind)


class PinArgumentsTest(unittest.TestCase):
    """make deploy-site passes FULL and PIN, quoted, so an empty FULL doesn't shift PIN into its place."""
    def call(self, *args):
        seen = []
        real, argv = deploy_site.plan, sys.argv
        deploy_site.plan = lambda *a: seen.append(a) or ("check", HEAD, None, HEAD, None, None)
        sys.argv = ["deploy_site.py", *args]
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                try:
                    deploy_site.main()
                except SystemExit:
                    pass
        finally:
            deploy_site.plan, sys.argv = real, argv
        return seen[0]

    def test_source_passes_full_and_pin(self):
        self.assertEqual(self.call("source", "production", "", "1"), ("production", "", "1"))

    def test_deploy_ci_and_deploy_preview_pass_pin(self):
        self.assertEqual(self.call("deploy-ci", "preview", "1"), ("preview", "", "1"))
        self.assertEqual(self.call("deploy-preview", "production", "1"), ("production", "", "1"))


class LegacyBetaTest(unittest.TestCase):
    """The preview channel was called beta until 2026-11-05: deploys logged and copies kept under that name
    still count as preview's, and nothing on disk is rewritten."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.staged = self.root / "pages"
        (self.staged / "site").mkdir(parents=True)
        (self.staged / "site" / "index.html").write_text("page")

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_log_reads_beta_entries_as_previews(self):
        log = self.root / "deploys.jsonl"
        deploy_site.append_log(log, entry("deploy", "a", channel="beta", built="check"))
        before = log.read_text()
        self.assertEqual(deploy_site.read_log(log)[0]["channel"], "preview")
        self.assertEqual(log.read_text(), before)

    def test_production_reuses_a_build_kept_and_logged_as_beta(self):
        deploy_site.keep(self.staged, self.root / "beta", HEAD)
        log = self.root / "deploys.jsonl"
        deploy_site.append_log(log, entry("deploy", "a", channel="beta", built="check"))
        self.assertEqual(deploy_site.preview_build(deploy_site.read_log(log), self.root, HEAD), ("check", []))

    def test_a_copy_kept_as_preview_wins_over_one_kept_as_beta(self):
        deploy_site.keep(self.staged, self.root / "beta", HEAD)
        self.assertEqual(deploy_site.kept_copy(self.root, "preview", HEAD), self.root / "beta" / HEAD)
        deploy_site.keep(self.staged, self.root / "preview", HEAD)
        self.assertEqual(deploy_site.kept_copy(self.root, "preview", HEAD), self.root / "preview" / HEAD)

    def test_no_copy_names_the_preview_one(self):
        self.assertEqual(deploy_site.kept_copy(self.root, "preview", HEAD), self.root / "preview" / HEAD)

    def test_production_never_takes_betas_copies_as_its_own(self):
        deploy_site.keep(self.staged, self.root / "beta", HEAD)
        self.assertEqual(deploy_site.kept_copy(self.root, "production", HEAD), self.root / "production" / HEAD)

    def test_pinning_looks_in_betas_copies_too(self):
        self.assertIn(self.root / "beta", deploy_site.kept_dirs(self.root))

    def test_a_preview_rollback_can_go_back_to_a_deploy_logged_as_beta(self):
        log = self.root / "deploys.jsonl"
        deploy_site.append_log(log, entry("deploy", "a", channel="beta"))
        deploy_site.append_log(log, entry("deploy", "b", channel="preview"))
        target, problem = deploy_site.rollback_target(deploy_site.read_log(log), "preview", "b" * 40)
        self.assertIsNone(problem)
        self.assertEqual(target["commit"], "a" * 40)

    def test_beta_is_still_a_name_for_preview(self):
        out = io.StringIO()
        with contextlib.redirect_stderr(out):
            self.assertEqual(deploy_site.channel_named("beta"), "preview")
        self.assertIn("preview", out.getvalue())
        self.assertEqual(deploy_site.channel_named("production"), "production")

    def test_main_passes_beta_on_as_preview(self):
        seen = []
        real, argv = deploy_site.plan, sys.argv
        deploy_site.plan = lambda *a: seen.append(a) or ("check", HEAD, None, HEAD, None, None)
        sys.argv = ["deploy_site.py", "source", "beta", "", ""]
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                deploy_site.main()
        finally:
            deploy_site.plan, sys.argv = real, argv
        self.assertEqual(seen, [("preview", "", "")])


class EmailRefusalTest(unittest.TestCase):
    """The install email deploys like production, so it borrows production's refusal, minus the site-only advice."""
    def test_drops_the_advice_to_deploy_to_preview(self):
        import deploy_email
        problem = deploy_site.refusal("production", HEAD, OTHER, True, False)
        self.assertIn("preview", problem)
        self.assertEqual(deploy_email.email_refusal(problem),
                         "the install email deploys only origin/master's commit; merge and push first")


class MakefileTest(unittest.TestCase):
    """make deploy-site turns what `source` prints into a deploy-<kind> command. If the two ever disagree,
    production silently falls through to make check and builds again instead of reusing checked bytes."""
    def test_every_reused_source_has_a_makefile_case(self):
        makefile = (deploy_site.ROOT / "Makefile").read_text()
        cases = dict(re.findall(r'"\$\$src" = (\w+) \]; then python3 -I tools/deploy_site.py (deploy-\w+)', makefile))
        self.assertEqual(cases, {"ci": "deploy-ci", "preview": "deploy-preview"})

    def test_the_makefile_defaults_to_preview(self):
        self.assertIn("CHANNEL ?= preview", (deploy_site.ROOT / "Makefile").read_text())


class AssembleTest(unittest.TestCase):
    """CI's firmware plus the commit's own files, laid out as make check leaves build/checked."""
    def test_layout_matches_make_checks(self):
        with tempfile.TemporaryDirectory() as tmp:
            fw, dst = Path(tmp, "fw"), Path(tmp, "pages")
            (fw / "b1" / "hub").mkdir(parents=True)
            (fw / "b1" / "hub" / "app.bin").write_bytes(b"app")
            (fw / "SHA256SUMS").write_text("sums")
            head = deploy_site.git("rev-parse", "HEAD")
            deploy_site.assemble(head, fw, dst)
            self.assertEqual((dst / "site" / "firmware" / "b1" / "hub" / "app.bin").read_bytes(), b"app")
            self.assertFalse((dst / "site" / "firmware" / "SHA256SUMS").exists())
            notice = subprocess.run(["git", "show", f"{head}:NOTICE"], capture_output=True, check=True).stdout
            self.assertEqual((dst / "site" / "NOTICE.txt").read_bytes(), notice)
            self.assertTrue((dst / "site" / "LICENSE.txt").is_file())
            self.assertTrue((dst / "site" / "index.html").is_file())
            self.assertTrue((dst / "functions" / "api" / "report.js").is_file())
            self.assertTrue((dst / "wrangler.toml").is_file())

    def test_pinned_layout_is_this_commits_pages_over_the_kept_firmware(self):
        with tempfile.TemporaryDirectory() as tmp:
            kept = kept_firmware(Path(tmp, "preview"), OTHER, OTHER)
            dst = Path(tmp, "pages")
            head = deploy_site.git("rev-parse", "HEAD")
            deploy_site.assemble(head, kept / "site" / "firmware", dst)
            self.assertEqual((dst / "site/firmware/b1/hub/app.bin").read_bytes(), b"app")
            self.assertTrue((dst / "site" / "installer" / "install.js").is_file())
            self.assertEqual((dst / "commit").read_text().strip(), head)


class PinnedReadersTest(unittest.TestCase):
    """A pinned deploy's FIRMWARE_READERS come from production's kept copy: the installer code that reads the
    firmware stays the version that shipped with it. The rest of the pages are this commit's."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.kept = kept_firmware(self.root / "production", OTHER, OTHER, readers=True)
        self.head = deploy_site.git("rev-parse", "HEAD")
        self.dst = self.root / "pages"

    def tearDown(self):
        self.tmp.cleanup()

    def test_readers_are_productions(self):
        self.assertEqual(deploy_site.assemble_pinned(self.head, self.kept, self.dst), [])
        for path in deploy_site.FIRMWARE_READERS:
            self.assertTrue((self.dst / path).read_text().startswith(f"// production's {Path(path).name}\n"))

    def test_the_rest_is_this_commits(self):
        deploy_site.assemble_pinned(self.head, self.kept, self.dst)
        ui = subprocess.run(["git", "show", f"{self.head}:site/installer/ui.js"], capture_output=True, check=True).stdout
        self.assertEqual((self.dst / "site/installer/ui.js").read_bytes(), ui)
        self.assertEqual((self.dst / "site/firmware/b1/hub/app.bin").read_bytes(), b"app")

    def test_a_name_the_commit_imports_that_productions_reader_lacks_refused(self):
        """add.js imports crc16 from store.js: an older store.js without it would break the page as it loads."""
        store = self.kept / "site/installer/store.js"
        store.write_text(store.read_text().replace("export function crc16", "function crc16"))
        problems = deploy_site.assemble_pinned(self.head, self.kept, self.dst)
        self.assertEqual(len(problems), 1)
        self.assertIn("add.js imports crc16 from store.js", problems[0])

    def test_a_kept_copy_missing_a_reader_refused(self):
        shutil.rmtree(self.root / "production")
        bare = kept_firmware(self.root / "production", OTHER, OTHER)
        problems = deploy_site.assemble_pinned(self.head, bare, self.dst)
        self.assertTrue(problems)
        self.assertIn("install.js", " ".join(problems))


class ImportCheckTest(unittest.TestCase):
    """reader_import_problems: every named import's target exports it, in each form a page or bundle uses."""
    def problems(self, files):
        with tempfile.TemporaryDirectory() as tmp:
            for name, text in files.items():
                Path(tmp, name).write_text(text)
            return deploy_site.reader_import_problems(Path(tmp))

    def test_default_and_named_import(self):
        self.assertEqual(len(self.problems({"a.js": 'import S, { crc } from "./s.js";', "s.js": "export default 1;"})), 1)

    def test_re_export(self):
        self.assertEqual(len(self.problems({"a.js": 'export { crc } from "./s.js";', "s.js": ""})), 1)

    def test_a_minified_export_list_with_dollar_names(self):
        self.assertEqual(self.problems({"a.js": 'import { Transport } from "./b.js";', "b.js": "export{$e as Transport,x as y};"}), [])

    def test_an_inline_module_in_a_page(self):
        page = '<script type="module">import { crc } from "./s.js";</script>'
        self.assertEqual(len(self.problems({"index.html": page, "s.js": ""})), 1)


class PinNoticeTest(unittest.TestCase):
    """PIN=1 says plainly what stays behind."""
    def test_names_the_firmware_kept_and_counts_whats_held_back(self):
        self.assertEqual(deploy_site.pin_notice(OTHER, ["lib/ota/a.cpp", "firmware/b.cpp", "Makefile"]),
                         "PIN=1: keeping production firmware bbbbbbb; 3 firmware changes on master NOT shipped")

    def test_one_change(self):
        self.assertIn("1 firmware change on master NOT shipped", deploy_site.pin_notice(OTHER, ["lib/x.cpp"]))

    def test_the_deploy_log_records_pin_1_even_with_nothing_held(self):
        self.assertEqual(deploy_site.pin_fields(["lib/x.cpp", "Makefile"]), {"pin": True, "held": 2})
        self.assertEqual(deploy_site.pin_fields([]), {"pin": True, "held": 0})

    def test_no_pin_adds_nothing(self):
        self.assertEqual(deploy_site.pin_fields(None), {})


class VersionFileTest(unittest.TestCase):
    """What goes up says which commit its pages and its firmware came from (version.json)."""
    def stage(self, firmware):
        with tempfile.TemporaryDirectory() as tmp:
            checked = Path(tmp, "checked")
            (checked / "site").mkdir(parents=True)
            (checked / "site" / "index.html").write_text("page")
            (checked / "functions").mkdir()
            (checked / "wrangler.toml").write_text("toml")
            missing = deploy_site.stage_root(checked, Path(tmp, "pages"), HEAD, firmware)
            return missing, json.loads(Path(tmp, "pages", "site", "version.json").read_text())

    def test_a_full_build_says_one_commit(self):
        self.assertEqual(self.stage(HEAD), ([], {"site": HEAD, "firmware": HEAD}))

    def test_a_pinned_build_says_both(self):
        self.assertEqual(self.stage(OTHER)[1], {"site": HEAD, "firmware": OTHER})

    def test_a_pin_1_build_says_both(self):
        """HEAD's pages over production's kept copy, staged as publish stages it."""
        with tempfile.TemporaryDirectory() as tmp:
            kept = kept_firmware(Path(tmp, "production"), OTHER, OTHER, readers=True)
            head = deploy_site.git("rev-parse", "HEAD")
            deploy_site.assemble_pinned(head, kept, Path(tmp, "checked"))
            self.assertEqual(deploy_site.stage_root(Path(tmp, "checked"), Path(tmp, "pages"), head, OTHER), [])
            version = json.loads(Path(tmp, "pages", "site", "version.json").read_text())
            self.assertEqual(version, {"site": head, "firmware": OTHER})


if __name__ == "__main__":
    unittest.main()
