#!/usr/bin/env python3
"""Read the installer's anonymous install counts (functions/api/install.js) from D1.

    tools/installs.py            beta, then production
    tools/installs.py beta       only beta's and the Pages previews' (outrigger-reports-beta)
    tools/installs.py production only outrigger.music's (outrigger-reports)

Prints finished and failed installs, by board, edition and week (weeks start on
Monday), and where failed ones stopped. A finished install counts once per board
per browser; a failure counts every attempt, so failures read high next to
finishes. Credentials and databases as tools/reports.py.
"""
import json
import sys
from collections import Counter
from datetime import date, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from reports import ROOT, database, load_credentials, query  # noqa: E402

NAMES = {b["site_id"]: b["short_name"] for b in json.loads((ROOT / "firmware/boards.json").read_text())["boards"].values()}


def week(day):
    d = date.fromisoformat(day)
    return (d - timedelta(days=d.weekday())).isoformat()


def summarise(rows):
    """Counts from the installs table's rows: [finished, failed] per board, edition and week."""
    groups = {"boards": {}, "editions": {}, "weeks": {}}
    for r in rows:
        i = 0 if r["result"] == "ok" else 1
        for group, key in (("boards", r["board"]), ("editions", r["edition"]), ("weeks", week(r["day"]))):
            groups[group].setdefault(key, [0, 0])[i] += 1
    failures = Counter(r["failure"] for r in rows if r["result"] != "ok")
    return {"ok": sum(r["result"] == "ok" for r in rows), "failed": sum(r["result"] != "ok" for r in rows),
            "failures": dict(failures.most_common()), **groups}


def describe(name, s):
    if not s["ok"] and not s["failed"]:
        return f"{name}: no installs counted yet"
    pair = lambda c: f"{c[0]} finished, {c[1]} failed"  # noqa: E731
    lines = [f"{name}: {s['ok']} finished, {s['failed']} failed "
             "(a finish counts once per board per browser, a failure every attempt)"]
    if s["failures"]:
        lines.append("  failed at: " + ", ".join(f"{k} {n}" for k, n in s["failures"].items()))
    lines.append("  by board:")
    lines += [f"    {NAMES.get(b, b)}: {pair(c)}" for b, c in sorted(s["boards"].items(), key=lambda kv: -sum(kv[1]))]
    lines.append("  by edition:")
    lines += [f"    {e}: {pair(c)}" for e, c in sorted(s["editions"].items(), key=lambda kv: -sum(kv[1]))]
    lines.append("  by week:")
    lines += [f"    week of {w}: {pair(c)}" for w, c in sorted(s["weeks"].items())]
    return "\n".join(lines)


def main():
    args = sys.argv[1:]
    if args and args[0] not in ("beta", "production") or len(args) > 1:
        sys.exit(f"installs: {__doc__.strip().splitlines()[2].strip()}")
    env = load_credentials()
    for channel in args or ["beta", "production"]:
        name, db_id = database(channel)
        if not query(env, db_id, "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'installs'")["results"]:
            print(f"{name}: no installs table yet (tools/reports.py migrate{'' if channel == 'production' else ' beta'})")
            continue
        rows = query(env, db_id, "SELECT day, board, edition, installer, result, failure FROM installs")["results"]
        print(describe(name, summarise(rows)))


if __name__ == "__main__":
    main()
