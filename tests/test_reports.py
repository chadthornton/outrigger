"""Tests for tools/reports.py: which database each channel reads, how a report reads, where `new` stops.

    python3 -I tests/test_reports.py
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import reports  # noqa: E402

TOML = """
pages_build_output_dir = "site"
[[d1_databases]]
binding = "DB"
database_name = "prod-db"
database_id = "p1"
[[env.preview.d1_databases]]
binding = "DB"
database_name = "beta-db"
database_id = "b1"
"""

ROW = {"id": 7, "created_at": "2026-11-03T15:00:00.000Z", "board": "Freenove 2.8", "chip": "ESP32-S3 (QFN56)",
       "flash_mb": 16, "psram_mb": 8, "usb_vid": 0x303a, "picked": "Another ESP32-S3 board", "installer": "2026-11-03.1",
       "email": None, "note": "line one\nline two", "verdict": "no profile yet"}


class DatabaseTest(unittest.TestCase):
    def test_production_reads_the_top_level_binding_and_beta_the_preview_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "wrangler.toml")
            path.write_text(TOML)
            self.assertEqual(reports.database("production", path), ("prod-db", "p1"))
            self.assertEqual(reports.database("beta", path), ("beta-db", "b1"))

    def test_the_repos_config_has_both(self):
        self.assertEqual(reports.database("production")[0], "outrigger-reports")
        self.assertEqual(reports.database("beta")[0], "outrigger-reports-beta")


class DescribeTest(unittest.TestCase):
    def test_a_report_reads_as_text(self):
        text = reports.describe(ROW)
        self.assertIn("#7  2026-11-03T15:00:00.000Z  Freenove 2.8", text)
        self.assertIn("flash 16 MB, PSRAM 8 MB, USB vendor 0x303a", text)
        self.assertIn("email: (none)", text)
        self.assertIn("note: line one\n        line two", text)

    def test_unknowns_show_as_question_marks(self):
        text = reports.describe({**ROW, "flash_mb": None, "psram_mb": None, "usb_vid": None, "chip": None})
        self.assertIn("chip: ?, flash ?, PSRAM ?, USB vendor ?", text)

    def test_flash_under_1_mb(self):
        self.assertIn("flash 0.25 MB", reports.describe({**ROW, "flash_mb": 0.25}))


class SeenTest(unittest.TestCase):
    def test_new_starts_at_zero_and_remembers_each_channel(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "sub", "seen.json")
            self.assertEqual(reports.seen("beta", path), 0)
            reports.remember("beta", 4, path)
            reports.remember("production", 9, path)
            self.assertEqual((reports.seen("beta", path), reports.seen("production", path)), (4, 9))


if __name__ == "__main__":
    unittest.main()
