"""Tests for tools/ecosystem_check.py: the daily ecosystem check's report.

    python3 -I tests/test_ecosystem_check.py
"""
import hashlib
import json
import re
import sys
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
        self.assertEqual(ec.group_of("melodee", ENTRIES, FAM), "blocked")
        self.assertEqual(ec.group_of("hortator", ENTRIES, FAM), "blocked")
        self.assertEqual(ec.group_of("mnm1", ENTRIES, FAM), "blocked")

    def test_the_table_holds_everything_the_add_page_holds(self):
        src = (ROOT / "site/installer/add.js").read_text()
        held = set()
        for name in ("BLOCKED", "OLD_FELUCCA"):
            body = re.search(name + r" = \[(.*?)\];", src, re.S).group(1)
            held |= set(re.findall(r'"([a-z0-9-]+)"', body))
        self.assertTrue(held, "add.js lists moved")
        self.assertEqual(held - set(FAM["blocked_ids"]), set())


class LinkTest(unittest.TestCase):
    def test_the_links_are_the_installers_pins(self):
        links = ec.installer_links()
        urls = [l["url"] for l in links]
        self.assertTrue(any(u.endswith("/wl82loader.bin") for u in urls))
        self.assertIn("https://thefirmwarehub.com/fw/mvave/FM-1_015.fwsc", urls)
        self.assertIn(ec.HUB_CATALOG, urls)
        self.assertIn(ec.MUTANT_CATALOG, urls)
        self.assertTrue(all(l["sha256"] for l in links if not l["json"]))

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

    def test_the_page_map_is_read_from_add_js(self):
        self.assertIsInstance(ec.hub_ids_in_page(), dict)


class ReportTest(unittest.TestCase):
    def test_broken_links_first_then_riskiest_releases(self):
        feed = [item("x0x", "v1.3"), item("orbit", "v2"), item("fmel", "v1.2"), item("penumbra", "v1.2")]
        body, _, attention = ec.build_report([("Stock V15", "https://h/fw", "answered 404")], feed, ENTRIES, FAM,
                                             {"date": "2026-11-03", "seen": [], "missing": []}, "2026-11-04")
        order = [body.index(s) for s in ("BROKEN: Stock V15", "penumbra v1.2", "orbit v2", "fmel v1.2", "x0x v1.3")]
        self.assertEqual(order, sorted(order))
        self.assertTrue(attention)

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
