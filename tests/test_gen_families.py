"""Tests for tools/gen_families.py: which projects the loader blocks, and why.

    python3 -I tests/test_gen_families.py
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gen_families as g  # noqa: E402

FAM = g.load(g.FAMILIES)


class BlockTest(unittest.TestCase):
    def blocked(self, mutant, hub=None, variants=None):
        return g.blocked_projects(FAM, {"mutant": mutant, "hub": hub or {}, "variants": variants or {}})

    def test_a_block_from_either_catalog_counts(self):
        # The Hub says built on Felucca and SLOOP; mutant.cloud says nothing.
        b = self.blocked({"mycelium": None}, {"mycelium": ["felucca", "sloop"]})
        self.assertEqual(b["mycelium"], "old-felucca")

    def test_our_survey_beats_the_catalogs(self):
        b = self.blocked({"jangada": "felucca", "melodee": "felucca", "skiff": "felucca"})
        self.assertEqual(b["jangada"], "old-felucca")
        self.assertNotIn("melodee", b)  # riskier, by our survey
        self.assertNotIn("skiff", b)

    def test_built_on_a_blocked_firmware_through_any_depth(self):
        b = self.blocked({"orange-cat": "melodee", "kitten": "orange-cat", "tribe": "x0x", "x0x": "felucca",
                          "deep": "clipper"})
        self.assertEqual(b["kitten"], "bluetooth")
        self.assertEqual(b["deep"], "bluetooth")  # built on a riskier firmware: unreviewed, held
        self.assertNotIn("tribe", b)

    def test_a_cleared_id_beats_a_blocked_base(self):
        # Our survey read their flash maps: SLOOP and Felucca 0.9 forks that write only inside the clean slate.
        b = self.blocked({"sloopdx": "sloop", "hortator": "felucca", "mnm1": "hortator", "newfork": "hortator"},
                         {"mnm1": ["hortator"]}, {"hortator-fm-1-900": "hortator"})
        for i in ("sloopdx", "hortator", "mnm1", "newfork", "hortator-fm-1-900"):
            self.assertNotIn(i, b)

    def test_the_survey_sorts_the_snapshot(self):
        b = g.blocked_projects(FAM, g.load(g.LINEAGE))
        for i in ("sloopdx", "salt", "hortator", "mnm1", "nauclerus", "hawkchua"):
            self.assertNotIn(i, b)
        # Written outside the clean slate, or no source: still held.
        for i in ("jangada", "vector37", "gator", "mycelium"):
            self.assertEqual(b[i], "old-felucca")
        # Built on SLOOP or Melodee and not reviewed: held.
        for i in ("orbit", "sloop-lab", "merthsloop", "optimist", "dock", "orange-cat", "melodee-waveloop-build"):
            self.assertIn(i, b)
        # SLOOP's sample slot USR4 (0xE7000-0xFAFFF) crosses 0xE9000, in its forks too; Melodee and
        # Tessera save there: riskier, not held.
        r = g.riskier_projects(FAM, g.load(g.LINEAGE))
        for i in ("sloop", "clipper", "spampy", "winampfm", "floyd", "floyd-felucca", "floyd-sloop", "nmsfm",
                  "karma", "sloopbanks", "fm1quest", "melodee", "tessera", "tessera-fm-1-83773022"):
            self.assertNotIn(i, b)
            self.assertIn(i, r)
        self.assertFalse(set(r) & set(b))

    def test_an_id_cant_be_both_cleared_and_blocked(self):
        fam = dict(FAM, cleared_ids=dict(FAM["cleared_ids"], sloop="no"))
        with self.assertRaises(ValueError):
            g.blocked_projects(fam, {"mutant": {}, "hub": {}, "variants": {}})

    def test_an_id_cant_be_both_riskier_and_held_or_cleared(self):
        for key in ("blocked_ids", "cleared_ids"):
            fam = dict(FAM, **{key: dict(FAM[key], sloop="no")})
            with self.assertRaises(ValueError):
                g.blocked_projects(fam, {"mutant": {}, "hub": {}, "variants": {}})

    def test_a_riskier_firmwares_other_files_and_hub_id_are_riskier_too(self):
        fam = dict(FAM, hub_ids={"floyd": "floyd-hub"})
        lineage = {"mutant": {"tessera": "melodee", "floyd": "sloop"}, "hub": {},
                   "variants": {"tessera-fm-1-83773022": "tessera"}}
        r = g.riskier_projects(fam, lineage)
        for i in ("tessera", "tessera-fm-1-83773022", "floyd", "floyd-hub"):
            self.assertIn(i, r)
        self.assertNotIn("tessera-fm-1-83773022", g.blocked_projects(fam, lineage))

    def test_every_riskier_entry_says_why_and_cites_a_source(self):
        for project, entry in FAM["riskier_ids"].items():
            self.assertTrue(entry.get("why") and entry.get("source"), project)

    def test_a_blocked_firmwares_other_files_are_blocked_too(self):
        b = self.blocked({"orange-cat": "melodee", "ctl": None}, variants={"orange-cat-ble": "orange-cat",
                                                                           "ctl-usb": "ctl"})
        self.assertEqual(b["orange-cat-ble"], "bluetooth")
        self.assertNotIn("ctl-usb", b)

    def test_a_renamed_blocked_firmware_is_blocked_under_the_hubs_id_too(self):
        fam = dict(FAM, hub_ids={"orbit-renamed": "orbit-hub"})
        b = g.blocked_projects(fam, {"mutant": {"orbit-renamed": "sloop"}, "hub": {}, "variants": {}})
        self.assertEqual(b["orbit-hub"], "old-felucca")

    def test_an_unreviewed_co_parent_makes_it_unknown(self):
        bases = {"estampie": ["felucca", "ghoulbox"], "tuna": ["felucca"], "sill": ["felucca", "tuna"]}
        self.assertEqual(g.category("estampie", bases, FAM), "unknown")
        self.assertEqual(g.category("sill", bases, FAM), "keep")

    def test_the_page_gets_the_boards_list(self):
        src = g.PAGE.read_text()
        for project in ("orbit", "mycelium", "orange-cat"):
            self.assertIn(f'"{project}"', src)
        self.assertRegex(src, r'RISKIER = \[[^\]]*"tessera-fm-1-83773022"')
        # A fork added since the snapshot is held by its base: SLOOP's and Melodee's too.
        self.assertRegex(src, r'HELD_BASES = \{[^}]*"sloop": "old-felucca"')
        self.assertRegex(src, r'HELD_BASES = \{[^}]*"clipper": "bluetooth"')
        self.assertIn('"b1-drawbar-organ": "b1"', src)
        self.assertRegex(src, r'CLEARED = \[[^\]]*"sloopdx"')

    def test_loops_and_unknown_bases_end(self):
        bases = {"fomni": ["fomni"], "a": ["b"], "b": ["a"], "new": ["brandnew"]}
        for i in bases:
            self.assertEqual(g.category(i, bases, FAM), "unknown")

    def test_the_board_gets_the_uc25hq80_save_list(self):
        src = g.header({}, {"uc25hq80_saves": {"stock": {"since": "", "source": "x"},
                                               "skiff": {"since": "2.2", "source": "y"}}})
        self.assertIn('{"skiff", "2.2"},', src)
        self.assertIn('{"stock", ""},', src)
        real = g.HEADER.read_text()
        self.assertRegex(real, r'kRiskierIdentity\[\] = \{[^}]*"sloop",')
        for project, since in (("felucca", "1.8.2-beta"), ("melodee", "1.6.0"), ("skiff", "2.2"),
                               ("sloop", "2.8.5"), ("stock", "")):
            self.assertIn(f'{{"{project}", "{since}"}},', real)

    def test_every_save_entry_cites_a_source(self):
        for project, entry in FAM["uc25hq80_saves"].items():
            if project.startswith("_"):
                continue
            self.assertTrue(entry.get("source"), project)

    def test_the_committed_header_is_current(self):
        self.assertEqual(g.main(["--check"]), 0)


if __name__ == "__main__":
    unittest.main()
