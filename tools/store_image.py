"""Builds the Waveshare's `store` partition as a LittleFS image, on the Mac.

    build/tools-venv/bin/python tools/store_image.py <partitions.csv> <out.bin>

The image holds JieLi's loader at loader/wl82loader.bin (from tests/fixtures,
checked against its SHA-256) and nothing else: the store's first setup. It is
JieLi's code, so the image is gitignored and never staged by `make site` or CI.
Flashing it erases every backup and save already in the store
(`make flash-store CONFIRM=erase-store`).

Geometry matches esp_littlefs in the ESP32 Arduino core 3.3.12 (sdkconfig.h):
4 KiB blocks, 128-byte reads and programs, names up to 64 bytes. The image is
written as littlefs disk version 2.0, which every 2.x driver mounts. After
writing, the image is mounted again and the blob read back.

Arduino's own mklittlefs is x86-only on Apple Silicon, so this uses
littlefs-python (make store-image installs it into build/tools-venv).
"""
import hashlib
import sys
from pathlib import Path

import littlefs

ROOT = Path(__file__).resolve().parent.parent
BLOB = ROOT / "tests" / "fixtures" / "wl82loader.bin"
BLOB_SHA256 = "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376"
BLOCK = 4096
GEOMETRY = dict(block_size=BLOCK, read_size=128, prog_size=128, block_cycles=512,
                name_max=64, disk_version=0x00020000)


def superblock(image):
    """(disk_version, block_size, block_count, name_max) from a littlefs image.

    The superblock lives in the metadata pair at blocks 0 and 1; the newer one
    has the higher revision count (its first 4 bytes). In it the name entry
    "littlefs" is followed by a 4-byte tag and the inline superblock struct
    (version, block size, block count, name max, ...), all little-endian. The
    last copy in the block is the newest commit. None if it can't be found.
    """
    import struct
    blocks = sorted((struct.unpack_from("<I", image, b * BLOCK)[0], b) for b in (0, 1))
    for _, b in reversed(blocks):
        blk = image[b * BLOCK:(b + 1) * BLOCK]
        i = blk.rfind(b"littlefs")
        if i >= 0 and i + 28 <= len(blk):
            return struct.unpack_from("<IIII", blk, i + 12)
    return None


def store_partition(csv_path):
    for line in Path(csv_path).read_text().splitlines():
        fields = [f.strip() for f in line.split("#", 1)[0].split(",")]
        if fields and fields[0] == "store":
            return int(fields[3], 0), int(fields[4], 0)
    sys.exit("store_image: no 'store' partition in %s" % csv_path)


def main():
    csv_path, out = sys.argv[1], Path(sys.argv[2])
    offset, size = store_partition(csv_path)
    if size % BLOCK:
        sys.exit("store_image: store size 0x%x isn't whole blocks" % size)
    blob = BLOB.read_bytes() if BLOB.is_file() else b""
    if hashlib.sha256(blob).hexdigest() != BLOB_SHA256:
        sys.exit("store_image: %s missing or wrong: run `make fixtures`" % BLOB)

    fs = littlefs.LittleFS(block_count=size // BLOCK, **GEOMETRY)
    fs.makedirs("loader", exist_ok=True)
    with fs.open("loader/wl82loader.bin", "wb") as f:
        f.write(blob)
    image = bytes(fs.context.buffer)
    if len(image) != size:
        sys.exit("store_image: image is %d bytes, partition %d" % (len(image), size))

    # Mount what was written, as the device will, and read the blob back.
    check = littlefs.LittleFS(block_count=size // BLOCK, mount=False, **GEOMETRY)
    check.context.buffer[:] = image
    check.mount()
    with check.open("loader/wl82loader.bin", "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != BLOB_SHA256:
            sys.exit("store_image: the blob doesn't read back from the image")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print("store_image: %s, %d bytes for the store partition at 0x%x (loader blob verified)"
          % (out, len(image), offset))


if __name__ == "__main__":
    main()
