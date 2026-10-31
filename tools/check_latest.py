#!/usr/bin/env python3
"""Tries each firmware's newest release against the loader, outside make test.

The tests pin exact versions (tools/fetch_fixtures.py), so a new release can't
break them. This reads the Firmware Hub's catalog, fetches the newest package of
each firmware the tests pin (or every catalog firmware, --all) into out_dir,
checks it against the catalog's SHA-256, and runs build/check_package on it: it
must parse and install over the stock firmware on a mock FM-1. Exits 1 if any
package fails, so the weekly workflow (.github/workflows/latest.yml) goes red.

usage: check_latest.py out_dir [--all]
"""
import hashlib
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

# Makes the calls do nothing where claude-progress is not installed.
_bytecode, sys.dont_write_bytecode = sys.dont_write_bytecode, True  # no __pycache__ in the plugin
try:
    sys.path.insert(0, os.environ["CLAUDE_PROGRESS_DIR"])
    from claude_progress import claude_progress
except Exception:
    def claude_progress(*args):
        pass
finally:
    sys.dont_write_bytecode = _bytecode

ROOT = Path(__file__).resolve().parent.parent
HUB = "https://thefirmwarehub.com/"
PINNED = {"skiff": "1.4", "felucca": "1.6.1", "sloop": "2.8.1"}  # fetch_fixtures.py


def fetch(url):
    # The Firmware Hub answers 403 to urllib's default User-Agent.
    req = urllib.request.Request(url, headers={"User-Agent": "usb-midi-host-fixtures/1"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def download(entry, out):
    """The package from the hub, or from its GitHub release; None if neither matches the catalog's hash."""
    name = entry["pkg"].rsplit("/", 1)[-1]
    urls = [HUB + entry["pkg"]]
    if entry.get("repo") and entry.get("tag"):
        urls.append("https://github.com/%s/releases/download/%s/%s" % (entry["repo"], entry["tag"], name))
    for url in urls:
        try:
            data = fetch(url)
        except (urllib.error.URLError, OSError):
            continue
        if hashlib.sha256(data).hexdigest() == entry["sha256"]:
            path = out / name
            path.write_bytes(data)
            return path
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    every = "--all" in sys.argv[2:]
    out.mkdir(parents=True, exist_ok=True)
    catalog = json.loads(fetch(HUB + "catalog.json"))
    entries = [f for f in catalog["firmwares"] if f.get("pkg") and (every or f["id"] in PINNED)]
    paths, missing = [], []
    for i, entry in enumerate(entries, 1):
        claude_progress("-n", "latest", "%d/%d" % (i, len(entries)), entry["id"])
        path = download(entry, out)
        if path is None:
            missing.append(entry["id"])
            continue
        paths.append(path)
        pinned = PINNED.get(entry["id"])
        note = "" if pinned is None else (" (tests pin %s)" % pinned if pinned != entry.get("version") else " (pinned)")
        print("latest: %s %s%s" % (entry["id"], entry.get("version", "?"), note))
    for m in missing:
        print("latest: %s: no download matched the catalog's SHA-256" % m)
    fixtures = ROOT / "tests" / "fixtures"
    result = subprocess.run([str(ROOT / "build" / "check_package"), str(fixtures / "wl82loader.bin"),
                             str(fixtures / "FM-1_015.fwsc")] + [str(p) for p in paths])
    ok = result.returncode == 0 and not missing
    claude_progress("-n", "latest", "done" if ok else "fail", "%d packages" % len(paths))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
