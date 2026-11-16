"""Tests for tools/profiles/midiguide.py: midi.guide CSVs -> device profiles.

    python3 -I tests/test_profiles_midiguide.py

The CSV rows are inline (no checkout of pencilresearch/midi needed); the MicroFreak and FM1 rows are
copied from midi.guide (CC-BY-SA 4.0, Pencil Research).
"""
import json
import sys
import tempfile
import unittest
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools/profiles"))
import midiguide as mg  # noqa: E402

HEADER = ("manufacturer,device,section,parameter_name,parameter_description,cc_msb,cc_lsb,cc_min_value,"
          "cc_max_value,cc_default_value,nrpn_msb,nrpn_lsb,nrpn_min_value,nrpn_max_value,nrpn_default_value,"
          "orientation,notes,usage\n")
TRIG_HEADER = ("manufacturer,device,section,sound_name,sound_description,midi_channel,note_number_min_value,"
               "note_number_max_value,note_number_default_value,comments,velocity_usage\n")

MICROFREAK_TYPE = ("Arturia,MicroFreak,Oscillator,Type,,9,,0,127,,,,,,,0-based,,"
                   "0-11: Basic Waves; 12-16: SuperWave; 17-22: Wavetable; 122-127: Vocoder\n")


def one(rows, header=HEADER, manufacturer="Arturia", device="MicroFreak"):
    profile, warnings = mg.import_device(manufacturer, device, header + rows)
    params = [p for s in profile["sections"] for p in s["parameters"]]
    return profile, params, [w["code"] for w in warnings]


class UsageTest(unittest.TestCase):
    def test_microfreak_type_names_its_values(self):
        _, (p,), codes = one(MICROFREAK_TYPE)
        self.assertEqual(codes, [])
        self.assertEqual(p["usage"][0], {"from": 0, "to": 11, "kind": "switch", "label": "Basic Waves"})
        self.assertEqual([s["label"] for s in mg.value_names(p["usage"])],
                         ["Basic Waves", "SuperWave", "Wavetable", "Vocoder"])
        self.assertEqual(p["cc"], {"msb": 9, "lsb": None, "min": 0, "max": 127, "default": None})
        self.assertIsNone(p["nrpn"])

    def test_mixed_single_switch_and_continuous(self):
        u = mg.parse_usage("0: Off; 1-10: Square; 11-20: Saw; 21~127: Morph")
        self.assertEqual([(s["from"], s["to"], s["kind"]) for s in u],
                         [(0, 0, "value"), (1, 10, "switch"), (11, 20, "switch"), (21, 127, "continuous")])
        self.assertEqual(len(mg.value_names(u)), 3)

    def test_continuous_label_is_not_a_value_name(self):
        u = mg.parse_usage("0~127: Volume")
        self.assertEqual(u, [{"from": 0, "to": 127, "kind": "continuous", "label": "Volume"}])
        self.assertEqual(mg.value_names(u), [])

    def test_empty_usage(self):
        self.assertEqual(mg.parse_usage(""), [])
        self.assertEqual(mg.parse_usage("   "), [])
        self.assertEqual(mg.parse_usage(None), [])

    def test_trailing_semicolon_and_odd_whitespace(self):
        u = mg.parse_usage("  0 - 63 :  Off ;64-127:On;; ")
        self.assertEqual([(s["from"], s["to"], s["label"]) for s in u], [(0, 63, "Off"), (64, 127, "On")])

    def test_label_may_hold_dashes_and_parentheses(self):
        (s,) = mg.parse_usage("0-16256: Hz (NRPN, the 14-bit value is the frequency)")
        self.assertEqual((s["to"], s["label"]), (16256, "Hz (NRPN, the 14-bit value is the frequency)"))

    def test_bad_segments_are_reported_and_dropped(self):
        pr = mg.Problems()
        u = mg.parse_usage("Off; -3: Down; 5:", 0, 127, pr, line=7)
        self.assertEqual([(s["from"], s["label"]) for s in u], [(5, "")])
        self.assertEqual([i["code"] for i in pr.items], ["usage-unparsed", "usage-unparsed", "usage-no-label"])
        self.assertTrue(all(i["line"] == 7 for i in pr.items))

    def test_out_of_range_and_overlap_warn(self):
        pr = mg.Problems()
        mg.parse_usage("0-63: Off; 60-200: On", 0, 127, pr)
        self.assertEqual(sorted(i["code"] for i in pr.items), ["usage-out-of-range", "usage-overlap"])

    def test_reversed_range_warns(self):
        pr = mg.Problems()
        mg.parse_usage("10-5: Odd", 0, 127, pr)
        self.assertEqual([i["code"] for i in pr.items], ["usage-reversed"])


class ParameterTest(unittest.TestCase):
    def test_nrpn_only_with_values_above_127(self):
        _, (p,), codes = one("Arturia,MicroFreak,Filter,Cutoff fine,,,,,,,3,17,0,16383,8192,0-based,,0~16383: Cutoff\n")
        self.assertEqual(codes, [])
        self.assertIsNone(p["cc"])
        self.assertEqual(p["nrpn"], {"msb": 3, "lsb": 17, "min": 0, "max": 16383, "default": 8192})
        self.assertEqual(p["usage_units"], "nrpn")

    def test_14_bit_cc(self):
        _, (p,), _ = one("Arturia,MicroFreak,General,Mod wheel,,1,33,0,127,0,,,,,,0-based,,\n")
        self.assertEqual((p["cc"]["msb"], p["cc"]["lsb"]), (1, 33))

    def test_cc_and_nrpn_row_with_usage_in_nrpn_values(self):
        _, (p,), codes = one("Elektron,Tonverk,Track,Rate,,9,,0,127,,1,2,0,2047,,0-based,,0-682: BPM 1; 683-1365: BPM 2\n",
                             manufacturer="Elektron", device="Tonverk")
        self.assertEqual(codes, [])
        self.assertEqual(p["usage_units"], "nrpn")

    def test_row_without_cc_or_nrpn_is_kept_and_flagged(self):
        _, (p,), codes = one("Arturia,MicroFreak,,Banksel,,,,0,127,,,,,,,0-based,,\n")
        self.assertEqual(p["name"], "Banksel")
        self.assertEqual(codes, ["no-message"])

    def test_sections_in_first_seen_order_and_duplicate_ccs_allowed(self):
        rows = ("Arturia,MicroFreak,Filter,Cutoff,,23,,0,127,,,,,,,0-based,,\n"
                "Arturia,MicroFreak,Oscillator,Wave,,10,,0,127,,,,,,,0-based,,\n"
                "Arturia,MicroFreak,Filter,Cutoff (mode 2),,23,,0,127,,,,,,,0-based,,\n")
        profile, _, codes = one(rows)
        self.assertEqual(codes, [])
        self.assertEqual([(s["name"], len(s["parameters"])) for s in profile["sections"]],
                         [("Filter", 2), ("Oscillator", 1)])

    def test_bom_header(self):
        text = mg._decode(("﻿" + HEADER + MICROFREAK_TYPE).encode("utf-8"))
        profile, warnings = mg.import_device("Arturia", "MicroFreak", text)
        self.assertEqual(warnings, [])
        self.assertEqual(profile["sections"][0]["parameters"][0]["name"], "Type")

    def test_not_a_number_and_bad_orientation(self):
        _, _, codes = one("Arturia,MicroFreak,X,Y,,9a,,0,127,,,,,,,sideways,,\n")
        self.assertIn("not-a-number", codes)
        self.assertIn("orientation", codes)

    def test_default_outside_range(self):
        _, _, codes = one("Arturia,MicroFreak,X,Y,,9,,0,127,200,,,,,,0-based,,\n")
        self.assertEqual(codes, ["default-out-of-range"])

    def test_names_compared_composed(self):
        # The folder name as macOS returns it (decomposed) against the composed CSV column.
        device = unicodedata.normalize("NFC", "Mäander")
        rows = f"Flame,{device},X,Y,,9,,0,127,,,,,,,0-based,,\n"
        nfd = mg._nfc(unicodedata.normalize("NFD", device))
        _, warnings = mg.import_device("Flame", nfd, HEADER + rows)
        self.assertEqual(warnings, [])

    def test_wrong_device_column_warns(self):
        _, _, codes = one("Arturia,MiniFreak,X,Y,,9,,0,127,,,,,,,0-based,,\n")
        self.assertEqual(codes, ["device-mismatch"])

    def test_missing_required_column_fails(self):
        with self.assertRaises(ValueError):
            mg.import_device("A", "B", "manufacturer,device,section\nA,B,C\n")

    def test_header_only_fails(self):
        with self.assertRaises(ValueError):
            mg.import_device("A", "B", HEADER)


class TriggersTest(unittest.TestCase):
    def test_channel_words_and_velocity(self):
        text = TRIG_HEADER + ("Alesis,SR-16,Kit,Kick,,basic,36,36,36,,1~127: Volume\n"
                              "Alesis,SR-16,Kit,Snare,,10,38,38,38,,\n")
        profile, warnings = mg.import_device("Alesis", "SR-16", None, text)
        self.assertEqual(warnings, [])
        kick, snare = profile["triggers"][0]["notes"]
        self.assertEqual((kick["channel"], kick["note"]["min"]), ("basic", 36))
        self.assertEqual(kick["velocity"][0]["kind"], "continuous")
        self.assertEqual(snare["channel"], 10)
        self.assertEqual(profile["sections"], [])


class TreeTest(unittest.TestCase):
    def test_every_file_is_accounted_for(self):
        with tempfile.TemporaryDirectory() as d:
            src, out = Path(d) / "midi", Path(d) / "out"
            (src / "Arturia").mkdir(parents=True)
            (src / "M-VAVE").mkdir()
            (src / "Bad").mkdir()
            (src / "template.csv").write_text(HEADER)
            (src / "Arturia/MicroFreak.csv").write_text(HEADER + MICROFREAK_TYPE)
            (src / "M-VAVE/FM1 Community.csv").write_text(
                HEADER + "M-VAVE,FM1 Community,303 engine,Cutoff,,74,,0,127,,,,,,,0-based,,0~127: Frequency\n")
            (src / "M-VAVE/FM1 Community.triggers.csv").write_text(
                TRIG_HEADER + "M-VAVE,FM1 Community,808 engine,Kick,,10,36,36,36,,\n")
            (src / "Bad/Empty.csv").write_text("")
            usb = {"m-vave/fm1-community": [{"name": "FM1 Community", "vid": "3654", "pid": "7756"}]}
            report = mg.import_tree(src, out, "abc123", usb)
            s = report["summary"]
            self.assertEqual((s["csv_files"], s["files_skipped"], s["devices"]), (5, 1, 3))
            self.assertEqual((s["devices_clean"], s["devices_failed"], s["devices_with_value_names"]), (2, 1, 1))
            self.assertEqual(len(report["files"]), 5)
            fm1 = json.loads((out / "M-VAVE/FM1 Community.json").read_text())
            self.assertEqual(fm1["id"], "m-vave/fm1-community")
            self.assertEqual(fm1["usb"][0]["vid"], "3654")
            self.assertEqual(fm1["triggers"][0]["notes"][0]["note"]["min"], 36)
            self.assertEqual(fm1["attribution"]["licence"], "CC-BY-SA-4.0")
            self.assertEqual(fm1["attribution"]["source_commit"], "abc123")
            self.assertIn("Pencil Research", fm1["attribution"]["credit"])
            mf = json.loads((out / "Arturia/MicroFreak.json").read_text())
            self.assertEqual(mf["usb"], [])


if __name__ == "__main__":
    unittest.main()
