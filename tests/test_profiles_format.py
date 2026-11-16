"""Tests for the profile format (tools/profiles/schema.py) and Felucca's profile (tools/profiles/felucca.py).

    python3 -I tests/test_profiles_format.py

Felucca's expected CC74 / CC71 targets below are written out by hand from Felucca 1.7.0's sources
(midi_control.c MIDI_CC_ENG decoded against each engine's .edit table), independently of the generator.
"""
import copy
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools/profiles"))
import felucca  # noqa: E402
import midiguide as mg  # noqa: E402
import schema  # noqa: E402

FELUCCA_JSON = ROOT / "data/profiles/felucca/Felucca.json"
SAMPLES = sorted((ROOT / "data/profiles/midi-guide").rglob("*.json"))

# engine -> (CC74 target, CC71 target), None = ignored. By hand from MIDI_CC_ENG's comments and the .edit tables.
FELUCCA_ENGINE_CCS = {
    "analog": ("CUT", "RES"), "fm6": ("MLVL", None), "phase": ("DCW", None), "lofi": ("TONE", None),
    "sample": ("CUT", None), "voice": (None, "Q"), "trio": ("CUT", "RES"), "wheel": (None, None),
    "grain": ("TONE", None), "phys": ("BRIT", None), "noise": ("FREQ", "RES"), "slice": ("TONE", None),
    "drum": ("TONE", None),
}

# A controller, synthetic (not any real product's numbers): two zones, and pads whose CCs depend on a layer.
CONTROLLER = {
    "schema": "outrigger.profile/1", "id": "test/synthetic-controller", "manufacturer": "Test",
    "device": "Synthetic controller", "kind": "controller", "usb": [],
    "parts": [{"id": "keys", "name": "Keys zone"}, {"id": "pads", "name": "Pads zone"}],
    "channel_modes": {"observable": False, "default": "split", "modes": [
        {"id": "split", "name": "Split", "kind": "block", "channels": {"1": "keys", "10": "pads"}}]},
    "sections": [{"name": "Knobs", "parameters": [
        {"name": "Knob 1", "cc": {"msb": 21, "lsb": None, "min": 0, "max": 127, "default": None},
         "nrpn": None, "usage": [], "usage_units": None, "orientation": "0-based", "scope": "part"}]}],
    "variant_sets": [{"id": "pad-layer", "name": "Pad layer", "scope": "part", "default": "a",
                      "controls": {"cc": [40, 41]}, "selected_by": {"observable": True, "how": "Sent as CC 3."},
                      "variants": [
                          {"id": "a", "name": "A", "sections": [{"name": "Pads", "parameters": [
                              {"name": "Pad 1", "cc": {"msb": 40, "lsb": None, "min": 0, "max": 127, "default": None},
                               "usage": [], "orientation": "0-based"}]}]},
                          {"id": "b", "name": "B", "sections": []}]}],
    "triggers": [], "attribution": {"licence": "GPL-3.0-only", "source": "tests/test_profiles_format.py"},
}


def felucca_profile():
    return json.loads(FELUCCA_JSON.read_text(encoding="utf-8"))


class Validator(unittest.TestCase):
    def test_committed_profiles_are_valid(self):
        self.assertEqual(len(SAMPLES), 4)
        for f in SAMPLES + [FELUCCA_JSON]:
            with self.subTest(f=f.name):
                self.assertEqual(schema.validate(json.loads(f.read_text(encoding="utf-8"))), [])

    def test_importer_writes_v1_and_it_validates(self):
        row = "Arturia,MicroFreak,Filter,Cutoff,,23,,0,127,,,,,,,0-based,,\n"
        profile, _ = mg.import_device("Arturia", "MicroFreak", ",".join(mg.CC_COLUMNS) + "\n" + row)
        self.assertEqual(profile["schema"], "outrigger.profile/1")
        self.assertEqual(schema.validate(profile), [])

    def test_v0_still_valid(self):
        p = json.loads(SAMPLES[0].read_text(encoding="utf-8"))
        p["schema"] = "outrigger.profile/0"
        self.assertEqual(schema.validate(p), [])

    def test_controller_fixture_is_valid(self):
        self.assertEqual(schema.validate(CONTROLLER), [])
        params, ignored = schema.resolve(CONTROLLER, {"pad-layer": "b"})
        self.assertEqual([p["name"] for p in params], ["Knob 1"])
        self.assertEqual(ignored, ["cc:40", "cc:41"])
        self.assertEqual(schema.route(CONTROLLER, "split", 10), "pads")

    def broken(self, mutate, expect, base=None):
        p = copy.deepcopy(base or felucca_profile())
        mutate(p)
        errs = schema.validate(p)
        self.assertTrue(any(expect in e for e in errs), f"wanted {expect!r} in {errs}")

    def test_rejects(self):
        vs = lambda p: p["variant_sets"][0]
        analog = lambda p: vs(p)["variants"][0]["sections"][0]["parameters"]
        cases = [
            (lambda p: p.update(schema="outrigger.profile/9"), "schema"),
            (lambda p: p.update(schema="outrigger.profile/0"), "need schema outrigger.profile/1"),
            (lambda p: p.update(kind="pedal"), "kind"),
            (lambda p: p["sections"][1]["parameters"][0]["cc"].update(msb=200), "not an integer 0..127"),
            (lambda p: p["sections"][1]["parameters"][0]["cc"].update(msb=74), "in the base sections and in the variants"),
            (lambda p: analog(p)[0]["cc"].update(msb=20), "is not one of the set's controls"),
            (lambda p: analog(p)[1]["cc"].update(msb=analog(p)[0]["cc"]["msb"]), "given twice"),
            (lambda p: vs(p)["selected_by"].pop("observable"), "observable"),
            (lambda p: vs(p).update(default="moog"), "default"),
            (lambda p: vs(p)["variants"][-1].update(ignores=["sysex"]), "ignores"),
            (lambda p: p["channel_modes"]["modes"][0]["channels"].update({"1": "t9"}), "unknown part"),
            (lambda p: p["channel_modes"]["modes"][0]["channels"].update({"17": "t1"}), "is not 1..16"),
            (lambda p: p["channel_modes"]["modes"][0]["channels"].update({"5": "t1"}), "two channels reach one part"),
            (lambda p: p["channel_modes"]["modes"][5]["defaults"].pop("t4"), "every part a channel"),
            (lambda p: p["channel_modes"]["modes"][5]["defaults"].update(t4=0), "is not 1..16, omni or off"),
            (lambda p: p["channel_modes"].update(default="ch2-5"), "is not a mode"),
            (lambda p: p.pop("parts"), "channel_modes needs parts"),
            (lambda p: p["sections"][1]["parameters"][2]["target"].update(default=99), "outside -64..63"),
            (lambda p: p["attribution"].pop("licence"), "attribution"),
            (lambda p: p["usb"][0].update(vid="12099"), "usb[0]"),
            (lambda p: p["messages"].update(sysex={}), "messages"),
        ]
        for mutate, expect in cases:
            with self.subTest(expect=expect):
                self.broken(mutate, expect)


class Felucca(unittest.TestCase):
    def test_profile_matches_the_sources(self):
        self.assertEqual(FELUCCA_JSON.read_text(encoding="utf-8"), felucca.render(felucca.build()),
                         "stale: run python3 -I tools/profiles/felucca.py")

    def test_engine_ccs_by_hand(self):
        p = felucca_profile()
        ids = [v["id"] for v in p["variant_sets"][0]["variants"]]
        self.assertEqual(sorted(ids), sorted(FELUCCA_ENGINE_CCS))           # 13 engines, DIGITAL left out
        for eng, (want74, want71) in FELUCCA_ENGINE_CCS.items():
            with self.subTest(engine=eng):
                params, ignored = schema.resolve(p, {"engine": eng})
                by_cc = {q["cc"]["msb"]: q["name"] for q in params if q["cc"]}
                self.assertEqual((by_cc.get(74), by_cc.get(71)), (want74, want71))
                self.assertEqual("cc:74" in ignored, want74 is None)
                self.assertEqual("cc:71" in ignored, want71 is None)
                self.assertEqual(len(by_cc), len([q for q in params if q["cc"]]), "a CC twice in one view")

    def test_standard_map_and_ranges(self):
        p = felucca_profile()
        std = {q["cc"]["msb"]: q for q in p["sections"][1]["parameters"]}
        self.assertEqual(sorted(std), [5, 7, 10, 72, 73, 75, 91, 93, 94])
        self.assertEqual(std[10]["target"], {"min": -64, "max": 63, "default": 0, "format": "F_BIPCT"})
        self.assertEqual(std[10]["orientation"], "centered")
        fm6, _ = schema.resolve(p, {"engine": "fm6"})
        mlvl = next(q for q in fm6 if q["name"] == "MLVL")
        self.assertEqual((mlvl["target"]["min"], mlvl["target"]["max"]), (-64, 63))

    def test_drum_ignores_bend_and_sustain(self):
        _, ignored = schema.resolve(felucca_profile(), {"engine": "drum"})
        self.assertIn("pitch_bend", ignored)
        self.assertIn("cc:64", ignored)
        _, ignored = schema.resolve(felucca_profile(), {"engine": "analog"})
        self.assertEqual(ignored, [])

    def test_learn_never(self):
        want = [0, 1, 6, 11, 32, 38, 64, *range(96, 102), *range(120, 128)]
        self.assertEqual(felucca_profile()["learn"]["never"], want)
        with self.assertRaises(ValueError):
            felucca.learn_never("static int ml_free_cc(uint32_t cc) { return cc < 100u; }")

    def test_anchors_hold_and_catch_a_change(self):
        mc = (felucca.SRC / "midi_control.c").read_text(encoding="utf-8")
        self.assertEqual([w for w, ok in felucca.anchors(mc) if not ok], [])
        moved = dict(felucca.anchors(mc.replace("#define ML_N 16u", "#define ML_N 32u")))
        self.assertFalse(moved["midi_control.c ML_N 16"])
        modes = felucca_profile()["channel_modes"]["modes"]
        self.assertEqual([m["name"] for m in modes], ["CH1-4", "SEL", "CH5-8", "CH9-12", "CH13-16", "TRACK"])

    def test_usb_is_felucca_not_the_updater(self):
        self.assertEqual([(u["vid"], u["pid"]) for u in felucca_profile()["usb"]], [("1209", "0001")])


class Route(unittest.TestCase):
    def setUp(self):
        self.p = felucca_profile()

    def test_blocks(self):
        r = lambda m, ch: schema.route(self.p, m, ch)
        self.assertEqual([r("ch1-4", c) for c in (1, 4, 5, 16)], ["t1", "t4", None, None])
        self.assertEqual([r("ch5-8", c) for c in (4, 5, 8, 9)], [None, "t1", "t4", None])
        self.assertEqual([r("ch13-16", c) for c in (12, 13, 16)], [None, "t1", "t4"])

    def test_sel(self):
        self.assertEqual(schema.route(self.p, "sel", 11, selected="t3"), "t3")

    def test_track(self):
        r = lambda ch, pc=None: schema.route(self.p, "track", ch, pc)
        self.assertEqual([r(c) for c in (1, 2, 4, 5)], ["t1", "t2", "t4", None])
        pc = {"t1": 9, "t2": 9, "t3": "omni", "t4": "omni"}
        self.assertEqual(r(9, pc), "t1")          # the first track set to the channel
        self.assertEqual(r(3, pc), "t3")          # else the first set to OMNI
        self.assertIsNone(r(3, {"t1": "off", "t2": "off", "t3": 2, "t4": 4}))


if __name__ == "__main__":
    unittest.main(verbosity=1)
