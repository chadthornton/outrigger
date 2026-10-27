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
