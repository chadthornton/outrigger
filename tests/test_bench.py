"""tools/bench.py against the fake bench board (tests/bench_fake.py). Run by make test."""
import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
from bench_fake import FakeBoard  # noqa: E402

_spec = importlib.util.spec_from_file_location("bench", ROOT / "tools" / "bench.py")
bench = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bench)
devices = bench.devices

# The example registry's boards, with both FM-1s' units known (FM-B's is the fake's).
REG = devices.load(ROOT / "tools/devices.example.json")
next(d for d in REG.entries if d["label"] == "FM-B")["unit"] = "0123456789abcdef"
FM_A = next(d for d in REG.entries if d["label"] == "FM-A")["unit"]


def switch(b, label, want="Small-V2", fm1="FM-B"):
    return bench.do_switch(b, label, want, fm1, REG)

failures = 0


def expect(cond, what):
    global failures
    if not cond:
        print("FAIL", what)
        failures += 1


def raises(fn, text):
    try:
        fn()
    except bench.BenchError as e:
        return text in str(e)
    return False


def test_switch_ok():
    b = FakeBoard()
    rec = switch(b, "Skiff 2.1.1")
    expect(rec["ok"], "a clean switch passes")
    expect(rec["running"] == "Skiff 2.1.1", "the board says Skiff runs")
    expect(rec["before"]["running"] == "M-VAVE stock V15", "what ran before is recorded")
    expect(rec["plan"] == ["Switches from M-VAVE stock V15.", "Keeps your data."], "the plan's lines are recorded")
    expect(rec["after_backup"]["ok"], "the switch ends with a backup")
    expect(b.posts == ["/bench/select?index=2", "/bench/act?kind=switch&index=2", "/bench/act?kind=backup"],
           "select, hold, then back up: " + repr(b.posts))


def test_switch_refuses_other_board():
    b = FakeBoard()
    b.mac, b.board = "02:00:00:00:00:01", "small-v1"
    expect(raises(lambda: switch(b, "Skiff 2.1.1"), "Small-V1"), "Small-V1 is refused when the run asked for Small-V2")
    expect(b.posts == [], "nothing posted to the wrong board")
    b = FakeBoard()
    b.mac = "02:00:00:00:00:99"
    expect(raises(lambda: switch(b, "Skiff 2.1.1"), "refusing"), "a MAC that isn't the name's")
    b = FakeBoard()
    b.board = "small-v1"
    expect(raises(lambda: switch(b, "Skiff 2.1.1"), "calls itself"), "a board whose own name disagrees")


def test_switch_checks_the_fm1():
    b = FakeBoard()
    b.last_unit = FM_A + "x"
    expect(raises(lambda: switch(b, "Skiff 2.1.1"), "FM-A"), "FM-A is refused when the run asked for FM-B")
    expect(b.posts == [], "nothing posted with the wrong FM-1 attached")
    b = FakeBoard()
    b.last_unit = FM_A
    expect(switch(b, "Skiff 2.1.1", fm1="FM-A")["ok"], "FM-A is fine when the run asks for it: nothing is precious")
    b = FakeBoard()
    b.units.append({"id": FM_A + "x"})  # old backups of FM-A, FM-B attached
    expect(switch(b, "Skiff 2.1.1")["ok"], "another FM-1's old backups don't block the run")


def test_refused_hold_fails_fast():
    b = FakeBoard()
    b.refuse_holds = True
    expect(raises(lambda: switch(b, "Skiff 2.1.1"), "refused the hold"), "a refused hold")
    expect(b.t < 60, "fails within a minute, not the job timeout")


def test_post_offline():
    class Gone(FakeBoard):
        def post(self, path):
            bench.Board("127.0.0.1:9", timeout=1).post(path)
    expect(raises(lambda: switch(Gone(), "Skiff 2.1.1"), "POST"), "an unreachable board")


def test_switch_unknown_label():
    b = FakeBoard()
    expect(raises(lambda: switch(b, "Melodee 1.6"), "isn't in the board's library"), "unknown firmware")
    expect(b.posts == [], "nothing posted")


def test_plan_blocked():
    b = FakeBoard()
    b.plan_blocked = True
    expect(raises(lambda: switch(b, "Felucca 1.8.0"), "can't be held"), "a plan that can't be held")
    expect(b.posts == ["/bench/select?index=1"], "no hold after a blocked plan")


def test_install_stops():
    b = FakeBoard()
    b.install_stops = True
    rec = switch(b, "Felucca 1.8.0")
    expect(not rec["ok"] and rec["unfinished"], "an install that stopped fails the run")
    expect("devices" not in rec, "no USB watch while the FM-1 sits in ROM boot")


def test_boot_loop():
    b = FakeBoard()
    b.boot_loops = True
    rec = switch(b, "Felucca 1.8.0")
    expect(not rec["ok"] and rec["usb_changes"] > 0, "a re-enumerating FM-1 fails the run")


def test_main_dry_run():
    expect(bench.main(["switch", "Skiff 2.1.1", "--dry-run"], reg=REG) == 0, "dry run passes on the fake board")
    expect(bench.main(["switch", "Nope", "--dry-run"], reg=REG) == 1, "dry run fails an unknown firmware")


for t in [test_switch_ok, test_switch_refuses_other_board, test_switch_checks_the_fm1, test_switch_unknown_label, test_plan_blocked,
          test_install_stops, test_boot_loop, test_main_dry_run, test_refused_hold_fails_fast, test_post_offline]:
    t()
if failures:
    print(f"bench: {failures} failure(s)")
    sys.exit(1)
print("bench: all passed")
