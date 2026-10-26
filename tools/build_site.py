#!/usr/bin/env python3
"""Stage firmware into site/ for the ESP Web Tools installer.

    tools/build_site.py <board-id> <arduino build dir> <boot_app0.bin> <version>

Writes site/firmware/<board-id>/{bootloader,partitions,boot_app0,app}.bin and
site/firmware/<board-id>/manifest.json.

Why four parts and not one merged image: the ESP Web Tools docs merge so that
esptool can patch flash mode/size into the bootloader header. Arduino already
writes the right header (dio, 80 MHz, 16 MB). A merged image starting at 0x0
also fills 0x9000-0xDFFF with 0xFF, which erases NVS (settings, Wi-Fi, the
loader's unit data). Arduino's own .merged.bin is worse: padded to 16 MB, it
would wipe every partition. Four parts write exactly what `make flash` writes.

Licensing guard: JieLi's wl82loader.bin must never ship. The script refuses to
publish a file with its name or SHA-256, and when a local copy of the blob is
around (tests/fixtures/, or $WL82_BLOB) it refuses any file containing a 1 KiB
piece of it.
"""
import hashlib
import json
import os
import shutil
import struct
import sys
from pathlib import Path

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

BOARDS = {
    "waveshare-esp32-s3-touch-lcd-1.69": {
        "sketch": "waveshare_169",
        "name": "usb-midi-host (Waveshare ESP32-S3-Touch-LCD-1.69)",
        "chip": "ESP32-S3",
        "flash_bytes": 16 * 1024 * 1024,
    },
}


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


def check_layout(parts, board):
    """Every part must end before the first data partition, so installs never touch NVS or storage."""
    table = dict((name, (off, size)) for name, off, size in read_partition_table(parts[0x8000]))
    if "nvs" not in table or "app0" not in table:
        fail(f"unexpected partition table: {sorted(table)}")
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
    header = parts[0x0][:4]
    if header[0] != 0xE9:
        fail("bootloader has no image header")
    size_code = header[3] >> 4  # 0=1MB, 1=2MB, 2=4MB, 3=8MB, 4=16MB
    if (1 << size_code) * 1024 * 1024 != board["flash_bytes"]:
        fail(f"bootloader header says {1 << size_code} MB flash")


def check_licensing(files):
    blob = None
    for candidate in (os.environ.get("WL82_BLOB"), ROOT / "tests/fixtures/wl82loader.bin"):
        if candidate and Path(candidate).is_file():
            blob = Path(candidate).read_bytes()
            break
    chunks = [blob[i:i + WL82_CHUNK] for i in range(0, len(blob) - WL82_CHUNK + 1, WL82_CHUNK)] if blob else []
    for path in files:
        if "wl82" in path.name.lower():
            fail(f"{path} looks like JieLi's loader blob; it must not be published")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() == WL82_SHA256:
            fail(f"{path} is JieLi's wl82loader.bin; it must not be published")
        for chunk in chunks:
            if chunk in data:
                fail(f"{path} contains part of JieLi's wl82loader.bin; it must not be published")
    return blob is not None


def main():
    if len(sys.argv) != 5:
        fail(__doc__.strip().splitlines()[2].strip())
    board_id, build_dir, boot_app0, version = sys.argv[1:]
    board = BOARDS.get(board_id) or fail(f"unknown board {board_id}; known: {', '.join(BOARDS)}")
    build_dir = Path(build_dir)

    parts = {}
    for offset, _, src in PARTS:
        path = Path(boot_app0) if src is None else build_dir / src.format(sketch=board["sketch"])
        if not path.is_file():
            fail(f"missing {path}; run `make build` first")
        parts[offset] = path.read_bytes()
    check_layout(parts, board)

    out = SITE / "firmware" / board_id
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    for offset, name, _ in PARTS:
        (out / name).write_bytes(parts[offset])

    manifest = {
        "name": board["name"],
        "version": version,
        # Without this, ESP Web Tools erases the whole chip on every install of
        # firmware that doesn't speak Improv. With it, the user gets an "Erase
        # device" box that starts unticked.
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [{
            "chipFamily": board["chip"],
            "improv": False,
            "parts": [{"path": name, "offset": offset} for offset, name, _ in PARTS],
        }],
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    try:
        scanned_blob = check_licensing(sorted(p for p in SITE.rglob("*") if p.is_file()))
    except SystemExit:
        shutil.rmtree(out)  # never leave a refused build where `make serve` would serve it
        raise
    total = sum(len(d) for d in parts.values())
    print(f"site/firmware/{board_id}: {version}, {total} bytes in {len(PARTS)} parts"
          f"{'' if scanned_blob else ' (no local wl82loader.bin: checked names and SHA only)'}")


if __name__ == "__main__":
    main()
