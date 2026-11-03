#!/usr/bin/env python3
"""The bench FM-1 through the Transporter (XIAO RP2040 on its USB D+/D-/GND),
scripted: Quixotic7's fm1_transporter_recover.py, run without its prompts.

usage:
  bench_recover.py dump [--out DIR]          full 1 MiB dump (read twice, compared), sha256 into SHA256SUMS
  bench_recover.py recover PKG.fwsc [--write] dump, check the unit, write PKG's app sectors (dry run without --write)
  [--port /dev/cu.usbmodemX] [--mock DUMP] [--fm1 FM-B] [--devices PATH]

Every command dumps first and refuses the FM-1 unless its unit ID (the loader's:
sha256 of sector 0xE9000, first 8 bytes) is the one --fm1 names in the device
registry (tools/devices.py): a check that the run has the FM-1 it asked for. --mock runs the
Unbricker's in-process mock Transporter seeded from DUMP and never opens a
serial port. --port names the Transporter so a board on the Mac is never
probed. Only the Unbricker writes, and only 0x4000-0x92FFF (the app).
"""
import argparse
import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import devices  # noqa: E402

RECOVERY = pathlib.Path(os.environ.get("FM1_RECOVERY", pathlib.Path.home() / "Projects/TROUBLESHOOTING/FM1-firmware-recovery"))
UNIT_SECTOR = 0xE9000
FLASH = 0x100000


class RecoverError(Exception):
    pass


def unit_id(dump):
    s = dump[UNIT_SECTOR:UNIT_SECTOR + 0x1000]
    if s in (b"\xff" * 0x1000, b"\x00" * 0x1000):
        return ""
    return hashlib.sha256(s).hexdigest()[:16]


def unbricker(args, recovery=RECOVERY):
    tool = recovery / "tools" / "MvaveFM1Unbricker"
    py = recovery / ".venv" / "bin" / "python"
    cmd = [str(py), "fm1_transporter_recover.py", *args]
    print("+", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=tool)
    if r.returncode != 0:
        raise RecoverError(f"fm1_transporter_recover.py {args[0]} exited {r.returncode}")


def common(a):
    out = []
    if a.port:
        out += ["--port", a.port]
    if a.mock:
        out += ["--dry-run", "--mock-flash", str(pathlib.Path(a.mock).resolve())]
    return out


def dump(a, out_dir):
    out_dir = pathlib.Path(out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    before = set(out_dir.glob("fm1-transporter-dump-*.bin"))
    started = time.time()
    unbricker(["dump", str(out_dir), "--v15", str(a.recovery / "firmware" / "FM-1.fwsc"), *common(a)], a.recovery)
    new = [p for p in out_dir.glob("fm1-transporter-dump-*.bin") if p not in before and p.stat().st_mtime >= started - 1]
    if len(new) != 1:
        raise RecoverError(f"expected one new dump in {out_dir}, found {len(new)}")
    path = new[0]
    data = path.read_bytes()
    if len(data) != FLASH:
        raise RecoverError(f"{path.name} is {len(data)} bytes, not 1 MiB")
    digest = hashlib.sha256(data).hexdigest()
    with open(out_dir / "SHA256SUMS", "a") as f:
        f.write(f"{digest}  {path.name}\n")
    uid = unit_id(data)
    print(f"dump {path} sha256 {digest} unit {uid or '(blank)'}")
    try:
        a.registry.expect_fm1(a.fm1, uid)
    except devices.DeviceError as e:
        raise RecoverError(str(e)) from None
    return path, uid


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["dump", "recover"])
    ap.add_argument("package", nargs="?")
    ap.add_argument("--out", help="dump folder (default: the recovery project's backups/)")
    ap.add_argument("--write", action="store_true", help="recover: actually write (default: dry run)")
    ap.add_argument("--port")
    ap.add_argument("--mock", help="a 1 MiB dump to seed the Unbricker's mock Transporter; no hardware")
    ap.add_argument("--recovery", type=pathlib.Path, default=RECOVERY)
    ap.add_argument("--fm1", default="FM-B", help="the FM-1 the run expects, by its registry label")
    ap.add_argument("--devices", help="the device registry (default: tools/devices.py's)")
    a = ap.parse_args(argv)
    try:
        a.registry = devices.load(a.devices)
    except devices.DeviceError as e:
        print("FAILED:", e)
        return 1
    try:
        # A mock's dumps are fake: never next to the real ones.
        out = a.out or (tempfile.mkdtemp(prefix="bench-recover-mock-") if a.mock else a.recovery / "backups")
        path, _ = dump(a, out)
        if a.command == "recover":
            if not a.package:
                ap.error("recover needs a .fwsc package")
            args = ["restore", str(pathlib.Path(a.package).resolve()), "--ref", str(path), *common(a)]
            if a.write:
                args += ["--write", "--yes"]
            unbricker(args, a.recovery)
    except RecoverError as e:
        print("REFUSED:" if "refusing" in str(e) else "FAILED:", e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
