"""The daily ecosystem check (.github/workflows/ecosystem-check.yml).

Test-fetches every file and list the live installer depends on, then lists the
FM-1 releases mutant.cloud has seen since the last run, grouped by how
Outrigger treats their family (tools/fm1_families.json). The report is the body
of one GitHub issue, labelled `ecosystem-check`; what the last run saw rides in
that body as a hidden comment, so the check needs no storage of its own.

    python3 -I tools/ecosystem_check.py --dry-run     # report to stdout, no issue writes
    python3 -I tools/ecosystem_check.py               # in CI: GITHUB_TOKEN, GITHUB_REPOSITORY

Exit 1 when an installer link is broken, so the run goes red and GitHub emails.
Standard library only: the workflow runs it with the runner's python3.
"""
import argparse
import ast
import datetime as dt
import email.utils
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UA = "outrigger-ecosystem-check (+https://github.com/chadthornton/outrigger)"
MUTANT = "https://mutant.cloud/"
FIRMWARE_URL = MUTANT + "firmware.json?device=fm1&kind=firmware"
FEED_URL = MUTANT + "feed.xml?device=fm1"
HUB_CATALOG = "https://thefirmwarehub.com/catalog.json"
MUTANT_CATALOG = MUTANT + "catalog.json?device=fm1"
LABEL = "ecosystem-check"
TITLE = "Ecosystem check"
STATE_RE = re.compile(r"<!-- ecosystem-state (.*?) -->", re.S)
TIMEOUT = 30

GROUPS = [
    ("unknown", "New or unknown family, or a file gone missing"),
    ("blocked", "Blocked: Outrigger won't add these"),
    ("clean", "Clean-slate families: switch with a clean slate"),
    ("keep", "Families we know (stock, Felucca): data kept per firmware"),
]


# ---------- What the installer depends on ----------

def _constants(path, names):
    """Top-level literal assignments from a Python source file, without importing it."""
    out = {}
    for node in ast.parse(Path(path).read_text()).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and getattr(node.targets[0], "id", None) in names:
            out[node.targets[0].id] = ast.literal_eval(node.value)
    return out


def installer_links(root=ROOT):
    """[{name, url, sha256 or None, json}]: the files the live installer downloads, from the
    pins the site build uses (tools/store_template.py), and the catalogs Add firmware reads."""
    t = _constants(root / "tools/store_template.py", {"LOADER_URL", "HUB", "STARTER"})
    blob = _constants(root / "tools/store_image.py", {"BLOB_SHA256"})["BLOB_SHA256"]
    links = [{"name": "JieLi's wl82loader.bin (GitHub)", "url": t["LOADER_URL"], "sha256": blob, "json": False}]
    for _id, name, version, _project, _fixture, path, sha in t["STARTER"]:
        links.append({"name": f"{name} {version} (Firmware Hub)", "url": t["HUB"] + path, "sha256": sha, "json": False})
    links.append({"name": "Firmware Hub catalog", "url": HUB_CATALOG, "sha256": None, "json": True})
    links.append({"name": "mutant.cloud catalog", "url": MUTANT_CATALOG, "sha256": None, "json": True})
    return links


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return r.read()


def check_link(link, fetch=get):
    """None when the link answers with what the installer expects, else why not."""
    try:
        data = fetch(link["url"])
    except urllib.error.HTTPError as e:
        return f"answered {e.code}"
    except Exception as e:  # noqa: BLE001 - any failure to fetch is the finding
        return f"couldn't be reached ({e})"
    if link["sha256"] and hashlib.sha256(data).hexdigest() != link["sha256"]:
        return "answered, but the file changed (SHA-256 differs from the installer's pin)"
    if link["json"]:
        try:
            if not json.loads(data).get("firmwares"):
                return "answered, but lists no firmwares"
        except ValueError:
            return "answered, but not with JSON"
    return None


# ---------- Families ----------

def load_families(root=ROOT):
    return json.loads((root / "tools/fm1_families.json").read_text())


def group_of(entry_id, entries, fam):
    """keep, clean, blocked or unknown for a mutant.cloud entry id, following
    "built on" through firmwares that are bases themselves (X0X, Hortator)."""
    if entry_id in fam["blocked_ids"]:
        return "blocked"
    if entry_id == "stock" or entry_id in fam["keep"]:
        return "keep"
    seen = {entry_id}
    base = (entries.get(entry_id) or {}).get("base")
    while base:
        if base in fam["blocked_ids"] or base in fam["blocked_bases"]:
            return "blocked"
        if base in fam["keep"] or base == "stock":
            return "keep"
        if base in fam["clean"]:
            return "clean"
        if base in seen or base not in entries:
            return "unknown"
        seen.add(base)
        base = entries[base].get("base")
    return "unknown"


# ---------- What's new ----------

def md(text):
    """Third-party text, safe in the issue body: no @mentions (they'd notify
    strangers), no links or markup of its own, no hidden comments."""
    text = re.sub(r"[\[\]<>`*_|]", "", str(text)).replace("@", "@\u200b")
    return text.strip()[:120]


def parse_feed(xml_bytes):
    """[{guid, id, title, link, date}] from mutant.cloud's RSS, newest first. The
    stdlib parser resolves no external entities, and the runner's expat (2.4+)
    caps entity expansion."""
    out = []
    for item in ET.fromstring(xml_bytes).iter("item"):
        guid = (item.findtext("guid") or "").strip()
        m = re.match(r"mutant\.cloud/([a-z0-9-]+)@", guid)
        if not m:
            continue
        when = item.findtext("pubDate")
        out.append({"guid": guid, "id": m.group(1), "title": (item.findtext("title") or guid).strip(),
                    "link": (item.findtext("link") or "").strip(),
                    "date": email.utils.parsedate_to_datetime(when).date().isoformat() if when else ""})
    return out


def missing_assets(entries):
    """{"<id> <file>": date} for FM-1 packages mutant.cloud saw vanish from their maker's link."""
    out = {}
    for e in entries.values():
        for a in (e.get("release") or {}).get("assets") or []:
            if a.get("missing") and str(a.get("product", "")).startswith("FM-1_"):
                out[f"{e['id']} {a['name']}"] = a["missing"]
    return out


def build_report(links_bad, feed, entries, fam, state, today):
    """(markdown body, new state, needs attention). state is what the last run
    saw ({} on the first run: then only the last day's releases count as new)."""
    first = not state
    seen = set(state.get("seen", []))
    since = (dt.date.fromisoformat(today) - dt.timedelta(days=1)).isoformat()
    new = [f for f in feed if (f["date"] >= since if first else f["guid"] not in seen)]
    missing = missing_assets(entries)
    new_missing = sorted(k for k in missing if k not in set(state.get("missing", [])))

    grouped = {g: [] for g, _ in GROUPS}
    for f in new:
        grouped[group_of(f["id"], entries, fam)].append(f)

    lines = [f"Daily check, {today}. Updated by `.github/workflows/ecosystem-check.yml`; "
             "a comment appears only when something needs a look.", ""]
    lines.append("## Installer links")
    if links_bad:
        lines += [f"- **BROKEN: {name}** {why}. <{url}>" for name, url, why in links_bad]
        lines.append("")
        lines.append("Fresh installs fail until this is fixed.")
    else:
        lines.append("All answer with the files the installer expects.")
    lines.append("")

    lines.append(f"## New FM-1 releases since {'yesterday' if first else state.get('date', 'the last run')}")
    if not new and not new_missing:
        lines.append("None.")
    for g, heading in GROUPS:
        items = grouped[g]
        extra = [f"- **File gone missing:** {md(k)} (since {md(missing[k])})" for k in new_missing] if g == "unknown" else []
        if not items and not extra:
            continue
        lines += ["", f"### {heading}"] + extra
        for fid in dict.fromkeys(f["id"] for f in items):
            mine = [f for f in items if f["id"] == fid]  # newest first, as the feed lists them
            more = f" (and {len(mine) - 1} earlier)" if len(mine) > 1 else ""
            base = (entries.get(fid) or {}).get("base") or "none listed"
            lines.append(f"- [{md(mine[0]['title'])}](https://mutant.cloud/{fid}){more}, {mine[0]['date']}, "
                         f"built on: {md(base)}")
    lines.append("")

    # A comment (a notification) only for what Chad should act on: a broken link,
    # a file gone missing, or a family the table has never heard of. Releases
    # that list no base land in the first group without a comment.
    new_bases = sorted({entries[f["id"]]["base"] for f in grouped["unknown"]
                        if (entries.get(f["id"]) or {}).get("base")})
    attention = bool(links_bad or new_missing or new_bases)
    new_state = {"date": today, "seen": sorted({f["guid"] for f in feed} | seen)[-500:],
                 "missing": sorted(missing)}
    lines.append(f"<!-- ecosystem-state {json.dumps(new_state)} -->")
    return "\n".join(lines), new_state, attention


def attention_comment(links_bad):
    first = "Installer links are broken" if links_bad else "New releases need a look"
    return f"**{first}.** Today's report is in the issue body above."


# ---------- GitHub ----------

def gh(method, path, token, payload=None):
    req = urllib.request.Request("https://api.github.com" + path, method=method,
                                 data=json.dumps(payload).encode() if payload is not None else None,
                                 headers={"Authorization": f"Bearer {token}", "User-Agent": UA,
                                          "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.loads(r.read() or b"null")


def find_issue(repo, token):
    issues = gh("GET", f"/repos/{repo}/issues?labels={LABEL}&state=all&per_page=10", token)
    return next((i for i in issues if "pull_request" not in i), None)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="print the report; write nothing")
    args = ap.parse_args(argv)
    today = dt.date.today().isoformat()

    links_bad = []
    for link in installer_links():
        why = check_link(link)
        if why:
            links_bad.append((link["name"], link["url"], why))

    fam = load_families()
    try:
        feed = parse_feed(get(FEED_URL))
        entries = {e["id"]: e for e in json.loads(get(FIRMWARE_URL))["entries"]}
    except Exception as e:  # noqa: BLE001 - the release list is a nicety; links still report
        print(f"mutant.cloud's feed or firmware.json failed: {e}", file=sys.stderr)
        feed, entries = [], {}

    repo, token = os.environ.get("GITHUB_REPOSITORY"), os.environ.get("GITHUB_TOKEN")
    issue = None if args.dry_run else find_issue(repo, token)
    state = {}
    if issue:
        m = STATE_RE.search(issue.get("body") or "")
        state = json.loads(m.group(1)) if m else {}
    body, _, attention = build_report(links_bad, feed, entries, fam, state, today)
    if not entries and state:
        # Keep what the last run saw, so tomorrow's run still lists today's releases.
        body = STATE_RE.sub(f"<!-- ecosystem-state {json.dumps(state)} -->", body)
        body = body.replace("## New FM-1 releases", "## New FM-1 releases (mutant.cloud didn't answer today)", 1)

    if args.dry_run:
        print(body)
        print(f"\nneeds attention: {attention}", file=sys.stderr)
    else:
        if issue is None:
            issue = gh("POST", f"/repos/{repo}/issues", token, {"title": TITLE, "body": body, "labels": [LABEL]})
        else:
            gh("PATCH", f"/repos/{repo}/issues/{issue['number']}", token, {"body": body, "state": "open"})
        if attention:
            gh("POST", f"/repos/{repo}/issues/{issue['number']}/comments", token,
               {"body": attention_comment(links_bad)})
        print(f"updated issue #{issue['number']}; needs attention: {attention}")
    for name, url, why in links_bad:
        print(f"::error::{name} {why}: {url}")
    return 1 if links_bad else 0


if __name__ == "__main__":
    sys.exit(main())
