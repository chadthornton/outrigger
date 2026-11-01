#!/usr/bin/env python3
"""Stage firmware into site/ for the browser installer (site/installer/).

    tools/build_site.py <board> <app set> <arduino build dir> <boot_app0.bin> <version> [<store-template.json>]

Writes site/firmware/<board-id>/<app set>/{bootloader,partitions,boot_app0,app}.bin
and its manifest.json. App sets with the loader also get store-template.json
(tools/store_template.py): the store the installer writes onto a blank board,
with a placeholder where the browser puts JieLi's loader. The board is named by the Makefile's BOARD= (its profile in
firmware/boards.json gives the site's folder name), the app set by APPS= (docs/architecture.md, "App sets and builds").

Why four parts and not one merged image: esptool can patch flash mode/size
into a merged image's bootloader header, but Arduino already
writes the right header (dio, 80 MHz, 16 MB). A merged image starting at 0x0
also fills 0x9000-0xDFFF with 0xFF, which erases NVS (settings, Wi-Fi, the
loader's unit data). Arduino's own .merged.bin is worse: padded to 16 MB, it
would wipe every partition. Four parts write exactly what `make flash` writes.

Licensing guard: JieLi's wl82loader.bin must never ship. The script refuses to
publish a file with its name or SHA-256, and when a local copy of the blob is
around (tests/fixtures/, or $WL82_BLOB) it refuses any file containing a 1 KiB
piece of it. FM-1 firmware packages must never ship either (the installer's
browser downloads them from the Firmware Hub): no .fwsc file, no copy of a
fixture package, and no JSON file holding the hex of a 1 KiB piece of a
fixture package's app (the store template is JSON; its own test checks it
piece by piece).
"""
import hashlib
import json
import os
import shutil
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import boards as board_table  # noqa: E402
import fm1lib  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "site"

WL82_SHA256 = "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376"
WL82_CHUNK = 1024

# offset, file in site/, file in the Arduino build dir (None: boot_app0 argument)
PARTS = [
    (0x0, "bootloader.bin", "{sketch}.ino.bootloader.bin"),
    (0x8000, "partitions.bin", "{sketch}.ino.partitions.bin"),
    (0xE000, "boot_app0.bin", None),
    (0x10000, "app.bin", "{sketch}.ino.bin"),
]

# Every board builds the one sketch, so its files carry the sketch's name.
SKETCH = "waveshare_169"


def site_boards(table):
    """The profiles as the installer sees them, by their folder under site/firmware/."""
    return {b["site_id"]: {
        "make_id": make_id,
        "name": f"usb-midi-host ({b['name']})",
        "short_name": b["short_name"],
        "chip": b["chip"],
        "flash_bytes": b["flash_mb"] * 1024 * 1024,
        "tag_id": b["tag_id"],
    } for make_id, b in board_table.checked(table)["boards"].items()}


BOARDS = site_boards(board_table.load())


# Installer names for the Makefile's app sets.
APP_SETS = {
    "hub": "MIDI Hub",
    "hub-loader": "MIDI Hub + Firmware Loader",
    "everything": "Everything",
}


# Each app set's label in the build tag (lib/platform/src/apps.h kAppSetLabel).
# hub-loader may carry the Wi-Fi diagnostics page (DIAG_WEB=1).
APP_SET_LABELS = {"hub": {"hub"}, "hub-loader": {"hub+L", "hub+LW"}, "everything": {"all"}}

# The build tag (lib/platform/src/build_tag.h), right after esp_app_desc_t at
# app image offset 0x120. The page reads it off a board to name the build it
# runs; tests/installer/tags.json holds the cases both readers run.
TAG_OFFSET, TAG_MAGIC, TAG_LAYOUT = 0x120, 0x54484D55, 1


def fail(msg):
    sys.exit(f"build_site: {msg}")


def read_partition_table(data):
    """Yield (name, offset, size) for each entry of an ESP partition table."""
    for i in range(0, len(data), 32):
        entry = data[i:i + 32]
        if entry[:2] != b"\xaa\x50":
            return
        offset, size = struct.unpack("<II", entry[4:12])
        yield entry[12:28].rstrip(b"\0").decode(), offset, size


# The layout every install keeps (docs/firmware-loader-plan.md, session 2): NVS
# and otadata where Arduino's default table has them, so settings survive a
# table change, the app at 0x10000, and the store after every app slot.
FIXED = {"nvs": (0x9000, 0x5000), "otadata": (0xE000, 0x2000)}
APP0_OFFSET, APP0_SIZE = 0x10000, 0x300000


def check_table(table, flash_bytes):
    """table: name -> (offset, size). Returns a list of problems."""
    problems = []
    for name, want in FIXED.items():
        if table.get(name) != want:
            problems.append(f"{name} is {table.get(name)}, must stay at {want}")
    if table.get("app0", (None, 0))[0] != APP0_OFFSET or table.get("app0", (0, 0))[1] < APP0_SIZE:
        problems.append(f"app0 is {table.get('app0')}, must start at 0x10000 with >= 3 MB")
    if "store" in table:
        store_off, store_size = table["store"]
        if store_off % 0x1000 or store_off + store_size > flash_bytes:
            problems.append("store is misaligned or runs past the flash")
        for name, (off, size) in table.items():
            if name != "store" and off < store_off + store_size and off + size > store_off:
                problems.append(f"{name} overlaps the store")
    ends = sorted((off, off + size, name) for name, (off, size) in table.items())
    for (a0, a1, an), (b0, b1, bn) in zip(ends, ends[1:]):
        if b0 < a1:
            problems.append(f"{an} overlaps {bn}")
    return problems


def check_layout(parts, board):
    """Every part must end before the first data partition, so installs never touch NVS or storage."""
    table = dict((name, (off, size)) for name, off, size in read_partition_table(parts[0x8000]))
    if "nvs" not in table or "app0" not in table:
        fail(f"unexpected partition table: {sorted(table)}")
    for problem in check_table(table, board["flash_bytes"]):
        fail(problem)
    nvs_off, nvs_size = table["nvs"]
    app_off, app_size = table["app0"]
    if table.get("otadata", (None,))[0] != 0xE000:
        fail("otadata is not at 0xe000; the manifest writes boot_app0 there")
    if app_off != 0x10000:
        fail(f"app0 at {app_off:#x}, manifest writes the app at 0x10000")
    if len(parts[0x10000]) > app_size:
        fail(f"app is {len(parts[0x10000])} bytes, app0 holds {app_size}")
    if 0x8000 + len(parts[0x8000]) > nvs_off or 0xE000 < nvs_off + nvs_size:
        fail("a part overlaps NVS")
    if len(parts[0x0]) > 0x8000:
        fail("bootloader runs into the partition table")
    if 0xE000 + len(parts[0xE000]) > 0x10000:
        fail("boot_app0 runs past otadata into app0 at 0x10000")
    for offset, data in parts.items():
        if not data:
            fail(f"the part at {offset:#x} is empty")
    header = parts[0x0][:4]
    if header[0] != 0xE9:
        fail("bootloader has no image header")
    size_code = header[3] >> 4  # 0=1MB, 1=2MB, 2=4MB, 3=8MB, 4=16MB
    if (1 << size_code) * 1024 * 1024 != board["flash_bytes"]:
        fail(f"bootloader header says {1 << size_code} MB flash")


def read_build_tag(app):
    """{commit, dirty, app_set, board} from an app image, or None without a tag."""
    tag = app[TAG_OFFSET:TAG_OFFSET + 20]
    if len(tag) < 20:
        return None
    magic, layout, dirty, board, commit = struct.unpack("<IHBBI", tag[:12])
    if magic != TAG_MAGIC or layout != TAG_LAYOUT:
        return None
    return {"commit": f"{commit:07x}", "dirty": bool(dirty), "app_set": tag[12:].split(b"\0")[0].decode(),
            "board": board}


def check_build_tag(app, app_set, board):
    """The tag the page will read off a board; refuses a build without one."""
    tag = read_build_tag(app)
    if tag is None:
        fail(f"the app has no build tag at {TAG_OFFSET:#x}; the installer couldn't name it on a board")
    if tag["app_set"] not in APP_SET_LABELS[app_set]:
        fail(f"the app's build tag says {tag['app_set']!r}, staged as {app_set}")
    if tag["board"] != board["tag_id"]:
        fail(f"the app's build tag says board {tag['board']}, staged as {board['make_id']} ({board['tag_id']})")
    return tag


# App sets whose firmware runs the loader, so the installer sets up a store for it.
LOADER_APP_SETS = ("hub-loader", "everything")


def make_manifest(board, app_set, version, tag, parts, store_template=None):
    """manifest.json for one app set. parts: offset -> bytes; store_template:
    the bytes of store-template.json for an app set that runs the loader."""
    manifest = {
        "name": f"{board['name']}: {APP_SETS[app_set]}",
        "version": version,
        # What the board will say it runs once installed (site/installer/board.js).
        "build": tag,
        "builds": [{
            "chipFamily": board["chip"],
            # The page checks each download by SHA-256, and each part on the
            # board by the MD5 the flasher computes after writing.
            "parts": [{"path": name, "offset": offset,
                       "md5": hashlib.md5(parts[offset]).hexdigest(),
                       "sha256": hashlib.sha256(parts[offset]).hexdigest()} for offset, name, _ in PARTS],
        }],
    }
    if store_template is not None:
        # Written onto a factory-blank store, with the loader fetched from GitHub (site/installer/store.js).
        manifest["store_template"] = "store-template.json"
        manifest["store_template_sha256"] = hashlib.sha256(store_template).hexdigest()
    return manifest


def check_licensing(files):
    blob = None
    for candidate in (os.environ.get("WL82_BLOB"), ROOT / "tests/fixtures/wl82loader.bin"):
        if candidate and Path(candidate).is_file():
            blob = Path(candidate).read_bytes()
            break
    chunks = [blob[i:i + WL82_CHUNK] for i in range(0, len(blob) - WL82_CHUNK + 1, WL82_CHUNK)] if blob else []
    package_sha = set()
    app_hex = []
    for pkg in sorted((ROOT / "tests/fixtures").glob("*.fwsc")):
        raw = pkg.read_bytes()
        package_sha.add(hashlib.sha256(raw).hexdigest())
        app = fm1lib.parse_package(raw)[1]
        for i in range(0, len(app) - WL82_CHUNK + 1, 0x1000):  # the start of every sector
            c = app[i:i + WL82_CHUNK]
            if c.count(c[0]) != len(c):  # all-0xFF and all-zero pieces are in every image
                app_hex.append((pkg.name, c.hex().encode()))
    for path in files:
        if "wl82" in path.name.lower():
            fail(f"{path} looks like JieLi's loader blob; it must not be published")
        if path.suffix.lower() == ".fwsc":
            fail(f"{path} is an FM-1 firmware package; it must not be published")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() == WL82_SHA256:
            fail(f"{path} is JieLi's wl82loader.bin; it must not be published")
        for chunk in chunks:
            if chunk in data:
                fail(f"{path} contains part of JieLi's wl82loader.bin; it must not be published")
        if hashlib.sha256(data).hexdigest() in package_sha:
            fail(f"{path} is an FM-1 firmware package; it must not be published")
        for name, chunk in app_hex if path.suffix == ".json" else []:
            if chunk in data:
                fail(f"{path} contains part of the firmware package {name}; it must not be published")
    return blob is not None


def main():
    if len(sys.argv) not in (6, 7):
        fail(__doc__.strip().splitlines()[2].strip())
    make_id, app_set, build_dir, boot_app0, version = sys.argv[1:6]
    template = Path(sys.argv[6]) if len(sys.argv) == 7 else None
    board_id = next((k for k, b in BOARDS.items() if b["make_id"] == make_id), None)
    if board_id is None:
        fail(f"unknown board {make_id}; known: {', '.join(b['make_id'] for b in BOARDS.values())}")
    board = BOARDS[board_id]
    if app_set not in APP_SETS:
        fail(f"unknown app set {app_set}; known: {', '.join(APP_SETS)}")
    build_dir = Path(build_dir)
    if app_set in LOADER_APP_SETS and not (template and template.is_file()):
        fail(f"{app_set} runs the loader: pass its store-template.json (tools/store_template.py)")

    parts = {}
    for offset, _, src in PARTS:
        path = Path(boot_app0) if src is None else build_dir / src.format(sketch=SKETCH)
        if not path.is_file():
            fail(f"missing {path}; run `make build` first")
        parts[offset] = path.read_bytes()
    check_layout(parts, board)
    tag = check_build_tag(parts[0x10000], app_set, board)

    out = SITE / "firmware" / board_id / app_set
    # Builds before app sets were staged flat in the board folder; don't keep serving them.
    for stale in (SITE / "firmware" / board_id).glob("*"):
        if stale.is_file():
            stale.unlink()
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    for offset, name, _ in PARTS:
        (out / name).write_bytes(parts[offset])

    template_bytes = template.read_bytes() if app_set in LOADER_APP_SETS else None
    if template_bytes is not None:
        (out / "store-template.json").write_bytes(template_bytes)
    manifest = make_manifest(board, app_set, version, tag, parts, store_template=template_bytes)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    try:
        scanned_blob = check_licensing(sorted(p for p in SITE.rglob("*") if p.is_file()))
    except SystemExit:
        shutil.rmtree(out)  # never leave a refused build where `make serve` would serve it
        raise
    total = sum(len(d) for d in parts.values())
    print(f"site/firmware/{board_id}/{app_set}: {version}, {total} bytes in {len(PARTS)} parts"
          f"{'' if scanned_blob else ' (no local wl82loader.bin: checked names and SHA only)'}")


if __name__ == "__main__":
    main()
