"""tools/bench_recover.py against the Unbricker's mock Transporter. Run by make test.

Needs the recovery project (its venv, the Unbricker, V15 and FM-1-1's
2026-10-25 dump); without it the test says so and passes.
"""
import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("bench_recover", ROOT / "tools" / "bench_recover.py")
br = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(br)

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
    expect(br.unit_id(main) == br.FM1_1_UNIT, "FM-1-1's dump has FM-1-1's unit ID")
    with tempfile.TemporaryDirectory() as t:
        t = pathlib.Path(t)
        bench = bytearray(main)
        bench[0xE9000:0xE9010] = b"BENCHUNIT-TEST!!"
        (t / "bench.bin").write_bytes(bench)

        code, text = run(["recover", str(SLOOP), "--write", "--mock", str(FM1_1_DUMP), "--out", str(t / "fm1-1")])
        expect(code == 1 and "REFUSED" in text, "FM-1-1 is refused")
        expect(not any(l.startswith("+") and " restore " in l for l in text.splitlines()), "no restore after the refusal")
        expect(len(list((t / "fm1-1").glob("*.bin"))) == 1, "only the read-only dump ran")

        code, text = run(["recover", str(SLOOP), "--mock", str(t / "bench.bin"), "--out", str(t / "dry")])
        restore = [l for l in text.splitlines() if l.startswith("+") and " restore " in l]
        expect(code == 0 and len(restore) == 1 and "--write" not in restore[0], "recover without --write is a dry run")

        backups = br.RECOVERY / "backups"
        before = sorted(p.name for p in backups.iterdir())
        code, text = run(["recover", str(SLOOP), "--mock", str(t / "bench.bin")])
        expect(sorted(p.name for p in backups.iterdir()) == before, "a mock run leaves the real backups alone")

        code, text = run(["recover", str(SLOOP), "--write", "--mock", str(t / "bench.bin"), "--out", str(t / "wet")])
        expect(code == 0, "the bench FM-1 is recovered (mock)")
        sums = (t / "wet" / "SHA256SUMS").read_text().split()
        expect(len(sums) == 2 and sums[1].startswith("fm1-transporter-dump-"), "the dump's sha256 is recorded")


main()
if failures:
    print(f"bench_recover: {failures} failure(s)")
    sys.exit(1)
print("bench_recover: all passed")
