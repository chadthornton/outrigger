"""tools/bench.py against the fake bench board (tests/bench_fake.py). Run by make test."""
import importlib.util
import pathlib
import sys
import tempfile

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
    expect(b.posts == ["/bench/act?kind=backup", "/bench/select?index=2", "/bench/act?kind=switch&index=2",
                       "/bench/act?kind=backup"], "back up, select, hold, then back up: " + repr(b.posts))
    expect(rec["before"]["unit"] == b.last_unit, "the unit is read by the backup before the switch")
    expect(rec["checks"] == {"result_ok": True, "fm1_back": True, "fm1_steady": True, "backup_ok": True,
                             "running": True}, "every check is read from the board: " + repr(rec["checks"]))


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
    expect(b.posts == ["/bench/act?kind=backup"], "only the identifying backup posted with the wrong FM-1 attached")
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
    expect(b.posts == ["/bench/act?kind=backup"], "nothing posted but the identifying backup")


def test_plan_blocked():
    b = FakeBoard()
    b.plan_blocked = True
    expect(raises(lambda: switch(b, "Felucca 1.8.0"), "can't be held"), "a plan that can't be held")
    expect(b.posts == ["/bench/act?kind=backup", "/bench/select?index=1"], "no hold after a blocked plan")


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
    expect(rec["checks"]["fm1_back"] and not rec["checks"]["fm1_steady"], "it came back but didn't stay")


def test_stale_unit_is_reread():
    class Swapped(FakeBoard):
        """The store last saw FM-B; FM-A is on the hub now, and a backup reveals it."""
        def _advance(self):
            if self.job and self.job[0] == "backup" and self.t >= self.job[2]:
                self.last_unit = FM_A
            super()._advance()
    b = Swapped()
    expect(raises(lambda: switch(b, "Skiff 2.1.1"), "FM-A"), "a swapped-in FM-A is caught by the backup first")
    expect(not any("switch" in p for p in b.posts), "no hold posted")


def test_wrong_firmware_running():
    class Stuck(FakeBoard):
        def _advance(self):
            before = self.running
            super()._advance()
            self.running = before  # the install "worked" but the old app is still there
    rec = switch(Stuck(), "Felucca 1.8.0")
    expect(not rec["ok"] and not rec["checks"]["running"] and rec["checks"]["result_ok"],
           "a result page that says ok isn't enough: the backed-up app must be the new one")


def test_fm1_report():
    b = FakeBoard()
    r = bench.fm1_report(b, "Small-V2", REG)
    expect(r["running"] == "M-VAVE stock V15" and r["fm1_on_usb"] and r["fm1"] == "FM-B",
           "the report reads the running firmware, the FM-1 on USB and which FM-1 it is: " + repr(r))
    expect(b.posts == [], "the report posts nothing")
    expect(bench.main(["fm1", "--dry-run"], reg=REG) == 0, "fm1 on the fake board")


def test_lock():
    with tempfile.TemporaryDirectory() as d:
        path = pathlib.Path(d) / "lock"
        held = bench.bench_lock(path)
        try:
            raised = False
            try:
                bench.bench_lock(path)
            except bench.BenchError as e:
                raised = "another bench run" in str(e)
            expect(raised, "a second driver is refused while the first holds the lock")
        finally:
            held.close()
        bench.bench_lock(path).close()
    expect(not str(getattr(bench, "LOCK_DIR", ROOT)).startswith(str(ROOT)), "the default locks are outside the worktree, shared by all")


def test_controller_on_the_hub():
    b = FakeBoard()
    b.controller = "late"
    rec = switch(b, "Felucca 1.8.0")
    expect(rec["ok"] and rec["checks"]["fm1_steady"], "a controller enumerating late isn't the FM-1 dropping: " + repr(rec["checks"]))
    expect(all(d[2] != "Launchkey Mini MK4 37" for d in rec["devices"]), "only the FM-1 is watched: " + repr(rec["devices"]))
    b = FakeBoard()
    b.controller = "always"
    b.boot_loops = True
    rec = switch(b, "Felucca 1.8.0")
    expect(not rec["checks"]["fm1_steady"], "a re-enumerating FM-1 still fails beside a steady controller")


def test_controller_alone_isnt_the_fm1():
    b = FakeBoard()
    b.controller = "always"
    b.fm1_gone = True
    expect(raises(lambda: bench.watch_fm1(b), "the FM-1 on USB"), "a Launchkey alone doesn't count as the FM-1 coming back")


def test_lock_per_board():
    with tempfile.TemporaryDirectory() as d:
        a = bench.bench_lock(bench.lock_path(pathlib.Path(d), "Small-V2"))
        try:
            bench.bench_lock(bench.lock_path(pathlib.Path(d), "Big-S3")).close()  # another board runs alongside
            raised = False
            try:
                bench.bench_lock(bench.lock_path(pathlib.Path(d), "small-v2"))
            except bench.BenchError:
                raised = True
            expect(raised, "the same board (any spelling) is refused while it's held")
        finally:
            a.close()


def test_main_dry_run():
    expect(bench.main(["switch", "Skiff 2.1.1", "--dry-run"], reg=REG) == 0, "dry run passes on the fake board")
    expect(bench.main(["switch", "Nope", "--dry-run"], reg=REG) == 1, "dry run fails an unknown firmware")


for t in [test_switch_ok, test_switch_refuses_other_board, test_switch_checks_the_fm1, test_switch_unknown_label, test_plan_blocked,
          test_install_stops, test_boot_loop, test_main_dry_run, test_refused_hold_fails_fast, test_post_offline,
          test_stale_unit_is_reread, test_wrong_firmware_running, test_fm1_report, test_lock,
          test_controller_on_the_hub, test_controller_alone_isnt_the_fm1, test_lock_per_board]:
    t()
if failures:
    print(f"bench: {failures} failure(s)")
    sys.exit(1)
print("bench: all passed")
