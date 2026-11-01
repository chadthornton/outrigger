"""Tests for tools/boards.py: the board profiles in firmware/boards.json, their
checks, and what they generate for the Makefile, the firmware and ui-shots.

    python3 -I tests/test_boards.py
"""
import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import boards  # noqa: E402

REAL = json.loads((ROOT / "firmware/boards.json").read_text())


def one(**changes):
    """The 3.5" profile with dotted-path changes: one(**{"display.dc": 19})."""
    b = copy.deepcopy(REAL["boards"]["waveshare_35"])
    for path, value in changes.items():
        *parents, leaf = path.split(".")
        node = b
        for p in parents:
            node = node[p]
        if value is DELETE:
            del node[leaf]
        else:
            node[leaf] = value
    return b


DELETE = object()


def problems(board, others=()):
    table = {"boards": {"x": board, **{f"o{i}": o for i, o in enumerate(others)}}}
    return boards.check(table)


class RealTableTest(unittest.TestCase):
    def test_the_shipped_table_passes(self):
        self.assertEqual(boards.check(REAL), [])

    def test_the_169_keeps_its_ids(self):
        # Existing installs and manifest URLs use these; the build tag's board 0 is the 1.69.
        b = REAL["boards"]["waveshare_169"]
        self.assertEqual(b["site_id"], "waveshare-esp32-s3-touch-lcd-1.69")
        self.assertEqual(b["tag_id"], 0)

    def test_the_169_is_first(self):
        # ui-shots renders boards in table order; the 1.69's shots stay byte-for-byte.
        self.assertEqual(boards.ids(REAL)[0], "waveshare_169")


class CheckTest(unittest.TestCase):
    def assertRefused(self, board, words, others=()):
        found = problems(board, others)
        self.assertTrue(any(words in p for p in found), f"wanted {words!r} in {found}")

    def test_usb_pins_are_the_hosts(self):
        self.assertRefused(one(**{"display.dc": 19}), "GPIO19")
        self.assertRefused(one(**{"display.backlight": 20}), "GPIO20")

    def test_octal_psram_pins(self):
        self.assertRefused(one(**{"hold_low": [35]}), "GPIO35")

    def test_quad_psram_frees_33_to_37(self):
        self.assertEqual(problems(one(**{"hold_low": [35], "psram": "quad"})), [])

    def test_flash_pins(self):
        self.assertRefused(one(**{"touch.sda": 29}), "GPIO29")

    def test_pins_the_s3_lacks(self):
        self.assertRefused(one(**{"display.sck": 23}), "GPIO23")
        self.assertRefused(one(**{"display.sck": 49}), "GPIO49")

    def test_one_pin_two_jobs(self):
        self.assertRefused(one(**{"display.backlight": 3}), "GPIO3")

    def test_expander_pins_need_an_expander(self):
        self.assertRefused(one(**{"expander": None}), "exio1")

    def test_expander_pin_range(self):
        self.assertRefused(one(**{"hold_low": ["exio8"]}), "exio8")

    def test_one_expander_pin_two_jobs(self):
        self.assertRefused(one(**{"hold_low": ["exio1"]}), "exio1")

    def test_missing_field(self):
        self.assertRefused(one(**{"display.driver": DELETE}), "display.driver")
        self.assertRefused(one(**{"screen": DELETE}), "screen")

    def test_unknown_driver(self):
        self.assertRefused(one(**{"display.driver": "ili9341"}), "ili9341")
        self.assertRefused(one(**{"touch.driver": "gt911"}), "gt911")

    def test_store_needs_16mb_flash(self):
        self.assertRefused(one(**{"flash_mb": 8}), "16 MB")

    def test_psram_8mb(self):
        self.assertRefused(one(**{"psram_mb": 2}), "8 MB")

    def test_screen_fits_ui_shots(self):
        self.assertRefused(one(**{"screen.width": 800}), "480")

    def test_rounded_needs_a_radius(self):
        self.assertRefused(one(**{"screen.shape": "rounded", "screen.corner_radius": 0}), "corner_radius")

    def test_ids_unique(self):
        self.assertRefused(one(), "tag_id 1", others=[one(**{"site_id": "other"})])
        self.assertRefused(one(), "site_id", others=[one(**{"tag_id": 7})])

    def test_expander_key_must_be_there(self):
        self.assertRefused(one(**{"expander": DELETE}), "expander")
        self.assertRefused(one(**{"expander.address": DELETE}), "expander.address")

    def test_bools_are_not_pins(self):
        self.assertRefused(one(**{"touch.rst": True}), "touch.rst")

    def test_i2c_addresses(self):
        self.assertRefused(one(**{"touch.address": 0x150}), "touch.address")
        self.assertRefused(one(**{"expander.address": 3}), "expander.address")

    def test_board_ids_are_make_and_c_names(self):
        found = boards.check({"boards": {"bad-id": one()}})
        self.assertTrue(any("bad-id" in p for p in found), found)


class OutputTest(unittest.TestCase):
    def test_fqbn(self):
        self.assertEqual(boards.fqbn(REAL["boards"]["waveshare_169"]),
                         "esp32:esp32:esp32s3:USBMode=default,FlashSize=16M,PSRAM=opi")
        self.assertIn("PSRAM=enabled", boards.fqbn(one(psram="quad")))

    def test_pin_codes(self):
        # The firmware's encoding: GPIO n is n, expander pin n is 100 + n, none is -1.
        self.assertEqual(boards.pin_code(15), 15)
        self.assertEqual(boards.pin_code("exio7"), 107)
        self.assertEqual(boards.pin_code(None), -1)

    def test_header_for_the_35(self):
        h = boards.header("waveshare_35", REAL["boards"]["waveshare_35"])
        for want in ["#define BOARD_DISPLAY_ST7796 1", "#define BOARD_TOUCH_FT6336 1", "#define BOARD_EXPANDER_TCA9554 1",
                     "#define BOARD_TAG_ID 1", "kLcdRst = 101", "kLcdCs = -1", "kHoldLow[] = {107}", "kSpiHz = 40000000"]:
            self.assertIn(want, h)

    def test_header_for_the_169(self):
        h = boards.header("waveshare_169", REAL["boards"]["waveshare_169"])
        for want in ["#define BOARD_DISPLAY_ST7789 1", "#define BOARD_TOUCH_CST816 1", "#define BOARD_TAG_ID 0",
                     "kHoldLow[] = {42}", "kSpiHz = 80000000", "kOffsets[4] = {0, 20, 0, 20}",
                     "ScreenShape::Rounded, 40, 0}"]:
            self.assertIn(want, h)
        self.assertNotIn("BOARD_EXPANDER", h)

    def test_shots_table_is_the_real_screens(self):
        t = boards.shots_table(REAL)
        self.assertIn('{"240x280", 240, 280, 40, 0, InputKind::Touch}', t)
        self.assertIn('{"320x480", 320, 480, 0, 1, InputKind::Touch}', t)
        self.assertLess(t.index("240x280"), t.index("320x480"))

    def test_shots_with_one_resolution_get_distinct_ids(self):
        table = {"boards": {"a": REAL["boards"]["waveshare_169"],
                            "b": dict(REAL["boards"]["waveshare_169"], screen={**REAL["boards"]["waveshare_169"]["screen"],
                                                                            "shape": "rect", "corner_radius": 0})}}
        t = boards.shots_table(table)
        self.assertIn('{"240x280", 240, 280, 40, 0,', t)
        self.assertIn('{"240x280-r0", 240, 280, 0, 0,', t)

    def test_write_if_changed_keeps_the_file_time(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "sub/x.h"
            self.assertTrue(boards.write_if_changed(p, "a"))
            self.assertFalse(boards.write_if_changed(p, "a"))
            self.assertTrue(boards.write_if_changed(p, "b"))
            self.assertEqual(p.read_text(), "b")


if __name__ == "__main__":
    unittest.main()
