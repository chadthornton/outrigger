"""Tests for tools/deploy_site.py: which commits may go live, and what may be uploaded.

    python3 -I tests/test_deploy_site.py
"""
import hashlib
import json
import shutil
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

    def test_beta_takes_any_pushed_commit(self):
        self.assertIsNone(self.refusal("beta", master=OTHER))

    def test_beta_refuses_an_unpushed_commit(self):
        self.assertIn("push", self.refusal("beta", master=OTHER, pushed=False))

    def test_dirty_tree_refused_on_both_channels(self):
        for channel in ("production", "beta"):
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
        log = [entry("deploy", "a", channel="beta"), entry("deploy", "b")]
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


class BetaFirstTest(unittest.TestCase):
    def test_production_needs_beta_on_the_same_commit(self):
        self.assertIsNone(deploy_site.beta_first_problem("production", HEAD, HEAD))
        self.assertIn("beta", deploy_site.beta_first_problem("production", HEAD, OTHER))

    def test_beta_itself_needs_nothing(self):
        self.assertIsNone(deploy_site.beta_first_problem("beta", HEAD, OTHER))


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
    """Beta can't use Cloudflare's rollback, so each deploy keeps what it uploaded, with SHA-256 sums."""
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
        return deploy_site.keep(self.staged, self.root / "kept" / "beta", commit * 40)

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
        self.assertTrue(deploy_site.kept_problems(self.root / "kept" / "beta" / ("c" * 40)))

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
        kept = sorted(p.name[0] for p in (self.root / "kept" / "beta").iterdir())
        self.assertEqual(kept, ["b", "c", "d"])

    def test_keeping_a_commit_again_replaces_its_copy(self):
        self.keep("a")
        (self.staged / "site" / "index.html").write_text("page 2")
        kept = self.keep("a")
        self.assertEqual((kept / "site" / "index.html").read_text(), "page 2")
        self.assertEqual(deploy_site.kept_problems(kept), [])


if __name__ == "__main__":
    unittest.main()
