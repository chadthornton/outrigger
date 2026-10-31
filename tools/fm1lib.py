"""The Waveshare's FM-1 library, written from the Mac: a Python port of
lib/fm1_loader's add_package() and its store layout, so packages can be added
to the board's store without the board (tools/store_add.py).

Layout (lib/fm1_loader/src/store.cpp, library.cpp, posix_storage.cpp):
- a 4 KiB sector lives at s/<first 2 hex of its SHA-256>/<other 62>; blank
  (all 0xFF) sectors are never stored;
- a package is the text manifest m/pkg/<id>: "key value" lines in key order,
  then "@<addr, 5 hex> <sha256>" lines for the 143 app sectors.
make test cross-checks this against the C++ (tests/test_fm1_interop.cpp).

Standard library only. `python3 tools/fm1lib.py selftest <dir> <fwsc>...` adds
the packages to a plain directory laid out like the store.
"""
import hashlib
import os
import re
import sys
from pathlib import Path

SECTOR = 0x1000
HEAD_END, APP_START, APP_END = 0x4000, 0x4000, 0x93000
FWSC_BLOCK, FWSC_MARKED, FWSC_NO_CHAR = 0x2F, 20, 0x7D
BLANK = b"\xff" * SECTOR
ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


class PackageError(Exception):
    pass


def crc16(data, c=0):
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def jl_enc(buf):
    out, k = bytearray(buf), 0xFFFF
    for i in range(len(out)):
        out[i] ^= k & 0xFF
        k = ((k << 1) ^ (0x1021 if k & 0x8000 else 0)) & 0xFFFF
    return bytes(out)


def u16(p, o):
    return p[o] | p[o + 1] << 8


def u32(p, o):
    return u16(p, o) | u16(p, o + 2) << 16


def parse_package(data):
    """(identity, app bytes) of a .fwsc file, with package.cpp's checks."""
    stride = FWSC_BLOCK + 1
    if len(data) < stride * FWSC_MARKED:
        raise PackageError("too short to be an FM-1 package")
    image, ident = bytearray(), ""
    for i in range(FWSC_MARKED):
        block = data[i * stride:(i + 1) * stride]
        image += block[:FWSC_BLOCK]
        if block[FWSC_BLOCK] != FWSC_NO_CHAR:
            ident += chr((block[FWSC_BLOCK] - i - 1) & 0xFF)
    image += data[stride * FWSC_MARKED:]
    if len(image) < 0x400:
        raise PackageError("too short to be an FM-1 package")
    hdr = jl_enc(image[:0x40])
    if crc16(hdr[2:0x40]) != u16(hdr, 0):
        raise PackageError("UFW header CRC fails")
    n = u16(hdr, 8)
    if n == 0 or n > 11:
        raise PackageError("UFW header lists a bad entry count")
    if crc16(image[0x40:0x40 + n * 0x50]) != u16(hdr, 2):
        raise PackageError("UFW entry list CRC fails")
    fl = None
    for i in range(n):
        e = jl_enc(image[0x40 + i * 0x50:0x90 + i * 0x50])
        if u16(e, 0) == 0:
            fl = (u32(e, 8), u32(e, 12))
    if fl is None:
        raise PackageError("no flash.bin entry")
    off, length = fl
    if off + length > len(image):
        raise PackageError("flash.bin runs past the package")
    if length < APP_END:
        raise PackageError("flash.bin doesn't reach 0x93000")
    if not re.fullmatch(r"FM-1_\d{3,15}", ident):  # package.cpp valid_identity()
        raise PackageError("identity isn't FM-1_ and 3-15 digits")
    app = bytes(image[off + APP_START:off + APP_END])
    if app.count(0xFF) == len(app) or app.count(0) == len(app):
        raise PackageError("app area is blank")
    return ident, app


def sector_key(digest):
    h = digest.hex()
    return "s/%s/%s" % (h[:2], h[2:])


def serialize(fields, sectors):
    out = "".join("%s %s\n" % (k, fields[k]) for k in sorted(fields) if fields[k] != "")
    return out + "".join("@%05x %s\n" % (addr, h.hex()) for addr, h in sectors)


class DirStore:
    """A plain directory laid out like the store (the Mac test's back end)."""

    def __init__(self, root):
        self.root = Path(root)

    def exists(self, key):
        return (self.root / key).is_file()

    def write(self, key, data):
        path = self.root / key
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def read(self, key):
        return (self.root / key).read_bytes()


def set_project(store, pkg_id, project):
    """Records the catalog project of a package stored before projects existed."""
    key = "m/pkg/" + pkg_id
    fields, sectors = {}, []
    for line in store.read(key).decode().splitlines():
        if line.startswith("@"):
            addr, h = line[1:].split(" ", 1)
            sectors.append((int(addr, 16), bytes.fromhex(h)))
        elif " " in line:
            k, v = line.split(" ", 1)
            fields[k] = v
    fields["project"] = project
    fields["family"] = "stock" if project == "stock" else "felucca"
    store.write(key, serialize(fields, sectors).encode())


def sideload_project(app_sha256_hex):
    """library.cpp sideload_project(): the project of a package no catalog lists."""
    return "sha:" + app_sha256_hex[:16]


def add_package(store, pkg_id, data, name="", version="", expected_sha256=None, pinned=False, project=""):
    """Stores a package's app sectors and its manifest. Returns the identity.

    project is the package's Firmware Hub catalog id; empty means sideloaded
    (library.cpp add_package())."""
    if not ID_RE.match(pkg_id):
        raise PackageError("bad library id %r" % pkg_id)
    file_sha = hashlib.sha256(data).hexdigest()
    if expected_sha256 and expected_sha256 != file_sha:
        raise PackageError("file hash doesn't match")
    ident, app = parse_package(data)
    if project and not ID_RE.match(project):
        raise PackageError("bad project %r" % project)
    app_hex = hashlib.sha256(app).hexdigest()
    project = project or sideload_project(app_hex)
    family = "stock" if project == "stock" else "felucca"
    sectors = []
    for i in range(0, len(app), SECTOR):
        s = app[i:i + SECTOR]
        h = hashlib.sha256(s).digest()
        if s != BLANK and not store.exists(sector_key(h)):
            store.write(sector_key(h), s)
        sectors.append((APP_START + i, h))
    clean = lambda s: s.replace("\n", " ").replace("\r", " ")
    fields = {"identity": ident, "project": project, "family": family, "file": file_sha,
              "app": app_hex, "name": clean(name), "version": clean(version),
              "pin": "1" if pinned else ""}
    store.write("m/pkg/" + pkg_id, serialize(fields, sectors).encode())
    return ident


def main():
    if len(sys.argv) < 4 or sys.argv[1] != "selftest":
        sys.exit(__doc__.strip().splitlines()[-1].strip())
    store = DirStore(sys.argv[2])
    for path in sys.argv[3:]:
        p = Path(path)
        ident = add_package(store, p.stem, p.read_bytes(), name=p.stem.split("-")[0], version="test")
        print("fm1lib: %s = %s" % (p.stem, ident))


if __name__ == "__main__":
    main()
