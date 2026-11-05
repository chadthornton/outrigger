"""Tests for tools/ecosystem_check.py: the daily ecosystem check's report.

    python3 -I tests/test_ecosystem_check.py
"""
import hashlib
import json
import re
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import ecosystem_check as ec  # noqa: E402

FAM = ec.load_families()
ENTRIES = {
    "felucca": {"id": "felucca", "base": None},
    "x0x": {"id": "x0x", "base": "felucca"},
    "tribe": {"id": "tribe", "base": "x0x"},
    "hortator": {"id": "hortator", "base": "felucca"},
    "mnm1": {"id": "mnm1", "base": "hortator"},
    "melodee": {"id": "melodee", "base": "felucca"},
    "orange-cat": {"id": "orange-cat", "base": "melodee"},
    "orbit": {"id": "orbit", "base": "sloop"},
    "fmel": {"id": "fmel", "base": "dinghy"},
    "ravel": {"id": "ravel", "base": "original"},
    "fomni": {"id": "fomni", "base": "fomni"},
    "penumbra": {"id": "penumbra", "base": None},
    "newfam": {"id": "newfam", "base": "brandnew"},
    "jiant": {"id": "jiant", "base": "felucca", "release": {"assets": [
        {"name": "felucca-V10.9.fwsc", "product": "FM-1_909", "missing": "2026-11-01"},
        {"name": "skiff-smk.fwsc", "product": "SMK-37 Elite_722", "missing": "2026-11-01"}]}},
}


def shown(body):
    """The report as GitHub shows it, without the hidden state."""
    return ec.STATE_RE.sub("", body)


def item(fid, ver, date="2026-11-04"):
    return {"guid": f"mutant.cloud/{fid}@{ver}", "id": fid, "title": f"{fid} {ver}",
            "link": f"https://mutant.cloud/{fid}", "date": date}


class FamilyTest(unittest.TestCase):
    def test_groups_follow_built_on_through_firmwares_that_are_bases(self):
        g = lambda i: ec.group_of(i, ENTRIES, FAM)  # noqa: E731
        self.assertEqual(g("felucca"), "keep")
        self.assertEqual(g("stock"), "keep")
        self.assertEqual(g("tribe"), "keep")  # tribe -> x0x -> felucca
        self.assertEqual(g("fmel"), "clean")
        self.assertEqual(g("ravel"), "clean")
        self.assertEqual(g("orbit"), "blocked")
        self.assertEqual(g("orange-cat"), "blocked")
        self.assertEqual(g("penumbra"), "unknown")
        self.assertEqual(g("fomni"), "unknown")  # its own base: no loop
        self.assertEqual(g("newfam"), "unknown")

    def test_our_ids_beat_what_mutant_cloud_says(self):
        # Melodee and the Felucca 0.9 survey are listed as built on Felucca.
        entries = dict(ENTRIES, jangada={"id": "jangada", "base": "felucca"},
                       sloopdx={"id": "sloopdx", "base": "sloop"})
        self.assertEqual(ec.group_of("melodee", entries, FAM), "blocked")
        self.assertEqual(ec.group_of("jangada", entries, FAM), "blocked")
        # Cleared by the survey: a clean slate, whatever their base, and so are firmwares built on them.
        self.assertEqual(ec.group_of("hortator", entries, FAM), "clean")
        self.assertEqual(ec.group_of("mnm1", entries, FAM), "clean")
        self.assertEqual(ec.group_of("sloopdx", entries, FAM), "clean")

    def test_a_block_from_the_hubs_base_counts_too(self):
        hub = {"firmwares": [{"id": "mycelium", "base": ["felucca", "sloop"]}]}
        entries = dict(ENTRIES, mycelium={"id": "mycelium", "base": "felucca"})
        self.assertEqual(ec.group_of("mycelium", entries, FAM, ec.bases_of(entries, hub)), "blocked")

    def test_the_board_snapshot_going_stale_is_flagged(self):
        entries = {"newfork": {"id": "newfork", "base": "sloop"}, "x0x": {"id": "x0x", "base": "felucca"}}
        stale = ec.unblocked_on_board(FAM, entries, {"firmwares": []}, {}, committed=set(FAM["blocked_ids"]))
        self.assertEqual(stale, ["newfork"])
        body, state, attention = ec.build_report([], [], {}, FAM, {"seen": ["z"]}, "2026-11-04", stale=stale)
        self.assertTrue(attention)
        self.assertIn("Not blocked on the board yet:** newfork", body)
        _, _, attention = ec.build_report([], [], {}, FAM, state, "2026-11-05", stale=stale)
        self.assertFalse(attention)  # said once

    def test_a_cleared_firmwares_new_release_is_flagged(self):
        # cleared_ids holds for the release whose flash map we read: a newer one
        # (sloopbanks 2.3 -> 2.5 took SLOOP's USR4 slot) needs reading again.
        read = FAM["cleared_ids"]["sloopdx"]["version"]
        entries = {"sloopdx": {"id": "sloopdx", "base": "sloop", "release": {"version": "v9.9"}},
                   "hortator": {"id": "hortator", "release": {"version": FAM["cleared_ids"]["hortator"]["version"]}},
                   "salt": {"id": "salt"}}  # no release listed: nothing to compare
        moved = ec.cleared_moved(FAM, entries)
        self.assertEqual(moved, [f"sloopdx {read} -> v9.9"])
        body, state, attention = ec.build_report([], [], {}, FAM, {"seen": ["z"]}, "2026-11-04", moved=moved)
        self.assertTrue(attention)
        self.assertIn("New release of a cleared firmware:** sloopdx", body)
        _, _, attention = ec.build_report([], [], {}, FAM, state, "2026-11-05", moved=moved)
        self.assertFalse(attention)  # said once


class LinkTest(unittest.TestCase):
    def test_the_links_are_the_installers_pins(self):
        links = ec.installer_links()
        urls = [l["url"] for l in links]
        self.assertTrue(any(u.endswith("/wl82loader.bin") for u in urls))
        self.assertIn("https://thefirmwarehub.com/fw/mvave/FM-1_015.fwsc", urls)
        self.assertIn(ec.HUB_CATALOG, urls)
        self.assertIn(ec.MUTANT_CATALOG, urls)
        self.assertTrue(all(l["sha256"] for l in links if not l["json"]))

    def test_the_fixture_links_are_fetch_fixtures_pins(self):
        links = ec.fixture_links()
        names = [l["name"] for l in links]
        self.assertIn("FM-1_015.fwsc", names)
        self.assertIn("wl82loader.bin", names)
        for l in links:
            self.assertTrue(l["url"].startswith("https://"), l)
            self.assertRegex(l["sha256"], r"^[0-9a-f]{64}$")

    def test_constants_follow_names_and_joined_strings(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "pins.py"
            src.write_text('import os\nA = "https://x/"\nB = A + "rel/"\nC = os.getcwd()\n'
                           'L = [("f", None, B + "f.bin", "ab")]\n')
            self.assertEqual(ec._constants(src, {"L", "C"}), {"L": [("f", None, "https://x/rel/f.bin", "ab")]})

    def test_check_link(self):
        blob = b"loader"
        link = {"name": "x", "url": "u", "sha256": hashlib.sha256(blob).hexdigest(), "json": False}
        self.assertIsNone(ec.check_link(link, lambda u: blob))
        self.assertIn("changed", ec.check_link(link, lambda u: b"other"))

        def gone(u):
            raise urllib.error.HTTPError(u, 404, "Not Found", {}, None)
        self.assertEqual(ec.check_link(link, gone), "answered 404")
        cat = {"name": "c", "url": "u", "sha256": None, "json": True}
        self.assertIsNone(ec.check_link(cat, lambda u: b'{"firmwares": [1]}'))
        self.assertIn("no firmwares", ec.check_link(cat, lambda u: b'{"firmwares": []}'))
        self.assertIn("not with JSON", ec.check_link(cat, lambda u: b"<html>"))


class RenameTest(unittest.TestCase):
    def test_renames_by_repo_unless_the_page_maps_them(self):
        hub = {"firmwares": [{"id": "b1", "repo": "Foo/B1"}, {"id": "skiff", "repo": "illuminus5/skiff"},
                             {"id": "fm1sid", "repo": "x/sid"}]}
        mutant = {"firmwares": [{"id": "b1-drawbar-organ", "entry": "b1-drawbar-organ", "repo": "foo/b1"},
                                {"id": "skiff-fm-1-722", "entry": "skiff", "repo": "illuminus5/skiff"},
                                {"id": "fm1-sid", "entry": "fm1-sid", "repo": "x/sid"},
                                {"id": "new", "entry": "new", "repo": "y/new"}]}
        self.assertEqual(ec.renames(hub, mutant, {"fm1-sid": "fm1sid"}), ["b1-drawbar-organ (Hub: b1)"])

    def test_the_page_map_lives_in_the_families_table(self):
        self.assertEqual(FAM["hub_ids"]["b1-drawbar-organ"], "b1")


class ReportTest(unittest.TestCase):
    def test_broken_links_first_then_riskiest_releases(self):
        feed = [item("x0x", "v1.3"), item("orbit", "v2"), item("fmel", "v1.2"), item("penumbra", "v1.2")]
        body, _, attention = ec.build_report([("Stock V15", "https://h/fw", "answered 404")], feed, ENTRIES, FAM,
                                             {"date": "2026-11-03", "seen": [], "missing": []}, "2026-11-04")
        order = [body.index(s) for s in ("BROKEN: Stock V15", "penumbra v1.2", "orbit v2", "fmel v1.2", "x0x v1.3")]
        self.assertEqual(order, sorted(order))
        self.assertTrue(attention)

    def test_a_broken_fixture_has_its_own_section(self):
        bad = [("felucca-1.8.0.fwsc", "https://g/felucca-1.8.0.fwsc", "answered 404")]
        body, _, attention = ec.build_report([], [], {}, FAM, {"seen": ["z"]}, "2026-11-05", fixtures_bad=bad)
        links, fixtures = body.split("## Test fixtures")[0], body.split("## Test fixtures")[1].split("## New")[0]
        self.assertIn("All answer with the files the installer expects.", links)
        self.assertIn("BROKEN: felucca-1.8.0.fwsc** answered 404", fixtures)
        self.assertIn("make test", fixtures)
        self.assertTrue(attention)
        comment = ec.attention_comment([], "chadthornton", bad)
        self.assertIn("test fixture link is broken", comment)
        self.assertNotIn("Installer", comment)

    def test_only_what_the_last_run_didnt_see_is_new(self):
        feed = [item("x0x", "v1.3"), item("x0x", "v1.2")]
        _, state, _ = ec.build_report([], feed, ENTRIES, FAM, {}, "2026-11-04")
        body, _, attention = ec.build_report([], feed + [item("fmel", "v1.3")], ENTRIES, FAM, state, "2026-11-05")
        self.assertNotIn("x0x", shown(body))
        self.assertIn("fmel v1.3", body)
        self.assertFalse(attention)  # a clean-slate release is news, not a notification

    def test_first_run_counts_only_the_last_day(self):
        feed = [item("x0x", "v1.3", "2026-11-04"), item("fmel", "v1.0", "2026-10-20")]
        body, _, _ = ec.build_report([], feed, ENTRIES, FAM, {}, "2026-11-04")
        self.assertIn("x0x v1.3", body)
        self.assertNotIn("fmel", shown(body))

    def test_many_versions_of_one_firmware_are_one_line(self):
        feed = [item("x0x", f"v1.{n}") for n in range(9, 0, -1)]
        body, _, _ = ec.build_report([], feed, ENTRIES, FAM, {"seen": ["z"]}, "2026-11-04")
        self.assertIn("x0x v1.9](https://mutant.cloud/x0x) (and 8 earlier)", body)
        self.assertEqual(body.count("x0x v1."), 1)

    def test_attention_for_a_missing_file_or_a_family_nobody_knows_once(self):
        state = {"seen": ["z"], "missing": []}
        body, state2, attention = ec.build_report([], [], ENTRIES, FAM, state, "2026-11-04")
        self.assertTrue(attention)
        self.assertIn("jiant felucca-V10.9.fwsc", body)
        self.assertNotIn("skiff-smk", body)  # SMK-37 files aren't ours
        _, _, attention = ec.build_report([], [], ENTRIES, FAM, state2, "2026-11-05")
        self.assertFalse(attention)  # said once
        _, _, attention = ec.build_report([], [item("newfam", "v1")], ENTRIES, FAM, state2, "2026-11-05")
        self.assertTrue(attention)
        _, _, attention = ec.build_report([], [item("penumbra", "v2")], ENTRIES, FAM, state2, "2026-11-05")
        self.assertFalse(attention)  # no base listed: shown, not notified

    def test_release_titles_cant_mention_or_mark_up(self):
        feed = [dict(item("x0x", "v1"), title="X0X @someone [click](http://evil) <!-- x -->")]
        body, _, _ = ec.build_report([], feed, ENTRIES, FAM, {"seen": ["z"]}, "2026-11-04")
        line = next(l for l in body.splitlines() if "X0X" in l)
        self.assertNotIn("@someone", line)
        self.assertNotIn("](http://evil)", line)
        self.assertNotIn("<!--", line)
        self.assertIn("](https://mutant.cloud/x0x)", line)

    def test_the_state_remembers_the_feed_window_only(self):
        _, state, _ = ec.build_report([], [item("x0x", "v2")], ENTRIES, FAM, {"seen": ["mutant.cloud/old@v1"]}, "2026-11-04")
        self.assertEqual(state["seen"], ["mutant.cloud/x0x@v2"])

    def test_a_rename_is_flagged_once(self):
        r = ["b1-drawbar-organ (Hub: b1)"]
        body, state, attention = ec.build_report([], [], {}, FAM, {"seen": ["z"]}, "2026-11-04", r)
        self.assertTrue(attention)
        self.assertIn("Renamed on mutant.cloud:** b1-drawbar-organ (Hub: b1)", body)
        _, _, attention = ec.build_report([], [], {}, FAM, state, "2026-11-05", r)
        self.assertFalse(attention)

    def test_the_comment_mentions_the_owner(self):
        self.assertTrue(ec.attention_comment([], "chadthornton").startswith("@chadthornton "))

    def test_state_rides_in_the_body(self):
        body, state, _ = ec.build_report([], [item("x0x", "v1")], ENTRIES, FAM, {}, "2026-11-04")
        self.assertEqual(json.loads(ec.STATE_RE.search(body).group(1)), state)

    def test_feed(self):
        xml = b"""<rss><channel>
          <item><title>Penumbra v1.2</title><link>https://mutant.cloud/penumbra</link>
            <guid isPermaLink="false">mutant.cloud/penumbra@v1.2</guid><pubDate>Wed, 04 Nov 2026 00:00:00 GMT</pubDate></item>
          <item><title>odd</title><guid>elsewhere</guid></item></channel></rss>"""
        self.assertEqual(ec.parse_feed(xml), [{"guid": "mutant.cloud/penumbra@v1.2", "id": "penumbra",
                                               "title": "Penumbra v1.2", "link": "https://mutant.cloud/penumbra",
                                               "date": "2026-11-04"}])


if __name__ == "__main__":
    unittest.main()
