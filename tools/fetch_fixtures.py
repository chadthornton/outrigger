"""Fetch the fm1_loader test fixtures into tests/fixtures/ (gitignored).

Each file is pinned by its own SHA-256 here, not by today's catalog: the catalog
lists only the latest version of each firmware, so when one moves on, this
script says so and stops instead of quietly fetching a different file (the
golden vectors are built from these exact bytes).

Usage: python3 tools/fetch_fixtures.py [fixtures_dir]
"""
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

HUB = "https://thefirmwarehub.com/"
JLUB_RAW = ("https://raw.githubusercontent.com/kagaimiq/jl-uboot-tool/"
            "adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a/")

# (file name, catalog id or "stock" or None, URL, sha256)
FIXTURES = [
    ("FM-1_015.fwsc", "stock", HUB + "fw/mvave/FM-1_015.fwsc",
     "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a"),
    ("skiff-1.4.fwsc", "skiff", HUB + "fw/skiff-1.4.fwsc",
     "0622a752e8304777c042ccea0732ba023e1413baa6dbc8724576e2ec76de02e0"),
    ("sloop-2.8.1.fwsc", "sloop", HUB + "fw/sloop-2.8.1.fwsc",
     "64c9dfa7e75312debeae01a7c96e4ad3801eaffff9c81cb76ece9c48daf0501d"),
    # Felucca 1.6.1, the base Skiff builds on: checkpoint 3's same-layout target.
    ("felucca-1.6.1.fwsc", "felucca", HUB + "fw/felucca-1.6.1.fwsc",
     "dff3a38f7e5b9833e15aba322558c9485e9e422e3ff813c616c91b3584a2d909"),
    # JieLi's loader, from jl-uboot-tool at the commit the Unbricker pins.
    ("wl82loader.bin", None, JLUB_RAW + "data/loaderblobs/usb/wl82loader.bin",
     "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376"),
]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url):
    # The Firmware Hub answers 403 to urllib's default User-Agent.
    req = urllib.request.Request(url, headers={"User-Agent": "usb-midi-host-fixtures/1"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def check_catalog(catalog):
    """Report pinned packages the catalog no longer lists with the same hash."""
    entries = {f["id"]: f for f in catalog["firmwares"]}
    entries["stock"] = catalog["stock"]
    moved = []
    for name, cid, url, digest in FIXTURES:
        if cid is None:
            continue
        entry = entries.get(cid)
        if entry is None or entry["sha256"] != digest or not url.endswith(entry["pkg"]):
            moved.append("%s: catalog now lists %s" % (name, entry and entry["pkg"]))
    return moved


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures")
    out.mkdir(parents=True, exist_ok=True)
    missing = [f for f in FIXTURES
               if not (out / f[0]).is_file() or sha256((out / f[0]).read_bytes()) != f[3]]
    if not missing:
        print("fixtures: all %d present and verified" % len(FIXTURES))
        return 0
    moved = check_catalog(json.loads(fetch(HUB + "catalog.json")))
    for line in moved:
        print("fixtures: NOTE %s (pinned file is still fetched by its own URL)" % line)
    for name, _, url, digest in missing:
        data = fetch(url)
        if sha256(data) != digest:
            print("fixtures: %s from %s has sha256 %s, expected %s; not saved"
                  % (name, url, sha256(data), digest), file=sys.stderr)
            return 1
        (out / name).write_bytes(data)
        print("fixtures: %s ok (%d bytes)" % (name, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
