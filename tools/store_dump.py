"""Rebuilds the FM-1 backups in a store image as flat 1 MiB flash dumps.

    build/tools-venv/bin/python tools/store_dump.py <store image> [out dir]

A store image comes from `make store-add LIST=1` (kept under the recovery
project's backups/store/). Each backup manifest m/backup/<unit>-b<n> lists 256
sector hashes; this writes <unit>-b<n>.bin next to the image (or in out dir), the
same 1 MiB layout as a Transporter or Unbricker dump, so either tool can restore
from it. Blank sectors are rebuilt as 0xFF; every other sector is checked
against its hash. Also prints each unit's saves.
"""
import hashlib
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_image import mount  # noqa: E402

SECTOR = 0x1000
BLANK = b"\xff" * SECTOR
BLANK_SHA = hashlib.sha256(BLANK).hexdigest()


def read(fs, path):
    with fs.open(path, "rb") as f:
        return f.read()


def manifest(text):
    fields, sectors = {}, []
    for line in text.splitlines():
        key, _, value = line.partition(" ")
        if key.startswith("@"):
            sectors.append((int(key[1:], 16), value))
        else:
            fields[key] = value
    return fields, sectors


def main():
    image_path = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else image_path.parent
    image = image_path.read_bytes()
    mounted = mount(image)
    if not mounted:
        sys.exit("store_dump: not a littlefs store image")
    fs = mounted[0]

    def sector(h):
        if h == BLANK_SHA:
            return BLANK
        data = read(fs, "s/%s/%s" % (h[:2], h[2:]))
        if hashlib.sha256(data).hexdigest() != h:
            sys.exit("store_dump: sector %s is damaged" % h)
        return data

    names = fs.listdir("m/backup") if "backup" in fs.listdir("m") else []
    for name in sorted(names):
        fields, sectors = manifest(read(fs, "m/backup/" + name).decode())
        if len(sectors) != 256:
            sys.exit("store_dump: %s lists %d sectors" % (name, len(sectors)))
        flash = b"".join(sector(h) for _, h in sectors)
        path = out / ("%s.bin" % name)
        path.write_bytes(flash)
        sha = lambda a, b: hashlib.sha256(flash[a:b]).hexdigest()[:8]
        print("store_dump: %s  sha256 %s  head %s  app %s  BT+voices %s" % (
            path, hashlib.sha256(flash).hexdigest(), sha(0, 0x4000), sha(0x4000, 0x93000), sha(0xE9000, 0xFC000)))
    saves = fs.listdir("m/save") if "save" in fs.listdir("m") else []
    for name in sorted(saves):
        fields, sectors = manifest(read(fs, "m/save/" + name).decode())
        print("store_dump: save %s  %s  %d sectors  %s" % (name, fields.get("identity"), len(sectors),
                                                            fields.get("label", "")))


if __name__ == "__main__":
    main()
