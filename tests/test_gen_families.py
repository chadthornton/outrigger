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
        self.assertEqual(b["melodee"], "bluetooth")
        self.assertNotIn("skiff", b)

    def test_built_on_a_blocked_firmware_through_any_depth(self):
        b = self.blocked({"orange-cat": "melodee", "kitten": "orange-cat", "tribe": "x0x", "x0x": "felucca",
                          "deep": "hortator"})
        self.assertEqual(b["kitten"], "bluetooth")
        self.assertEqual(b["deep"], "old-felucca")
        self.assertNotIn("tribe", b)

    def test_a_blocked_firmwares_other_files_are_blocked_too(self):
        b = self.blocked({"tessera": "melodee", "ctl": None}, variants={"tessera-fm-1-83773022": "tessera",
                                                                        "ctl-usb": "ctl"})
        self.assertEqual(b["tessera-fm-1-83773022"], "bluetooth")
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
        for project in ("sloop", "orbit", "mycelium", "tessera-fm-1-83773022"):
            self.assertIn(f'"{project}"', src)
        self.assertIn('"b1-drawbar-organ": "b1"', src)

    def test_loops_and_unknown_bases_end(self):
        bases = {"fomni": ["fomni"], "a": ["b"], "b": ["a"], "new": ["brandnew"]}
        for i in bases:
            self.assertEqual(g.category(i, bases, FAM), "unknown")

    def test_the_committed_header_is_current(self):
        self.assertEqual(g.main(["--check"]), 0)


if __name__ == "__main__":
    unittest.main()
