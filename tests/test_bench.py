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
    rec = bench.do_switch(b, "Skiff 2.1.1")
    expect(rec["ok"], "a clean switch passes")
    expect(rec["running"] == "Skiff 2.1.1", "the board says Skiff runs")
    expect(rec["before"]["running"] == "Stock V15", "what ran before is recorded")
    expect(rec["plan"] == ["Switches from Stock V15.", "Keeps your data."], "the plan's lines are recorded")
    expect(rec["after_backup"]["ok"], "the switch ends with a backup")
    expect(b.posts == ["/bench/select?index=2", "/bench/act?kind=switch&index=2", "/bench/act?kind=backup"],
           "select, hold, then back up: " + repr(b.posts))


def test_switch_refuses_rig():
    b = FakeBoard()
    b.mac, b.board = bench.RIG_MAC, "rig"
    expect(raises(lambda: bench.do_switch(b, "Skiff 2.1.1"), "rig board"), "the rig board is refused")
    expect(b.posts == [], "nothing posted to the rig")
    b = FakeBoard()
    b.board = "unknown"
    expect(raises(lambda: bench.do_switch(b, "Skiff 2.1.1"), "not a listed bench spare"), "an unlisted board")
    b = FakeBoard()
    b.units = [{"id": bench.RIG_UNIT + "x"}]
    expect(raises(lambda: bench.do_switch(b, "Skiff 2.1.1"), "Chad's own FM-1"), "the rig FM-1 is refused")
    expect(b.posts == [], "nothing posted with the rig FM-1 attached")


def test_switch_unknown_label():
    b = FakeBoard()
    expect(raises(lambda: bench.do_switch(b, "Melodee 1.6"), "isn't in the board's library"), "unknown firmware")
    expect(b.posts == [], "nothing posted")


def test_plan_blocked():
    b = FakeBoard()
    b.plan_blocked = True
    expect(raises(lambda: bench.do_switch(b, "Felucca 1.8.0"), "can't be held"), "a plan that can't be held")
    expect(b.posts == ["/bench/select?index=1"], "no hold after a blocked plan")


def test_install_stops():
    b = FakeBoard()
    b.install_stops = True
    rec = bench.do_switch(b, "Felucca 1.8.0")
    expect(not rec["ok"] and rec["unfinished"], "an install that stopped fails the run")
    expect("devices" not in rec, "no USB watch while the FM-1 sits in ROM boot")


def test_boot_loop():
    b = FakeBoard()
    b.boot_loops = True
    rec = bench.do_switch(b, "Felucca 1.8.0")
    expect(not rec["ok"] and rec["usb_changes"] > 0, "a re-enumerating FM-1 fails the run")


def test_main_dry_run():
    expect(bench.main(["switch", "Skiff 2.1.1", "--dry-run"]) == 0, "dry run passes on the fake board")
    expect(bench.main(["switch", "Nope", "--dry-run"]) == 1, "dry run fails an unknown firmware")


for t in [test_switch_ok, test_switch_refuses_rig, test_switch_unknown_label, test_plan_blocked,
          test_install_stops, test_boot_loop, test_main_dry_run]:
    t()
if failures:
    print(f"bench: {failures} failure(s)")
    sys.exit(1)
print("bench: all passed")
