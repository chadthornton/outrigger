"""Generate the golden vectors for lib/fm1_loader from the proven Python.

The reference is Quixotic7's MvaveFM1Unbricker (fm1_unbrick.py, pinned
707470a) driving kagaimiq's jl-uboot-tool classes (JL_UBOOT, JL_LoaderV2) over
the Unbricker's own CDB-level MockScsi (tests/test_unbrick.py). Nothing here
opens a real device.

What is the Unbricker's code, and what is composed here from it:
- identify: scsi_inquiry, upload_loader, the usb_buffer_size fallback of
  open_real_device, check_identity. Exactly the Unbricker's open sequence,
  except that the diagnostic retained-RAM read (FD07) is left out.
- backup: read_flash(0, 1 MiB) twice and compare, as take_backup does.
- install: planned from the stored backup before the key (nothing differs:
  no ROM boot at all), then one read_flash(0, 1 MiB) compared with that
  backup, then the writes. Checkpoint 3 found the FM-1 restarting itself when
  the loader had read for about 8.5 s without an erase.
- write: the Unbricker's write_and_verify erases and writes the whole app
  area and retries the whole area. The loader writes only the 4 KiB sectors
  that differ, so per sector this calls the Unbricker's own erase_range,
  write_range and read_flash and retries that sector once. Command layouts
  are the Unbricker's and jl-uboot-tool's; the sector loop is ours.
- reset: JL_LoaderV2.run_app(1) (0xFC0C), as jl-uboot-tool's `reset` does.
  The Unbricker never resets: the user power-cycles the FM-1.

Each scenario file lists every SCSI command the host sends, one per line:
  <CDB hex> <I|O|N><transfer length> <sha256[:16] of data-out or -> [!]
I = data in, O = data out, N = none; "!" marks a command the device refused.
It ends with "# flash <sha256 of the mock's flash>" and "# result <outcome>".

Usage: build/golden-venv/bin/python tools/gen_golden.py [out_dir]
Needs crcmod and pyyaml (make golden sets up the venv), the fixtures
(make fixtures) and the Unbricker checkout (FM1_UNBRICKER).
"""
import hashlib
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
UNBRICKER = Path(os.environ.get(
    "FM1_UNBRICKER",
    Path.home() / "Projects/TROUBLESHOOTING/FM1-firmware-recovery/tools/MvaveFM1Unbricker"))
FIXTURES = REPO / "tests" / "fixtures"
sys.path.insert(0, str(UNBRICKER))

import fm1_unbrick as fu  # noqa: E402
from tests.test_unbrick import MockScsi, _load_jl_or_skip  # noqa: E402

SECTOR = fu.SECTOR_SIZE
UNIT_SECTOR = fu.BTIF_START
RESET_ARG = 1


def sha(data):
    return hashlib.sha256(data).hexdigest()


def synth(seed, n):
    """Deterministic bytes, the way the Unbricker's synthetic_flash makes them."""
    out, counter = bytearray(), 0
    while len(out) < n:
        out += hashlib.sha256(seed + counter.to_bytes(4, "little")).digest()
        counter += 1
    return bytes(out[:n])


class GoldenScsi(MockScsi):
    """The Unbricker's MockScsi, plus the reset (FC0C) and a configurable
    inquiry product once the loader runs (unknown on real hardware)."""

    def __init__(self, jl, uboot, blob, loader_product="UBOOT1.00"):
        super().__init__(jl, uboot, blob)
        self.loader_product = loader_product
        self.was_reset = False
        self.log = []

    def execute(self, cdb, data_out, data_in):
        if data_out is not None:
            shape = "O%d %s" % (len(data_out), sha(bytes(data_out))[:16])
        elif data_in is not None:
            shape = "I%d -" % len(data_in)
        else:
            shape = "N0 -"
        line = "%s %s" % (bytes(cdb).hex(), shape)
        try:
            r = self._execute(cdb, data_out, data_in)
        except self.jl.SCSIException:
            self.log.append(line + " !")
            raise
        self.log.append(line)
        return r

    def _execute(self, cdb, data_out, data_in):
        if self.was_reset:
            raise self.jl.SCSIException("device left ROM boot")
        if cdb[0] == 0x12 and self.loader_running:
            r = super().execute(cdb, data_out, data_in)
            data_in[16:32] = self.loader_product.ljust(16).encode()
            return r
        if self.loader_running and int.from_bytes(cdb[0:2], "big") == 0xFC0C:
            self.was_reset = True
            self._resp(data_in, 0xFC0C)
            return 0
        return super().execute(cdb, data_out, data_in)


def identify(jl, scsi):
    """Returns (dev, None) or (None, reason)."""
    vendor, product, _ = fu.scsi_inquiry(scsi)
    if vendor.upper() != fu.DEVICE_VENDOR:
        return None, "not-wl82"
    if product == fu.DEVICE_PRODUCT:
        fu.upload_loader(jl, scsi, vendor.lower())
    else:
        try:
            jl.JL_LoaderV2(scsi).online_device()
        except Exception:
            return None, "no-loader"
    ldr = jl.JL_LoaderV2(scsi)
    try:
        io_max = int(ldr.usb_buffer_size())
    except Exception:
        io_max = 512
    if not 16 <= io_max <= 0xFFFF:
        io_max = 512
    dev = fu.Fm1Device("mock://golden", ldr, io_max=io_max)
    try:
        fu.check_identity(dev)
    except fu.SafetyError:
        ldr.run_app(RESET_ARG)
        return None, "identity"
    return dev, None


def write_sector(dev, addr, data):
    for _ in (1, 2):
        fu.erase_range(dev, addr, SECTOR)
        fu.write_range(dev, addr, data)
        if fu.read_flash(dev, addr, SECTOR, None) == data:
            return True
    return False


def write_app(dev, current_app, target_app):
    for off in range(0, fu.FW_LEN, SECTOR):
        want = target_app[off:off + SECTOR]
        if current_app[off:off + SECTOR] != want:
            if not write_sector(dev, fu.FW_START + off, want):
                return False
    return True


def install(jl, scsi, target_app, stored):
    if stored[fu.FW_START:fu.FW_END] == target_app:
        return "done"                                  # planned from the backup: nothing to write
    dev, why = identify(jl, scsi)
    if dev is None:
        return why
    for addr in range(0, fu.FLASH_SIZE, SECTOR):    # stops at the first sector that differs
        if fu.read_flash(dev, addr, SECTOR, None) != stored[addr:addr + SECTOR]:
            dev.ldr.run_app(RESET_ARG)
            return "backup-stale"
    if not write_app(dev, stored[fu.FW_START:fu.FW_END], target_app):
        return "write-failed"
    dev.ldr.run_app(RESET_ARG)
    return "done"


def backup(jl, scsi, target_app, stored):
    """Back up this FM-1 ("Save now"): identify, the double read, reset. Writes nothing."""
    dev, why = identify(jl, scsi)
    if dev is None:
        return why
    a = fu.read_flash(dev, 0, fu.FLASH_SIZE, None)
    b = fu.read_flash(dev, 0, fu.FLASH_SIZE, None)
    dev.ldr.run_app(RESET_ARG)
    return "done" if a == b else "backup-mismatch"


def resume(jl, scsi, target_app, stored):
    dev, why = identify(jl, scsi)
    if dev is None:
        return why
    fu.read_flash(dev, UNIT_SECTOR, SECTOR, None)     # which unit: its stored backup
    current = fu.read_flash(dev, 0, fu.FLASH_SIZE, None)[fu.FW_START:fu.FW_END]
    if not write_app(dev, current, target_app):
        return "write-failed"
    dev.ldr.run_app(RESET_ARG)
    return "done"


V15_HEAD = None  # stock V15's boot head, from its package: the only head the board installs on


def base_flash(app):
    flash = bytearray(fu.synthetic_flash())
    flash[:len(V15_HEAD)] = V15_HEAD
    flash[fu.FW_START:fu.FW_END] = app
    flash[UNIT_SECTOR:UNIT_SECTOR + SECTOR] = synth(b"unit-A", SECTOR)
    return flash


def scenarios(pkgs):
    sloop, skiff = pkgs["sloop-2.8.1.fwsc"].firmware, pkgs["skiff-1.4.fwsc"].firmware
    half = bytearray(sloop)
    half[:0x20000] = skiff[:0x20000]                   # 32 sectors already written
    half[0x20000:0x21000] = b"\xff" * SECTOR           # the 33rd was erased, then the host died
    first = next(o for o in range(0, fu.FW_LEN, SECTOR)
                 if sloop[o:o + SECTOR] != skiff[o:o + SECTOR])
    # (name, flash, MockUboot kwargs, entry, target, loader product)
    return [
        ("install", base_flash(sloop), {}, install, skiff, "UBOOT1.00"),
        ("noop", base_flash(skiff), {}, install, skiff, "UBOOT1.00"),
        ("retry_once", base_flash(sloop), {"flaky_write": 3}, install, skiff, "UBOOT1.00"),
        ("retry_fails", base_flash(sloop), {"bad_cell": fu.FW_START + first + 0x123},
         install, skiff, "UBOOT1.00"),
        ("wrong_key", base_flash(sloop), {"chip_key": 0x1234}, install, skiff, "UBOOT1.00"),
        ("wrong_flash_id", base_flash(sloop), {"flash_id": 0x856015}, install, skiff,
         "UBOOT1.00"),
        ("backup_mismatch", base_flash(sloop), {"flaky_read": 2048 + 7}, backup, None,
         "UBOOT1.00"),
        ("stale", base_flash(sloop), {"flaky_read": 7}, install, skiff, "UBOOT1.00"),
        ("resume", base_flash(bytes(half)), {}, resume, skiff, "UBOOT1.00"),
        ("backup", base_flash(skiff), {}, backup, None, "UBOOT1.00"),
    ]


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else REPO / "tests" / "golden")
    out.mkdir(parents=True, exist_ok=True)
    jl = _load_jl_or_skip()
    blob = (jl.dataroot / fu.WL82_LOADER_REL.split("data/", 1)[1]).read_bytes()
    assert sha(blob) == fu.WL82_LOADER_SHA256
    fu.Progress.update = lambda self, n: None          # keep the progress lines off stderr
    fu.Progress.close = lambda self: None

    pkgs = {}
    lines = []
    for name in ("FM-1_015.fwsc", "skiff-1.4.fwsc", "sloop-2.8.1.fwsc"):
        p = fu.Package.load(FIXTURES / name)
        pkgs[name] = p
        lines.append("%s identity=%s kind=%s file=%s fl_off=0x%x fl_len=0x%x app=%s head=%s"
                     % (name, p.identity, p.kind, p.file_sha256, p.fl_off, p.fl_len,
                        p.firmware_sha256, p.head_sha256))
    (out / "packages.txt").write_text("\n".join(lines) + "\n")
    global V15_HEAD
    V15_HEAD = bytes(pkgs["FM-1_015.fwsc"].flash[:0x4000])

    for name, flash, kw, entry, target, product in scenarios(pkgs):
        m = fu.MockUboot(image=bytes(flash), **kw)
        scsi = GoldenScsi(jl, m, blob, product)
        result = entry(jl, scsi, target, bytes(flash))  # the stored backup: the flash as it was
        text = "".join(line + "\n" for line in scsi.log)
        text += "# flash %s\n# result %s\n" % (sha(bytes(m.flash)), result)
        (out / ("%s.txt" % name)).write_text(text)
        print("golden: %-16s %5d commands, %s" % (name, len(scsi.log), result))

    # Cipher and CRC vectors from jl-uboot-tool's own code.
    from jltech.crc import jl_crc16
    from jltech.cipher import cipher_bytes, jl_crc_cipher, jl_enc_cipher
    vec = [
        "crc16 123456789 %04x" % jl_crc16(b"123456789"),
        "crc16 synth512 %04x" % jl_crc16(synth(b"crc", 512)),
        "mengli zero32 %s" % cipher_bytes(jl_crc_cipher, bytes(32)).hex(),
        "mengli synth64 %s" % cipher_bytes(jl_crc_cipher, synth(b"mengli", 64)).hex(),
        "jlenc zero64 %s" % cipher_bytes(jl_enc_cipher, bytes(64)).hex(),
        "chipkey 980f %s" % cipher_bytes(jl_crc_cipher, (0x980F).to_bytes(2, "little"))[::-1].hex(),
    ]
    # The USB-MIDI side of the Unbricker's softkey: the identity query, an FM-1's
    # answer to it, and the key.
    vec += [
        "hs_query %s" % fu.HS_QUERY.hex(),
        "softkey %s" % fu.SOFT_KEY.hex(),
    ]
    for ident in ("FM-1_714", "FM-1_015", "ota-FM-1_015"):
        vec.append("identity %s %s" % (ident, fu.identity_answer(ident).hex()))
    (out / "crypto.txt").write_text("\n".join(vec) + "\n")


if __name__ == "__main__":
    main()
