"""The store the web installer writes onto a factory-blank store: the same
littlefs image `make store-image` builds, but with a placeholder where
JieLi's loader goes, so the site can publish it.

    build/tools-venv/bin/python tools/store_template.py <partitions.csv> <out.json>

The installer downloads wl82loader.bin from the pinned jl-uboot-tool commit
into the visitor's browser, checks its SHA-256, copies it over the
placeholder at `extents`, and writes `blocks` into the erased store. littlefs
checksums its metadata, not file contents, so the result mounts as if
`make store-image` had built it (tests/installer/test_store_template.py).

The placeholder is 32-bit words 0xA5000000 + n (n = the word's index in the
loader), so each run of it in the image says which part of the loader goes
there. littlefs's own words in a data block (its skip-list pointers) are
block numbers, far below 0xA5000000, so they can't be mistaken for it.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

import littlefs

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_image import BLOB_SHA256, BLOCK, GEOMETRY, store_partition  # noqa: E402

LOADER_PATH = "loader/wl82loader.bin"
LOADER_SIZE = 24064
LOADER_URL = ("https://raw.githubusercontent.com/kagaimiq/jl-uboot-tool/"
              "adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a/data/loaderblobs/usb/wl82loader.bin")
MARK = 0xA5000000


def build_template(store_size):
    """A dict for store-template.json: the store's used blocks, with the
    placeholder, and where each piece of the loader goes."""
    words = LOADER_SIZE // 4
    placeholder = b"".join(struct.pack("<I", MARK + n) for n in range(words))
    fs = littlefs.LittleFS(block_count=store_size // BLOCK, **GEOMETRY)
    fs.makedirs("loader", exist_ok=True)
    with fs.open(LOADER_PATH, "wb") as f:
        f.write(placeholder)
    image = bytes(fs.context.buffer)

    blocks = [off for off in range(0, store_size, BLOCK) if image[off:off + BLOCK] != b"\xff" * BLOCK]
    # Runs of consecutive placeholder words, by image offset.
    extents = []
    for off in blocks:
        at = off
        while at < off + BLOCK:
            (word,) = struct.unpack_from("<I", image, at)
            if word & 0xFF000000 != MARK or (word - MARK) >= words:
                at += 4
                continue
            start, n = at, word - MARK
            while at < off + BLOCK and struct.unpack_from("<I", image, at)[0] == MARK + n + (at - start) // 4:
                at += 4
            extents.append([start, n * 4, at - start])
    return {
        "size": store_size,
        "blank_md5": hashlib.md5(b"\xff" * store_size).hexdigest(),
        "loader": {"path": LOADER_PATH, "url": LOADER_URL, "sha256": BLOB_SHA256},
        "blob_size": LOADER_SIZE,
        "blocks": [{"offset": off, "hex": image[off:off + BLOCK].hex()} for off in blocks],
        "extents": extents,
    }


def splice(template, blob):
    """[(offset, bytes)] to write: the template's blocks with the loader in place."""
    out = {b["offset"]: bytearray.fromhex(b["hex"]) for b in template["blocks"]}
    for img, src, n in template["extents"]:
        block = img - img % BLOCK
        out[block][img - block:img - block + n] = blob[src:src + n]
    return [(off, bytes(data)) for off, data in sorted(out.items())]


def main():
    _, size = store_partition(sys.argv[1])
    Path(sys.argv[2]).write_text(json.dumps(build_template(size)) + "\n")


if __name__ == "__main__":
    main()
