"""Tests for tools/deploy_site.py: which commits may go live, and what may be uploaded.

    python3 -I tests/test_deploy_site.py
"""
import hashlib
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


class SiteProblemsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.site = Path(self.tmp.name)
        (self.site / "index.html").write_text("<!doctype html>")
        (self.site / "firmware").mkdir()
        (self.site / "firmware" / "app.bin").write_bytes(b"\x01" * 4096)
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
        (self.site / "firmware" / "app.bin").unlink()
        self.assertTrue(any("firmware" in p for p in self.problems()))

    def test_file_over_the_per_file_limit_refused(self):
        (self.site / "firmware" / "app.bin").write_bytes(b"\0" * 2048)
        self.assertTrue(any("app.bin" in p for p in self.problems(max_file=1024)))

    def test_too_many_files_refused(self):
        self.assertTrue(any("files" in p for p in self.problems(max_files=1)))

    def test_file_with_the_blob_sha_refused(self):
        sha = hashlib.sha256(self.blob).hexdigest()
        (self.site / "firmware" / "x.bin").write_bytes(self.blob)
        self.assertTrue(any("x.bin" in p for p in deploy_site.site_problems(self.site, None, sha=sha)))

    def test_file_containing_a_piece_of_the_blob_refused(self):
        (self.site / "firmware" / "app.bin").write_bytes(b"\x01" * 100 + self.blob[512:1536] + b"\x02" * 100)
        self.assertTrue(any("app.bin" in p for p in self.problems()))

    def test_file_named_like_the_blob_refused(self):
        (self.site / "firmware" / "WL82Loader.bin").write_bytes(b"\x03")
        self.assertTrue(any("WL82Loader.bin" in p for p in self.problems()))

    def test_no_local_blob_refused(self):
        self.assertTrue(any("wl82loader.bin" in p for p in deploy_site.site_problems(self.site, None)))


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


if __name__ == "__main__":
    unittest.main()
