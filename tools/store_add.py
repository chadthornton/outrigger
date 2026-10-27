"""Adds FM-1 packages to the Waveshare's store without losing what it holds.

    build/tools-venv/bin/python tools/store_add.py <partitions.csv> <port> [--pin] <pkg.fwsc>...

The board must be in Computer mode (its USB host off, so nothing else writes the
store meanwhile). Steps:
1. read the whole `store` partition with esptool and keep that image under
   build/store/ as a laptop copy of the backups and saves it holds;
2. mount it with littlefs-python (the geometry of tools/store_image.py);
3. add each package with tools/fm1lib.py (the same layout as lib/fm1_loader),
   named after the catalog entry when its SHA-256 is in thefirmwarehub.com's
   catalog, else after the file;
4. check that every file that was there before is still there, unchanged;
5. write the image back and let esptool verify it.
Nothing else in the store changes: backups, saves and units stay as they were.
"""
import datetime
import os
import hashlib
import json
import subprocess
import sys
import urllib.request
from pathlib import Path

import littlefs

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm1lib  # noqa: E402
from store_image import BLOCK, GEOMETRY, store_partition, superblock  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CATALOG = "https://thefirmwarehub.com/catalog.json"
# Laptop copies of the board's store (its backups and saves), outside build/ so
# `make clean` can't take them: the recovery project's backups folder.
COPIES = Path(os.environ.get("FM1_STORE_COPIES", Path.home() / "Projects/TROUBLESHOOTING/FM1-firmware-recovery/backups/store"))


class LfsStore:
    def __init__(self, fs):
        self.fs = fs

    def exists(self, key):
        try:
            self.fs.stat(key)
            return True
        except Exception:
            return False

    def write(self, key, data):
        parent = key.rsplit("/", 1)[0]
        self.fs.makedirs(parent, exist_ok=True)
        with self.fs.open(key, "wb") as f:
            f.write(data)

    def read(self, key):
        with self.fs.open(key, "rb") as f:
            return f.read()


def esptool():
    tools = Path.home() / "Library/Arduino15/packages/esp32/tools/esptool_py"
    found = sorted(tools.glob("*/esptool"))
    if not found:
        sys.exit("store_add: esptool not found under %s" % tools)
    return str(found[-1])


def catalog_names():
    """sha256 -> (id, name, version) from the Firmware Hub's catalog, or {} offline."""
    try:
        req = urllib.request.Request(CATALOG, headers={"User-Agent": "usb-midi-host-store-add/1"})
        with urllib.request.urlopen(req, timeout=20) as r:
            cat = json.load(r)
    except Exception as e:
        print("store_add: catalog unavailable (%s); naming packages after their files" % e)
        return {}
    out = {e["sha256"]: (e["id"], e["name"], e.get("version", "")) for e in cat["firmwares"]}
    s = cat["stock"]
    out[s["sha256"]] = ("stock", "M-VAVE stock", "V%d" % int(s["product"][5:]))
    return out


def snapshot(fs, path=""):
    """Every file in the store: path -> sha256."""
    out = {}
    for entry in fs.scandir(path or "/"):
        p = (path + "/" + entry.name).lstrip("/")
        if entry.type == 2:  # directory
            out.update(snapshot(fs, p))
        else:
            with fs.open(p, "rb") as f:
                out[p] = hashlib.sha256(f.read()).hexdigest()
    return out


def main():
    args = sys.argv[1:]
    pin = "--pin" in args  # mark the added packages as favorites
    read_only = "--list" in args  # read the store, list it, keep the copy, write nothing
    args = [a for a in args if a not in ("--pin", "--list")]
    if len(args) < 3 - read_only:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    csv_path, port, pkgs = args[0], args[1], [Path(p) for p in args[2:]]
    if not pkgs and not read_only:
        sys.exit("store_add: no packages named")
    offline = port.startswith("image:")  # image:<in.bin>:<out.bin>, for testing without a board
    offset, size = store_partition(csv_path)
    names = catalog_names()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    before = COPIES / ("store-from-board-%s.bin" % stamp)
    before.parent.mkdir(parents=True, exist_ok=True)
    if offline:
        before = Path(port.split(":")[1])
    else:
        subprocess.run([esptool(), "--chip", "esp32s3", "--port", port, "read-flash", hex(offset), hex(size),
                        str(before)], check=True)
    image = before.read_bytes()
    if not offline:
        with open(COPIES / "SHA256SUMS", "a") as sums:
            sums.write("%s  %s\n" % (hashlib.sha256(image).hexdigest(), before.name))
    if len(image) != size:
        sys.exit("store_add: read %d bytes, the store is %d" % (len(image), size))
    print("store_add: read the store (%d bytes), kept as %s" % (len(image), before))

    # Mount with the disk version the board's littlefs wrote, so writing back
    # never upgrades the store past what the board can read.
    sb = superblock(image)
    if not sb:
        sys.exit("store_add: no littlefs superblock in the store; refusing to touch it")
    version, block_size, block_count, name_max = sb
    if (block_size, block_count, name_max) != (BLOCK, size // BLOCK, GEOMETRY["name_max"]):
        sys.exit("store_add: store geometry %r isn't the expected one; refusing" % (sb,))
    print("store_add: littlefs disk version %d.%d" % (version >> 16, version & 0xFFFF))
    fs = littlefs.LittleFS(block_count=size // BLOCK, mount=False, **dict(GEOMETRY, disk_version=version))
    fs.context.buffer[:] = image
    fs.mount()
    was = snapshot(fs)
    if read_only:
        for path in sorted(was):
            print("  " + path if not path.startswith("s/") else "", end="" if path.startswith("s/") else "\n")
        print("store_add: %d files (%d sectors); read only, nothing written"
              % (len(was), len([k for k in was if k.startswith("s/")])))
        return
    store = LfsStore(fs)
    added = []
    for p in pkgs:
        data = p.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        pkg_id, name, version = names.get(sha, (p.stem, p.stem, ""))
        pkg_id = "%s-%s" % (pkg_id, version) if version and not pkg_id.endswith(version) else pkg_id
        ident = fm1lib.add_package(store, pkg_id, data, name=name, version=version, pinned=pin)
        added.append("%s (%s %s, %s)" % (pkg_id, name, version, ident))
    now = snapshot(fs)
    changed = [k for k, v in was.items() if now.get(k) != v and not k.startswith("m/pkg/")]
    if changed:
        sys.exit("store_add: refusing to write back: %d existing files changed (%s...)" % (len(changed), changed[0]))
    fs.unmount()
    after = Path(port.split(":")[2]) if offline else before.with_name("store-to-board-%s.bin" % stamp)
    after.write_bytes(bytes(fs.context.buffer))
    if not offline:
        subprocess.run([esptool(), "--chip", "esp32s3", "--port", port, "write-flash", hex(offset), str(after)],
                       check=True)
    for a in added:
        print("store_add: added " + a)
    print("store_add: %d files kept unchanged; the board's store now has %d packages"
          % (len(was), len([k for k in now if k.startswith("m/pkg/")])))


if __name__ == "__main__":
    main()
