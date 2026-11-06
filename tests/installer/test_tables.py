"""tools/build_site.py against the shared cases in tables.json, the same ones
the page's planner runs (plan.test.mjs), so the two checkers can't drift.
Run by `make test`: python3 -I tests/installer/test_tables.py
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from build_site import (BOARDS, check_build_tag, check_layout, check_table, mac_problem, make_manifest,  # noqa: E402
                        read_build_tag, registry_macs)
from check_partitions import read_csv  # noqa: E402

CASES = json.loads((ROOT / "tests/installer/tables.json").read_text())
TAGS = json.loads((ROOT / "tests/installer/tags.json").read_text())
OURS = read_csv(ROOT / "firmware/waveshare_169/partitions.csv")
BOARD = BOARDS["waveshare-esp32-s3-touch-lcd-1.69"]
BOARD_35 = BOARDS["waveshare-esp32-s3-touch-lcd-3.5"]

# build_site never sees the board, and stages from a fixed list of four files,
# so these cases belong to the page alone.
PAGE_ONLY = ("backup store", "0x9000", "missing")


def resolve(spec):
    if spec is None:
        return None
    if spec == "ours":
        return dict(OURS)
    spec = dict(spec)
    replace = spec.pop("_replace", False)
    return {**({} if replace else OURS), **{k: tuple(v) for k, v in spec.items()}}


def encode(table, length):
    out = bytearray(b"\xff" * length)
    for i, (name, (off, size)) in enumerate(table.items()):
        kind = 0 if name.startswith("app") else 1
        out[i * 32:i * 32 + 32] = struct.pack("<2sBBII16sI", b"\xaa\x50", kind, 0, off, size, name.encode(), 0)
    n = len(table) * 32
    out[n:n + 2] = b"\xeb\xeb"
    return bytes(out)


def parts_for(case):
    lengths = {**CASES["default_parts"], **case.get("parts", {})}
    parts = {}
    for off, length in lengths.items():
        if length is None:
            continue
        off = int(off, 16)
        data = bytearray(b"\x5a" * length)
        if off == 0x8000:
            data = bytearray(encode(resolve(case["new_table"]), length))
        if off == 0 and length >= 4:
            data[0], data[3] = 0xE9, 0x40  # image header: 16 MB flash
        parts[off] = bytes(data)
    return parts


def app_with(tag_hex):
    tag = bytes.fromhex(tag_hex)
    head = bytearray(b"\xff" * (TAGS["offset"] + 20 + 64))
    head[TAGS["offset"]:TAGS["offset"] + len(tag)] = tag
    return bytes(head[:TAGS["offset"] + len(tag)]) if len(tag) < 20 else bytes(head)


def refuses(fn, *args):
    try:
        fn(*args)
    except SystemExit:
        return True
    return False


def check_tags():
    failures = []
    for case in TAGS["cases"]:
        got = read_build_tag(app_with(case["hex"]))
        if got != case["expect"]:
            failures.append(f"read_build_tag {case['name']}: {got} != {case['expect']}")
    loader = app_with(TAGS["cases"][0]["hex"])  # hub+L
    hub = app_with(TAGS["cases"][1]["hex"])  # hub
    loader_35 = app_with(next(c["hex"] for c in TAGS["cases"] if c["expect"] and c["expect"]["board"] == 1))
    untagged = app_with("ff" * 20)
    if refuses(check_build_tag, loader, "hub-loader", BOARD) or refuses(check_build_tag, hub, "hub", BOARD):
        failures.append("check_build_tag refused a matching tag")
    if refuses(check_build_tag, loader_35, "hub-loader", BOARD_35):
        failures.append("check_build_tag refused a 3.5\" build staged for the 3.5\"")
    if not refuses(check_build_tag, untagged, "hub", BOARD):
        failures.append("check_build_tag accepted an app with no tag")
    if not refuses(check_build_tag, hub, "hub-loader", BOARD):
        failures.append("check_build_tag accepted a MIDI Hub app staged as hub-loader")
    if not refuses(check_build_tag, loader_35, "hub-loader", BOARD) or not refuses(check_build_tag, loader, "hub-loader", BOARD_35):
        failures.append("check_build_tag accepted a build staged for the other board")
    return failures


def check_macs():
    """A build that lists the registry's boards (a DIAG_WEB build) is never staged for the site."""
    failures = []
    macs = registry_macs(ROOT / "tools/devices.example.json")
    if [label for label, _ in macs] != ["Small-V1", "Small-V2", "Big-S3"]:
        failures.append(f"registry_macs reads the ESP32s only: {macs}")
    app = bytearray(b"\xff" * 4096)
    if mac_problem(bytes(app), macs) is not None:
        failures.append("an app without a registry MAC is refused")
    app[1000:1006] = bytes.fromhex("020000000003")
    if "Big-S3" not in (mac_problem(bytes(app), macs) or ""):
        failures.append("an app holding Big-S3's MAC isn't refused")
    if registry_macs(ROOT / "no-such-registry.json") != []:
        failures.append("no registry should mean nothing to look for")
    return failures


def check_manifest():
    """The page checks each download by SHA-256 and each write by the board's MD5."""
    parts = parts_for(CASES["cases"][0])
    tag = {"commit": "def5678", "dirty": False, "app_set": "hub"}
    m = make_manifest(BOARD, "hub", "def5678", tag, parts)
    got = [(p["offset"], p["md5"], p["sha256"]) for p in m["builds"][0]["parts"]]
    want = [(off, hashlib.md5(parts[off]).hexdigest(), hashlib.sha256(parts[off]).hexdigest())
            for off in (0x0, 0x8000, 0xE000, 0x10000)]
    failures = [] if got == want else [f"manifest parts {got} != {want}"]
    if m.get("build") != tag:
        failures.append(f"manifest build {m.get('build')} != {tag}")
    if "store_template" in m:
        failures.append("a MIDI Hub manifest names a store template; only loader builds set up a store")
    # No history: the page says neither "updates" nor "newer" (board.js compareBuilds), so none is better than [].
    if "ancestors" in m:
        failures.append("a manifest made without history lists ancestors")
    if make_manifest(BOARD, "hub", "def5678", tag, parts, history=["def5678", "abc1234"]).get("ancestors") != ["def5678", "abc1234"]:
        failures.append("the manifest drops the build's ancestors")
    template = b'{"size": 1}\n'
    loader = make_manifest(BOARD, "hub-loader", "def5678", dict(tag, app_set="hub+L"), parts, store_template=template)
    if loader.get("store_template") != "store-template.json":
        failures.append(f"a loader manifest's store_template is {loader.get('store_template')!r}")
    if loader.get("store_template_sha256") != hashlib.sha256(template).hexdigest():
        failures.append("a loader manifest doesn't carry its store template's SHA-256")
    return failures


def main():
    failures = []
    for case in CASES["cases"]:
        table = {k: v[:2] for k, v in resolve(case["new_table"]).items()}
        if (not check_table(table, CASES["flash_bytes"])) != case["table_ok"]:
            failures.append(f"check_table disagrees on: {case['name']}")
        if case["expect"] in PAGE_ONLY:
            continue
        try:
            check_layout(parts_for(case), BOARD)
            refused = None
        except SystemExit as e:
            refused = str(e)
        if (refused is None) != (case["expect"] == "ok"):
            failures.append(f"check_layout {'refused' if refused else 'accepted'}: {case['name']} ({refused})")
    failures += check_tags()
    failures += check_manifest()
    failures += check_macs()
    for f in failures:
        print("tables: " + f)
    if failures:
        return 1
    print(f"tables: build_site agrees on {len(CASES['cases'])} shared install cases and {len(TAGS['cases'])} build tags")
    return 0


if __name__ == "__main__":
    sys.exit(main())
