"""The beta testers' guide: docs/beta/0*.md as one page, site/beta/index.html. build.py calls write_guide().

The Markdown is the small subset those pages use: headings, paragraphs, lists (numbered or not, with indented
continuation lines), one level of nested list, tables, **bold**, *italic*, `code` and links. A link to another
page (03-test-plan.md) becomes that page's section on the guide (#test-plan).
"""
import html
import re
from pathlib import Path

LIST_ITEM = re.compile(r"^( *)(\d+\.|-) +(.*)$")


def slug(md_path):
    """03-test-plan.md -> test-plan"""
    return re.sub(r"^\d+-", "", Path(md_path).stem)


def inline(text):
    text = html.escape(text, quote=False)
    codes = []
    text = re.sub(r"`([^`]+)`", lambda m: codes.append(m[1]) or f"\0{len(codes) - 1}\0", text)

    def link(m):
        label, href = m[1], m[2]
        if re.fullmatch(r"\d+-[\w-]+\.md", href):
            href = "#" + slug(href)
        elif href.startswith("https://outrigger.music/"):
            href = "/" + href.removeprefix("https://outrigger.music/")
        attrs = ' target="_blank" rel="noopener"' if href.startswith("http") else ""
        return f'<a href="{html.escape(href)}"{attrs}>{label}</a>'

    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"<i>\1</i>", text)
    return re.sub("\0(\\d+)\0", lambda m: f"<code>{codes[int(m[1])]}</code>", text)


def _blocks(lines):
    """Split into blocks: runs of non-blank lines, except that indented lines after a list stay with it."""
    block = []
    for line in lines:
        if not line.strip():
            if block:
                yield block
            block = []
        elif block and line.startswith("#"):
            yield block
            block = [line]
        else:
            block.append(line)
    if block:
        yield block


def _list(lines):
    """A list block, as (indent, ordered, text, children) items, children one level deep."""
    items = []
    for line in lines:
        m = LIST_ITEM.match(line)
        if m and (not items or len(m[1]) <= items[0][0]):
            items.append([len(m[1]), m[2] != "-", m[3], []])
        elif m and items:
            items[-1][3].append([len(m[1]), m[2] != "-", m[3], []])
        elif items[-1][3]:
            items[-1][3][-1][2] += " " + line.strip()
        else:
            items[-1][2] += " " + line.strip()
    return items


def _render_list(items):
    tag = "ol" if items[0][1] else "ul"
    out = [f"<{tag}>"]
    for _, _, text, children in items:
        inner = _render_list(children) if children else ""
        out.append(f"<li>{inline(text)}{inner}</li>")
    out.append(f"</{tag}>")
    return "".join(out)


def _table(lines):
    rows = [[c.strip() for c in line.strip().strip("|").split("|")] for line in lines]
    head, body = rows[0], rows[2:]
    out = ['<div class="table"><table><thead><tr>', *(f"<th>{inline(c)}</th>" for c in head), "</tr></thead><tbody>"]
    for row in body:
        out += ["<tr>", *(f"<td>{inline(c)}</td>" for c in row), "</tr>"]
    return "".join(out + ["</tbody></table></div>"])


def render(md, section_id):
    """One docs page as a <section>; its title comes back too, for the contents."""
    out, title = [], ""
    for block in _blocks(md.splitlines()):
        first = block[0]
        if first.startswith("# "):
            title = first[2:].strip()
            out.append(f'<h2 id="{section_id}">{inline(title)}</h2>')
        elif first.startswith("## "):
            out.append(f"<h3>{inline(first[3:].strip())}</h3>")
        elif first.startswith("|"):
            out.append(_table(block))
        elif LIST_ITEM.match(first):
            out.append(_render_list(_list(block)))
        else:
            out.append(f"<p>{inline(' '.join(line.strip() for line in block))}</p>")
    return title, f"<section>{''.join(out)}</section>"


STYLE = """
  main { max-width: 760px; margin: 0 auto; padding: 32px max(24px, env(safe-area-inset-left)) 96px; }
  .topnav { display: flex; align-items: center; gap: 12px; padding: 20px max(24px, env(safe-area-inset-left));
            border-bottom: 1px solid var(--line); }
  .topnav .sp { flex: 1; }
  .topnav a.link { font-size: .9375rem; color: var(--muted); text-decoration: none; }
  h1 { font-size: 2rem; line-height: 1.15; letter-spacing: -.02em; }
  .lede { color: var(--muted); margin-top: 8px; }
  .toc { margin: 24px 0 0; padding: 16px 20px 16px 40px; background: var(--panel); border: 1px solid var(--line);
         border-radius: var(--radius); }
  .toc li + li { margin-top: 4px; }
  section { border-top: 1px solid var(--line); margin-top: 48px; padding-top: 32px; }
  section h2 { font-size: 1.5rem; line-height: 1.2; scroll-margin-top: 24px; }
  section h3 { font-size: 1.125rem; margin-top: 28px; }
  section p, section ul, section ol, section .table { margin: 12px 0 0; }
  section ul, section ol { padding-left: 1.4em; }
  section li + li { margin-top: 6px; }
  section li > ul, section li > ol { margin-top: 6px; }
  code { font: .875em ui-monospace, "SF Mono", Menlo, monospace; background: var(--soft); padding: 1px 5px;
         border-radius: 5px; }
  .table { overflow-x: auto; }
  table { border-collapse: collapse; font-size: .9375rem; width: 100%; }
  th, td { text-align: left; vertical-align: top; padding: 8px 12px 8px 0; border-bottom: 1px solid var(--line); }
  th { font-weight: 600; }
"""


def write_guide(docs, out):
    """docs: docs/beta. out: site/beta/index.html."""
    pages = sorted(docs.glob("0*.md"))
    rendered = [(slug(p), *render(p.read_text(), slug(p))) for p in pages]
    toc = "".join(f'<li><a href="#{sid}">{inline(title)}</a></li>' for sid, title, _ in rendered)
    body = "\n".join(section for _, _, section in rendered)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex">
<title>Beta testers' guide · Outrigger</title>
<link rel="stylesheet" href="/style.css">
<style>{STYLE}</style>
</head>
<body>
<nav class="topnav"><a class="wordmark" href="/"><i></i>Outrigger</a><span class="beta">Beta</span><span class="sp"></span>
  <a class="link" href="mailto:feedback@outrigger.music">feedback@outrigger.music</a></nav>
<main>
<h1>Beta testers' guide</h1>
<p class="lede">Thanks for testing Outrigger. Most testers need the first three parts; the rest are for when
  something goes wrong.</p>
<ol class="toc">{toc}</ol>
{body}
</main>
</body>
</html>
""")
