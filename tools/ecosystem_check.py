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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_families  # noqa: E402

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


def check_link(link, fetch=get, keep=None):
    """None when the link answers with what the installer expects, else why not.
    keep (a dict), when given, gets the downloaded bytes by URL."""
    try:
        data = fetch(link["url"])
        if keep is not None:
            keep[link["url"]] = data
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


def bases_of(entries, hub=None):
    """{id: [base, ...]} from mutant.cloud's entries and the Hub's catalog, as the loader's blocks read them."""
    return gen_families.lineage_bases({
        "mutant": {i: e.get("base") for i, e in entries.items()},
        "hub": {e["id"]: e.get("base") or [] for e in (hub or {}).get("firmwares") or []}})


def group_of(entry_id, entries, fam, bases=None):
    """keep, clean, blocked or unknown for a mutant.cloud entry id, by the same
    rules as the loader's blocks (tools/gen_families.py)."""
    c = gen_families.category(entry_id, bases if bases is not None else bases_of(entries), fam)
    return "blocked" if c in gen_families.BLOCKS else c


def unblocked_on_board(fam, entries, hub, variants, committed):
    """Projects today's catalogs would block that the committed families_gen.h doesn't:
    the snapshot needs a refresh (tools/gen_families.py --refresh) and a build."""
    live = gen_families.blocked_projects(fam, {
        "mutant": {i: e.get("base") for i, e in entries.items()},
        "hub": {e["id"]: e.get("base") or [] for e in (hub or {}).get("firmwares") or []},
        "variants": variants})
    return sorted(p for p in live if p not in committed)


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


def renames(hub, mutant, mapped):
    """["<entry> (Hub: <id>)"] for mutant.cloud listings whose entry isn't the Hub's id for the
    same repo and that the page doesn't map back: added from mutant.cloud, they'd start a new shelf."""
    by_repo = {}
    for e in hub.get("firmwares") or []:
        if e.get("repo"):
            by_repo.setdefault(e["repo"].lower(), set()).add(e["id"])
    out = set()
    for e in mutant.get("firmwares") or []:
        ids = by_repo.get((e.get("repo") or "").lower())
        entry = e.get("entry") or e.get("id")
        if ids and entry not in ids and mapped.get(entry) not in ids:
            out.add(f"{entry} (Hub: {', '.join(sorted(ids))})")
    return sorted(out)


def build_report(links_bad, feed, entries, fam, state, today, renamed=(), hub=None, stale=()):
    """(markdown body, new state, needs attention). state is what the last run
    saw ({} on the first run: then only the last day's releases count as new)."""
    first = not state
    seen = set(state.get("seen", []))
    since = (dt.date.fromisoformat(today) - dt.timedelta(days=1)).isoformat()
    new = [f for f in feed if (f["date"] >= since if first else f["guid"] not in seen)]
    missing = missing_assets(entries)
    new_missing = sorted(k for k in missing if k not in set(state.get("missing", [])))
    new_renamed = [r for r in renamed if r not in set(state.get("renamed", []))]
    new_stale = [p for p in stale if p not in set(state.get("stale", []))]
    bases = bases_of(entries, hub)

    grouped = {g: [] for g, _ in GROUPS}
    for f in new:
        grouped[group_of(f["id"], entries, fam, bases)].append(f)

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
    if not new and not new_missing and not new_renamed and not new_stale:
        lines.append("None.")
    for g, heading in GROUPS:
        items = grouped[g]
        extra = ([f"- **File gone missing:** {md(k)} (since {md(missing[k])})" for k in new_missing]
                 + [f"- **Renamed on mutant.cloud:** {md(r)}. Map it back in tools/fm1_families.json hub_ids, "
                    "or it starts a new shelf" for r in new_renamed]
                 + [f"- **Not blocked on the board yet:** {md(p)}. Run tools/gen_families.py --refresh and ship a build"
                    for p in new_stale]) if g == "unknown" else []
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
    attention = bool(links_bad or new_missing or new_bases or new_renamed or new_stale)
    # The feed is the window: what it lists today is all a later run needs to remember.
    new_state = {"date": today, "seen": sorted({f["guid"] for f in feed}),
                 "missing": sorted(missing), "renamed": list(renamed), "stale": list(stale)}
    lines.append(f"<!-- ecosystem-state {json.dumps(new_state)} -->")
    return "\n".join(lines), new_state, attention


def attention_comment(links_bad, owner):
    # The bot opened the issue, so only a mention reaches the repo's owner.
    first = "Installer links are broken" if links_bad else "Something new needs a look"
    return f"@{owner} **{first}.** Today's report is in the issue body above."


# ---------- GitHub ----------

def gh(method, path, token, payload=None):
    req = urllib.request.Request("https://api.github.com" + path, method=method,
                                 data=json.dumps(payload).encode() if payload is not None else None,
                                 headers={"Authorization": f"Bearer {token}", "User-Agent": UA,
                                          "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.loads(r.read() or b"null")


def find_issue(repo, token):
    """The report's issue: by label, else (a label GitHub dropped) by title and author."""
    issues = gh("GET", f"/repos/{repo}/issues?labels={LABEL}&state=all&per_page=10", token)
    found = next((i for i in issues if "pull_request" not in i), None)
    if found:
        return found
    issues = gh("GET", f"/repos/{repo}/issues?creator=github-actions%5Bbot%5D&state=all&per_page=50", token)
    return next((i for i in issues if "pull_request" not in i and i.get("title") == TITLE), None)


def ensure_label(repo, token):
    try:
        gh("POST", f"/repos/{repo}/labels", token, {"name": LABEL, "color": "5319e7",
                                                   "description": "The daily ecosystem check's report"})
    except urllib.error.HTTPError as e:
        if e.code != 422:  # 422: it exists
            raise


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="print the report; write nothing")
    args = ap.parse_args(argv)
    today = dt.date.today().isoformat()

    fam = load_families()
    links_bad = []
    got = {}
    for link in installer_links():
        why = check_link(link, keep=got)
        if why:
            links_bad.append((link["name"], link["url"], why))
    try:
        hub, mutant_cat = json.loads(got[HUB_CATALOG]), json.loads(got[MUTANT_CATALOG])
        renamed = renames(hub, mutant_cat, fam.get("hub_ids", {}))
    except (KeyError, ValueError):
        hub = mutant_cat = None
        renamed = None  # a catalog is down: that's reported above

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
    if renamed is None:
        renamed = state.get("renamed", [])
    if entries and hub and mutant_cat:
        variants = {e["id"]: e["entry"] for e in mutant_cat["firmwares"] if e.get("entry") and e["id"] != e["entry"]}
        committed = re.findall(r'^    "([^"]+)",$', gen_families.HEADER.read_text(), re.M)
        stale = unblocked_on_board(fam, entries, hub, variants, set(committed))
    else:
        stale = state.get("stale", [])
    body, _, attention = build_report(links_bad, feed, entries, fam, state, today, renamed, hub, stale)
    if not entries and state:
        # Keep what the last run saw, so tomorrow's run still lists today's releases.
        body = STATE_RE.sub(f"<!-- ecosystem-state {json.dumps(state)} -->", body)
        body = body.replace("## New FM-1 releases", "## New FM-1 releases (mutant.cloud didn't answer today)", 1)

    if args.dry_run:
        print(body)
        print(f"\nneeds attention: {attention}", file=sys.stderr)
    else:
        if issue is None:
            ensure_label(repo, token)
            issue = gh("POST", f"/repos/{repo}/issues", token, {"title": TITLE, "body": body, "labels": [LABEL]})
        else:
            gh("PATCH", f"/repos/{repo}/issues/{issue['number']}", token, {"body": body, "state": "open"})
        if attention:
            gh("POST", f"/repos/{repo}/issues/{issue['number']}/comments", token,
               {"body": attention_comment(links_bad, repo.split("/")[0])})
        print(f"updated issue #{issue['number']}; needs attention: {attention}")
    for name, url, why in links_bad:
        print(f"::error::{name} {why}: {url}")
    return 1 if links_bad else 0


if __name__ == "__main__":
    sys.exit(main())
