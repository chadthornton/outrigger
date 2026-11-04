"""Tests for tools/installs.py: how the anonymous install counts add up and read.

    python3 -I tests/test_installs.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import installs  # noqa: E402

S, B = "waveshare-esp32-s3-touch-lcd-1.69", "waveshare-esp32-s3-touch-lcd-3.5"


def row(day, board=S, edition="hub-loader", result="ok", failure=None, installer="2026-11-04.2"):
    return {"day": day, "board": board, "edition": edition, "installer": installer, "result": result, "failure": failure}


ROWS = [row("2026-11-02"), row("2026-11-03", B), row("2026-11-03", failure="read", result="failed"),
        row("2026-11-04", edition="hub"), row("2026-11-09", B, failure="write", result="failed"),
        row("2026-11-09", B, failure="write", result="failed")]


class SummariseTest(unittest.TestCase):
    def test_totals_and_failures_by_where_they_stopped(self):
        s = installs.summarise(ROWS)
        self.assertEqual((s["ok"], s["failed"]), (3, 3))
        self.assertEqual(s["failures"], {"write": 2, "read": 1})

    def test_by_board_and_edition(self):
        s = installs.summarise(ROWS)
        self.assertEqual(s["boards"], {S: [2, 1], B: [1, 2]})
        self.assertEqual(s["editions"], {"hub-loader": [2, 3], "hub": [1, 0]})

    def test_weeks_start_on_monday(self):
        # 2026-11-02 is a Monday; 11-09 the next one.
        self.assertEqual(installs.summarise(ROWS)["weeks"], {"2026-11-02": [3, 1], "2026-11-09": [0, 2]})

    def test_nothing_yet(self):
        s = installs.summarise([])
        self.assertEqual((s["ok"], s["failed"], s["boards"]), (0, 0, {}))


class DescribeTest(unittest.TestCase):
    def test_reads_as_text_with_short_board_names(self):
        text = installs.describe("outrigger-reports-beta", installs.summarise(ROWS))
        self.assertIn("outrigger-reports-beta: 3 finished, 3 failed", text)
        self.assertIn("Waveshare 1.69\"", text)
        self.assertIn("write 2, read 1", text)
        self.assertIn("week of 2026-11-09", text)
        self.assertIn("once per board per browser", text)

    def test_an_unknown_board_shows_its_id(self):
        self.assertIn("some-new-board", installs.describe("db", installs.summarise([row("2026-11-04", "some-new-board")])))

    def test_nothing_yet(self):
        self.assertIn("no installs counted yet", installs.describe("db", installs.summarise([])))


if __name__ == "__main__":
    unittest.main()
