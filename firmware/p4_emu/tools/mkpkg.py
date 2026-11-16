#!/usr/bin/env python3
"""Make the fwpkg partition image for fm1-p4-emu from an FM-1 package.

    python3 -I firmware/p4_emu/tools/mkpkg.py PKG.fwsc OUT.bin [NAME]

OUT.bin is a 64-byte header (b"FM1PKG\\0\\x01", the package's length as u32 LE, NAME up to 51
bytes, NUL-padded) and the package. Write it at the fwpkg partition's offset, 0x410000. Keep
OUT.bin out of the repo: the packages are their authors' (SHRUB is closed).
"""
import struct
import sys
from pathlib import Path

PART_SIZE = 0x200000

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    name = (sys.argv[3] if len(sys.argv) > 3 else src.stem).encode()[:51]
    pkg = src.read_bytes()
    img = b"FM1PKG\0\x01" + struct.pack("<I", len(pkg)) + name.ljust(52, b"\0") + pkg
    if len(img) > PART_SIZE:
        sys.exit("%s: %d bytes, more than the 2 MiB partition" % (src, len(pkg)))
    out.write_bytes(img)
    print("%s: %s, %d bytes of package" % (out, name.decode(), len(pkg)))

if __name__ == "__main__":
    main()
