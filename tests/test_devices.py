"""Tests for tools/devices.py: the test-hardware registry, read from tools/devices.example.json.

    python3 -I tests/test_devices.py
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import devices  # noqa: E402

EXAMPLE = ROOT / "tools/devices.example.json"


class RegistryTest(unittest.TestCase):
    def setUp(self):
        self.reg = devices.load(EXAMPLE)

    def test_find_by_label_any_case(self):
        self.assertEqual(self.reg.find("small-v2")["mac"], "02:00:00:00:00:02")
        self.assertEqual(self.reg.find("Big-S3")["kind"], "esp32")

    def test_unknown_label_says_which_exist(self):
        with self.assertRaises(devices.DeviceError) as e:
            self.reg.find("Waveshare-1")
        self.assertIn("Small-V1", str(e.exception))

    def test_by_mac_any_form(self):
        self.assertEqual(self.reg.by_mac("02:00:00:00:00:01")["label"], "Small-V1")
        self.assertEqual(self.reg.by_mac("02:00:00:00:00:01".upper())["label"], "Small-V1")
        self.assertIsNone(self.reg.by_mac("02:00:00:00:00:99"))

    def test_expect_esp32(self):
        self.reg.expect_esp32("Small-V2", "02:00:00:00:00:02")
        with self.assertRaises(devices.DeviceError) as e:
            self.reg.expect_esp32("Small-V2", "02:00:00:00:00:01")
        self.assertIn("Small-V1", str(e.exception))  # says what it found instead

    def test_expect_fm1(self):
        self.reg.expect_fm1("FM-A", "00000000000000aa")
        self.reg.expect_fm1("FM-A", "00000000000000aaffff")  # a longer form of the same unit
        with self.assertRaises(devices.DeviceError):
            self.reg.expect_fm1("FM-A", "00000000000000bb")

    def test_an_unread_unit_refuses_and_says_how(self):
        with self.assertRaises(devices.DeviceError) as e:
            self.reg.expect_fm1("FM-B", "1234567890abcdef")
        self.assertIn("1234567890abcdef", str(e.exception))
        self.assertIn("add it to " + str(EXAMPLE), str(e.exception))

    def test_kind_mismatch(self):
        with self.assertRaises(devices.DeviceError):
            self.reg.expect_esp32("FM-A", "02:00:00:00:00:02")

    def test_bad_files_refused(self):
        for bad in ({"devices": [{"label": "X", "kind": "esp32"}]},                       # no mac
                    {"devices": [{"label": "X", "kind": "esp32", "mac": "zz"}]},          # not a mac
                    {"devices": [{"label": "X", "kind": "fm1", "unit": "short"}]},        # not a unit
                    {"devices": [{"label": "X", "kind": "esp32", "mac": "02:00:00:00:00:01"},
                                 {"label": "x", "kind": "esp32", "mac": "02:00:00:00:00:02"}]},  # label twice
                    {"devices": [{"label": "X", "kind": "pi", "mac": "02:00:00:00:00:01"}]}):     # kind
            with self.subTest(bad=bad), tempfile.NamedTemporaryFile("w", suffix=".json") as f:
                json.dump(bad, f)
                f.flush()
                with self.assertRaises(devices.DeviceError):
                    devices.load(f.name)

    def test_missing_file_says_where(self):
        with self.assertRaises(devices.DeviceError) as e:
            devices.load("/nonexistent/devices.json")
        self.assertIn("devices.example.json", str(e.exception))


class BenchHeaderTest(unittest.TestCase):
    """The firmware's board table (lib/web_core/src/bench.cpp), written at build time so no MAC is in the repo."""

    def test_lists_each_esp32_by_name_and_mac(self):
        text = devices.bench_header(devices.load(EXAMPLE))
        self.assertIn('{"small-v1", {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}},', text)
        self.assertIn('{"big-s3", {0x02, 0x00, 0x00, 0x00, 0x00, 0x03}},', text)
        self.assertNotIn("fm-a", text.lower())  # FM-1s have no MAC

    def test_no_registry_is_an_empty_table(self):
        text = devices.bench_header(None)
        self.assertNotIn("{", text)

    def test_the_name_is_the_host_the_tools_dial(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json") as f:
            json.dump({"devices": [{"label": "Small-V1", "kind": "esp32", "mac": "02:00:00:00:00:01",
                                    "host": "bench-one.local"},
                                   {"label": "Big-P4", "kind": "esp32", "mac": "02:00:00:00:00:04"}]}, f)
            f.flush()
            text = devices.bench_header(devices.load(f.name))
        self.assertIn('{"bench-one", {', text)  # tools reach it at its host, so the firmware answers there
        self.assertIn('{"big-p4", {', text)     # no host: the label

    def test_a_name_too_long_for_the_board_is_refused(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json") as f:
            json.dump({"devices": [{"label": "Waveshare-number-one", "kind": "esp32", "mac": "02:00:00:00:00:01"}]}, f)
            f.flush()
            with self.assertRaises(devices.DeviceError):
                devices.bench_header(devices.load(f.name))

    def test_write_header(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "gen" / "bench_boards.h"
            devices.write_header(out, None)
            self.assertNotIn("0x", out.read_text())
            devices.write_header(out, EXAMPLE)
            self.assertIn("0x03", out.read_text())
            before = out.stat().st_mtime_ns
            devices.write_header(out, EXAMPLE)  # unchanged: not rewritten, so nothing recompiles
            self.assertEqual(out.stat().st_mtime_ns, before)

    def test_a_bench_build_needs_the_registry(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "bench_boards.h"
            with self.assertRaises(devices.DeviceError):
                devices.write_header(out, "/nonexistent/devices.json")
            devices.write_header(out, "/nonexistent/devices.json", optional=True)  # a DIAG_WEB build without one
            self.assertNotIn("0x", out.read_text())


if __name__ == "__main__":
    unittest.main()
