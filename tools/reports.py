#!/usr/bin/env python3
"""Read the installer's "Report this board" reports (functions/api/report.js) from D1.

    tools/reports.py new [preview]       reports since the last `new`, then remember where it stopped (each morning)
    tools/reports.py list [preview] [N]  the last N reports (default 20), remembering nothing
    tools/reports.py forget <id> [preview]   delete one report's email (someone asked us to)
    tools/reports.py delete <id> [preview]   delete one report
    tools/reports.py migrate [preview]   apply migrations/ to the database (wrangler, once per new migration)

Without `beta` it reads outrigger.music's database (outrigger-reports); with it
(or `preview`, the channel's name), preview.outrigger.music's and the other Pages
previews' (outrigger-reports-beta, named for the channel's old name). Database ids come from
wrangler.toml. Credentials: ~/.config/cloudflare/outrigger-pages.env, as
tools/deploy_site.py (the token needs Account > D1 > Edit). Never printed.
Where `new` stopped: ~/.config/outrigger/reports-seen.json.
"""
import json
import subprocess
import sys
import tomllib
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from deploy_site import ROOT, WRANGLER, load_credentials  # noqa: E402

SEEN = Path.home() / ".config/outrigger/reports-seen.json"
API = "https://api.cloudflare.com/client/v4/accounts/{account}/d1/database/{db}/query"


def fail(msg):
    sys.exit(f"reports: {msg}")


def database(channel, toml_path=ROOT / "wrangler.toml"):
    """(name, id) of the channel's D1 database: top level for production, [env.preview] for beta."""
    config = tomllib.loads(toml_path.read_text())
    table = config if channel == "production" else config.get("env", {}).get("preview", {})
    for db in table.get("d1_databases", []):
        if db.get("binding") == "DB":
            return db["database_name"], db["database_id"]
    fail(f"wrangler.toml has no DB binding for {channel}")


def query(env, db_id, sql, params=()):
    req = urllib.request.Request(API.format(account=env["CLOUDFLARE_ACCOUNT_ID"], db=db_id), method="POST",
                                 data=json.dumps({"sql": sql, "params": list(params)}).encode(),
                                 headers={"Authorization": f"Bearer {env['CLOUDFLARE_API_TOKEN']}",
                                          "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as res:
            body = json.load(res)
    except urllib.error.HTTPError as e:
        fail(f"D1 said {e.code}: {e.read().decode(errors='replace')[:300]}")
    if not body.get("success"):
        fail(f"D1 query failed: {body.get('errors')}")
    return body["result"][0]


def describe(r):
    """One report as plain text, for a session to read."""
    flash = "?" if r.get("flash_mb") is None else f"{r['flash_mb']:g} MB"
    psram = "?" if r.get("psram_mb") is None else f"{r['psram_mb']} MB"
    vid = "?" if r.get("usb_vid") is None else f"0x{r['usb_vid']:04x}"
    lines = [f"#{r['id']}  {r['created_at']}  {r['board']}",
             f"  chip: {r.get('chip') or '?'}, flash {flash}, PSRAM {psram}, USB vendor {vid}",
             f"  picked: {r.get('picked') or '?'}; installer {r.get('installer') or '?'}",
             f"  email: {r.get('email') or '(none)'}"]
    if r.get("note"):
        lines.append("  note: " + r["note"].replace("\n", "\n        "))
    if r.get("verdict"):
        lines.append(f"  {'the page said' if r.get('log') else 'chip check said'}: {r['verdict']}")
    if r.get("log"):
        lines.append("  log:\n    " + r["log"].replace("\n", "\n    "))
    return "\n".join(lines)


def seen(channel, path=SEEN):
    try:
        return json.loads(path.read_text()).get(channel, 0)
    except (FileNotFoundError, json.JSONDecodeError):
        return 0


def remember(channel, last_id, path=SEEN):
    try:
        data = json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        data = {}
    data[channel] = last_id
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")


# The preview channel's database is called beta (outrigger-reports-beta); either word picks it.
PREVIEW_WORDS = ("preview", "beta")


def channel_args(args):
    """(the database's channel, the arguments without the channel word)."""
    channel = "beta" if any(a in PREVIEW_WORDS for a in args) else "production"
    return channel, [a for a in args if a not in PREVIEW_WORDS]


def main():
    channel, args = channel_args(sys.argv[1:])
    if not args or args[0] not in ("new", "list", "forget", "delete", "migrate"):
        fail(__doc__.strip().splitlines()[2].strip())
    cmd, rest = args[0], args[1:]
    name, db_id = database(channel)
    env = load_credentials()
    if cmd == "migrate":
        sys.exit(subprocess.run(["npx", "--yes", WRANGLER, "d1", "migrations", "apply", name, "--remote",
                                 *([] if channel == "production" else ["--env", "preview"])], cwd=ROOT, env=env).returncode)
    if cmd in ("forget", "delete"):
        if len(rest) != 1 or not rest[0].isdigit():
            fail(f"{cmd} takes one report number")
        sql = "UPDATE reports SET email = NULL WHERE id = ?" if cmd == "forget" else "DELETE FROM reports WHERE id = ?"
        changed = query(env, db_id, sql, [int(rest[0])])["meta"]["changes"]
        print(f"{name}: report #{rest[0]} " + ("not found" if not changed else "email deleted" if cmd == "forget" else "deleted"))
        return
    if cmd == "list":
        n = int(rest[0]) if rest and rest[0].isdigit() else 20
        rows = query(env, db_id, "SELECT * FROM reports ORDER BY id DESC LIMIT ?", [n])["results"][::-1]
    else:
        rows = query(env, db_id, "SELECT * FROM reports WHERE id > ? ORDER BY id", [seen(channel)])["results"]
    print(f"{name}: {len(rows)} {'new ' if cmd == 'new' else ''}report{'s' if len(rows) != 1 else ''}")
    for r in rows:
        print(describe(r))
    if cmd == "new" and rows:
        remember(channel, rows[-1]["id"])


if __name__ == "__main__":
    main()
