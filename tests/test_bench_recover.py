"""tools/bench_recover.py against the Unbricker's mock Transporter. Run by make test.

Needs the recovery project (its venv, the Unbricker, V15 and FM-A's
2026-10-25 dump); without it the test says so and passes.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("bench_recover", ROOT / "tools" / "bench_recover.py")
br = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(br)

FM_A_UNIT = "69bb070aaa87bd10"
FM1_1_DUMP = br.RECOVERY / "backups" / "fm1-transporter-dump-20261025-103326.bin"
SLOOP = br.RECOVERY / "firmware" / "sloop-2.8.1.fwsc"
failures = 0


def expect(cond, what):
    global failures
    if not cond:
        print("FAIL", what)
        failures += 1


def run(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = br.main(argv)
    return code, out.getvalue()


def main():
    expect(br.unit_id(b"\xff" * br.FLASH) == "", "a blank unit sector has no ID")
    if not (FM1_1_DUMP.exists() and SLOOP.exists() and (br.RECOVERY / ".venv").exists()):
        print("bench_recover: recovery project not found, mock runs skipped")
        return
    main = FM1_1_DUMP.read_bytes()
    expect(br.unit_id(main) == FM_A_UNIT, "FM-A's dump has FM-A's unit ID")
    with tempfile.TemporaryDirectory() as t:
        t = pathlib.Path(t)
        bench = bytearray(main)
        bench[0xE9000:0xE9010] = b"BENCHUNIT-TEST!!"
        (t / "bench.bin").write_bytes(bench)
        # A registry where FM-B is the doctored dump's unit, and FM-A the real one's.
        reg = t / "devices.json"
        reg.write_text(json.dumps({"devices": [{"label": "FM-A", "kind": "fm1", "unit": FM_A_UNIT},
                                               {"label": "FM-B", "kind": "fm1", "unit": br.unit_id(bytes(bench))}]}))
        common = ["--devices", str(reg)]

        code, text = run(["recover", str(SLOOP), "--write", "--mock", str(FM1_1_DUMP), "--out", str(t / "fm1-1"), *common])
        expect(code == 1 and "REFUSED" in text, "FM-A is refused when the run asked for FM-B")
        expect(not any(l.startswith("+") and " restore " in l for l in text.splitlines()), "no restore after the refusal")
        expect(len(list((t / "fm1-1").glob("*.bin"))) == 1, "only the read-only dump ran")

        code, text = run(["recover", str(SLOOP), "--mock", str(FM1_1_DUMP), "--out", str(t / "a"), "--fm1", "FM-A", *common])
        expect(code == 0, "FM-A is fine when the run asks for it (dry run)")

        code, text = run(["recover", str(SLOOP), "--mock", str(t / "bench.bin"), "--out", str(t / "dry"), *common])
        restore = [l for l in text.splitlines() if l.startswith("+") and " restore " in l]
        expect(code == 0 and len(restore) == 1 and "--write" not in restore[0], "recover without --write is a dry run")

        backups = br.RECOVERY / "backups"
        before = sorted(p.name for p in backups.iterdir())
        code, text = run(["recover", str(SLOOP), "--mock", str(t / "bench.bin"), *common])
        expect(sorted(p.name for p in backups.iterdir()) == before, "a mock run leaves the real backups alone")

        code, text = run(["recover", str(SLOOP), "--write", "--mock", str(t / "bench.bin"), "--out", str(t / "wet"), *common])
        expect(code == 0, "the bench FM-1 is recovered (mock)")
        sums = (t / "wet" / "SHA256SUMS").read_text().split()
        expect(len(sums) == 2 and sums[1].startswith("fm1-transporter-dump-"), "the dump's sha256 is recorded")


main()
if failures:
    print(f"bench_recover: {failures} failure(s)")
    sys.exit(1)
print("bench_recover: all passed")
