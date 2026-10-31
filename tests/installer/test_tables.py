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
from build_site import BOARDS, check_build_tag, check_layout, check_table, make_manifest, read_build_tag  # noqa: E402
from check_partitions import read_csv  # noqa: E402

CASES = json.loads((ROOT / "tests/installer/tables.json").read_text())
TAGS = json.loads((ROOT / "tests/installer/tags.json").read_text())
OURS = read_csv(ROOT / "firmware/waveshare_169/partitions.csv")
BOARD = BOARDS["waveshare-esp32-s3-touch-lcd-1.69"]

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
    untagged = app_with("ff" * 20)
    if refuses(check_build_tag, loader, "hub-loader") or refuses(check_build_tag, hub, "hub"):
        failures.append("check_build_tag refused a matching tag")
    if not refuses(check_build_tag, untagged, "hub"):
        failures.append("check_build_tag accepted an app with no tag")
    if not refuses(check_build_tag, hub, "hub-loader"):
        failures.append("check_build_tag accepted a MIDI Hub app staged as hub-loader")
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
    for f in failures:
        print("tables: " + f)
    if failures:
        return 1
    print(f"tables: build_site agrees on {len(CASES['cases'])} shared install cases and {len(TAGS['cases'])} build tags")
    return 0


if __name__ == "__main__":
    sys.exit(main())
