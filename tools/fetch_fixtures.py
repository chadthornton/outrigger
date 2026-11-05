"""Fetch the fm1_loader test fixtures into tests/fixtures/ (gitignored).

Each file is pinned by its own SHA-256 here, not by today's catalog: the golden
vectors are built from these exact bytes. The community firmwares come from
Outrigger's own test-fixtures release, which keeps a copy of each with its
GPL source. Neither upstream keeps them: the Firmware Hub keeps only each
firmware's latest file, and authors delete releases (Felucca 1.8.0's went on
2026-11-04). The comment above each entry names where its bytes came from.
That release is for tests only; files there are added, never replaced.
tools/check_latest.py tries each firmware's newest release separately.

Usage: python3 tools/fetch_fixtures.py [fixtures_dir]
"""
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

HUB = "https://thefirmwarehub.com/"
GITHUB = "https://github.com/"
MIRROR = GITHUB + "chadthornton/outrigger/releases/download/test-fixtures/"
JLUB_RAW = ("https://raw.githubusercontent.com/kagaimiq/jl-uboot-tool/"
            "adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a/")

# (file name, catalog id or "stock" or None, URL, sha256)
FIXTURES = [
    # M-VAVE's own repo at a pinned commit, as the installer's starter set
    # (tools/store_template.py): the Hub keeps only the newest stock file.
    ("FM-1_015.fwsc", "stock", "https://raw.githubusercontent.com/m-vave/fm1_open/"
     "b178ac43c25f010c60a878056fb5331fe693fd62/sdk/firmware/FM-1_015.fwsc",
     "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a"),
    # From illuminus5/skiff's v1.4 release.
    ("skiff-1.4.fwsc", "skiff", MIRROR + "skiff-1.4.fwsc",
     "0622a752e8304777c042ccea0732ba023e1413baa6dbc8724576e2ec76de02e0"),
    # From isod89/sloop-fm1's v2.8.1 release.
    ("sloop-2.8.1.fwsc", "sloop", MIRROR + "sloop-2.8.1.fwsc",
     "64c9dfa7e75312debeae01a7c96e4ad3801eaffff9c81cb76ece9c48daf0501d"),
    # Felucca 1.6.1, the base Skiff builds on: checkpoint 3's same-layout target.
    # From hugelton/Felucca's v1.6.1 release.
    ("felucca-1.6.1.fwsc", "felucca", MIRROR + "felucca-1.6.1.fwsc",
     "dff3a38f7e5b9833e15aba322558c9485e9e422e3ff813c616c91b3584a2d909"),
    # Felucca 1.8.0 and Skiff 2.1.1 were the installer's starter set until
    # 2026-11-04; the receive and Add firmware tests still use them.
    # From hugelton/Felucca's v1.8.0 release (deleted 2026-11-04).
    ("felucca-1.8.0.fwsc", "felucca", MIRROR + "felucca-1.8.0.fwsc",
     "a31760f2588b39bcb3a8d6f78f6041a1a550978a972ecd71564e45513698ecd6"),
    # From illuminus5/skiff's v2.1.1 release.
    ("skiff-2.1.1.fwsc", "skiff", MIRROR + "skiff-2.1.1.fwsc",
     "0cfae2431f351365d83842bf1a381ee48baead371e81f77faaa2b3c6aabbd304"),
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
        if entry is None or entry["sha256"] != digest:
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
    try:  # informational only: the pinned files don't come from the catalog
        for line in check_catalog(json.loads(fetch(HUB + "catalog.json"))):
            print("fixtures: NOTE %s (the pinned version is still fetched)" % line)
    except (OSError, ValueError, KeyError) as e:
        print("fixtures: NOTE catalog unreadable (%s)" % e)
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
