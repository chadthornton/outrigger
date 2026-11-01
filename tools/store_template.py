"""The store the web installer writes onto a factory-blank store: JieLi's
loader and a starter set of FM-1 firmwares, laid out by littlefs as `make
store-image` and `make store-add` would, but published without any of their
bytes.

    build/tools-venv/bin/python tools/store_template.py <partitions.csv> <out.json> [fixtures dir]

The build machine has the files (tests/fixtures, pinned by SHA-256), so it
builds the real store and publishes what the visitor's browser needs to
rebuild it:
- the file system's own blocks (directories, manifests, the loader's blocks
  with a placeholder where its bytes go) as hex, trailing 0xFF trimmed;
- every block that is one 4 KiB sector of a firmware as just that sector's
  SHA-256 (fm1lib's store keeps each sector as a one-block file);
- the MD5 of each run of blocks as written, and the SHA-256 of the whole.
The browser downloads the loader from the pinned jl-uboot-tool commit and
each package from the Firmware Hub, checks their SHA-256, decodes each
package to its app (site/installer/store.js), and fills the blocks in. If
the SHA-256 of what it built isn't the template's, it writes nothing.

littlefs checksums its metadata, not file contents, so the placeholder
image's metadata is the real one's (tests/installer/test_store_template.py
checks the spliced store mounts and holds what store-add writes).

The loader's placeholder is 32-bit words 0xA5000000 + n (n = the word's index
in the loader), so each run of it says which part of the loader goes there.
Only the blocks where the placeholder and the real loader differ are scanned.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

import littlefs

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm1lib  # noqa: E402
from store_add import LfsStore  # noqa: E402
from store_image import BLOB_SHA256, BLOCK, GEOMETRY, store_partition  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
LOADER_PATH = "loader/wl82loader.bin"
LOADER_SIZE = 24064
LOADER_URL = ("https://raw.githubusercontent.com/kagaimiq/jl-uboot-tool/"
              "adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a/data/loaderblobs/usb/wl82loader.bin")
MARK = 0xA5000000
HUB = "https://thefirmwarehub.com/"

# The starter set (Chad, 2026-10-31, tracker ask fw-add-browser): stock and the
# three projects proven on hardware, at the catalog's versions that day. Named
# as tools/store_add.py names catalog packages. The Firmware Hub keeps only
# each project's latest file, so when one releases, its URL here stops
# answering and the installer sets up no store until these are bumped (with
# tools/fetch_fixtures.py's pins) and the site deployed again.
STARTER = [
    # id, name, version, project, fixture, Firmware Hub path, sha256
    ("stock-V15", "M-VAVE stock", "V15", "stock", "FM-1_015.fwsc", "fw/mvave/FM-1_015.fwsc",
     "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a"),
    ("felucca-1.8.0", "Felucca", "1.8.0", "felucca", "felucca-1.8.0.fwsc", "fw/felucca-1.8.0.fwsc",
     "a31760f2588b39bcb3a8d6f78f6041a1a550978a972ecd71564e45513698ecd6"),
    ("skiff-2.1.1", "Skiff", "2.1.1", "skiff", "skiff-2.1.1.fwsc", "fw/skiff-2.1.1.fwsc",
     "0cfae2431f351365d83842bf1a381ee48baead371e81f77faaa2b3c6aabbd304"),
    ("sloop-2.8.4", "SLOOP", "2.8.4", "sloop", "sloop-2.8.4.fwsc", "fw/sloop-2.8.4.fwsc",
     "5a6a0843c868650dba6718cf85e43e719aec18c3dc5cc803c57c38b382158d4e"),
]


def read_inputs(fixtures):
    """(loader, {id: package bytes}) from the fixtures, each checked against its pin."""
    fixtures = Path(fixtures)
    want = [("wl82loader.bin", BLOB_SHA256)] + [(s[4], s[6]) for s in STARTER]
    data = {}
    for name, sha in want:
        path = fixtures / name
        data[name] = path.read_bytes() if path.is_file() else b""
        if hashlib.sha256(data[name]).hexdigest() != sha:
            sys.exit("store_template: %s missing or wrong: run `make fixtures`" % path)
    return data["wl82loader.bin"], {s[0]: data[s[4]] for s in STARTER}


def build_image(store_size, loader, packages):
    fs = littlefs.LittleFS(block_count=store_size // BLOCK, **GEOMETRY)
    fs.makedirs("loader", exist_ok=True)
    with fs.open(LOADER_PATH, "wb") as f:
        f.write(loader)
    store = LfsStore(fs)
    for pkg_id, name, version, project, *_ in STARTER:
        fm1lib.add_package(store, pkg_id, packages[pkg_id], name=name, version=version, project=project)
    fs.unmount()
    return bytes(fs.context.buffer)


def loader_extents(image, real, words):
    """Runs of consecutive placeholder words, in the blocks where the loader is: [image offset, loader offset, bytes]."""
    extents = []
    for off in range(0, len(image), BLOCK):
        if image[off:off + BLOCK] == real[off:off + BLOCK]:
            continue
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
    return extents


def runs_of(blocks):
    """Contiguous runs of block offsets as (offset, size), the superblock pair (blocks 0 and 1) last:
    until it's written, a store cut short reads as no store, never as a store."""
    runs = []
    for off in blocks:
        if runs and runs[-1][0] + runs[-1][1] == off and off != 2 * BLOCK:
            runs[-1][1] += BLOCK
        else:
            runs.append([off, BLOCK])
    if not runs or runs[0] != [0, 2 * BLOCK]:
        sys.exit("store_template: the store doesn't start with its superblock pair")
    return [tuple(r) for r in runs[1:] + runs[:1]]


def build_template(store_size, loader, packages):
    """A dict for store-template.json (see the module docstring)."""
    words = LOADER_SIZE // 4
    if len(loader) != LOADER_SIZE:
        sys.exit("store_template: the loader is %d bytes, expected %d" % (len(loader), LOADER_SIZE))
    placeholder = b"".join(struct.pack("<I", MARK + n) for n in range(words))
    real = build_image(store_size, loader, packages)
    image = build_image(store_size, placeholder, packages)

    sectors = set()
    meta = []
    for pkg_id, name, version, project, _, path, sha in STARTER:
        ident, app = fm1lib.parse_package(packages[pkg_id])
        sectors |= {hashlib.sha256(app[i:i + BLOCK]).hexdigest() for i in range(0, len(app), BLOCK)}
        meta.append({"id": pkg_id, "name": name, "version": version, "project": project, "identity": ident,
                     "url": HUB + path, "sha256": sha, "app_sha256": hashlib.sha256(app).hexdigest()})

    used = [off for off in range(0, store_size, BLOCK) if real[off:off + BLOCK] != b"\xff" * BLOCK]
    blocks = []
    for off in used:
        digest = hashlib.sha256(real[off:off + BLOCK]).hexdigest()
        if digest in sectors:
            blocks.append({"offset": off, "sector": digest})
        else:
            blocks.append({"offset": off, "hex": image[off:off + BLOCK].rstrip(b"\xff").hex()})
    runs = runs_of(used)
    template = {
        "size": store_size,
        "blank_md5": hashlib.md5(b"\xff" * store_size).hexdigest(),
        "loader": {"path": LOADER_PATH, "url": LOADER_URL, "sha256": BLOB_SHA256},
        "blob_size": LOADER_SIZE,
        "packages": meta,
        "blocks": blocks,
        "extents": loader_extents(image, real, words),
        "runs": [{"offset": off, "size": n, "md5": hashlib.md5(real[off:off + n]).hexdigest()} for off, n in runs],
        "sha256": hashlib.sha256(b"".join(off.to_bytes(4, "little") + real[off:off + BLOCK] for off in used)).hexdigest(),
    }
    spliced = dict(splice(template, loader, packages))
    if any(spliced[off] != real[off:off + BLOCK] for off in used):
        sys.exit("store_template: the template doesn't rebuild the store it came from")
    return template


def splice(template, loader, packages):
    """[(offset, block)] of the store: the template's blocks filled in from the loader and packages."""
    by_hash = {}
    for data in packages.values():
        app = fm1lib.parse_package(data)[1]
        for i in range(0, len(app), BLOCK):
            by_hash[hashlib.sha256(app[i:i + BLOCK]).hexdigest()] = app[i:i + BLOCK]
    out = {}
    for b in template["blocks"]:
        if "sector" in b:
            out[b["offset"]] = bytearray(by_hash[b["sector"]])
        else:
            out[b["offset"]] = bytearray(bytes.fromhex(b["hex"]).ljust(BLOCK, b"\xff"))
    for img, src, n in template["extents"]:
        block = img - img % BLOCK
        out[block][img - block:img - block + n] = loader[src:src + n]
    return [(off, bytes(data)) for off, data in sorted(out.items())]


def write_runs(template, spliced):
    """[(offset, bytes)] in the order the installer writes them."""
    blocks = dict(spliced)
    return [(r["offset"], b"".join(blocks[r["offset"] + i] for i in range(0, r["size"], BLOCK)))
            for r in template["runs"]]


def main():
    _, size = store_partition(sys.argv[1])
    loader, packages = read_inputs(sys.argv[3] if len(sys.argv) > 3 else ROOT / "tests" / "fixtures")
    Path(sys.argv[2]).write_text(json.dumps(build_template(size, loader, packages)) + "\n")


if __name__ == "__main__":
    main()
