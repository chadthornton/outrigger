"""Tests for docs/site-redesign/guide.py: the beta docs as the testers' guide page.

    python3 -I tests/test_guide.py
"""
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "docs/site-redesign"))
from guide import inline, render, write_guide  # noqa: E402


class InlineTest(unittest.TestCase):
    def test_marks(self):
        self.assertEqual(inline("**Hold** *Expected:* `make check`"),
                         "<b>Hold</b> <i>Expected:</i> <code>make check</code>")

    def test_a_page_link_becomes_its_section(self):
        self.assertEqual(inline("[Known issues](07-known-issues.md)"), '<a href="#known-issues">Known issues</a>')

    def test_homepage_links_stay_on_the_site(self):
        self.assertEqual(inline("[here](https://outrigger.music/#install)"), '<a href="/#install">here</a>')

    def test_text_is_escaped_and_code_keeps_its_stars(self):
        self.assertEqual(inline("a < b & `x*y*z`"), "a &lt; b &amp; <code>x*y*z</code>")


class BlockTest(unittest.TestCase):
    def test_list_items_keep_their_continuation_lines(self):
        _, out = render("1. **Install.** Do it.\n   *Expected:* it works.\n2. Next.\n", "x")
        self.assertIn("<ol><li><b>Install.</b> Do it. <i>Expected:</i> it works.</li><li>Next.</li></ol>", out)

    def test_nested_list(self):
        _, out = render("- one\n  - inner\n- two\n", "x")
        self.assertIn("<ul><li>one<ul><li>inner</li></ul></li><li>two</li></ul>", out)

    def test_table(self):
        _, out = render("| A | B |\n|---|---|\n| 1 | **2** |\n", "x")
        self.assertIn("<th>A</th><th>B</th>", out)
        self.assertIn("<td>1</td><td><b>2</b></td>", out)


class GuideTest(unittest.TestCase):
    def test_every_beta_page_renders_cleanly(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "index.html"
            write_guide(ROOT / "docs/beta", out)
            page = out.read_text()
        self.assertEqual(page.count("<section>"), len(list((ROOT / "docs/beta").glob("0*.md"))))
        text = re.sub(r"<code>.*?</code>", "", page)
        for leftover in ["**", "](", "\n# ", "\n## "]:
            self.assertNotIn(leftover, text)
        ids = set(re.findall(r'id="([^"]+)"', page))
        for target in re.findall(r'href="#([^"]+)"', page):
            self.assertIn(target, ids)
        self.assertNotIn("announcement", page.lower())


if __name__ == "__main__":
    unittest.main()
