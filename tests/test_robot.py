"""tools/robot.py: scenario expansion, the checks on made-up logs (two robots,
splits, merges, clocks, unplugs), and whole runs against build/robot_sim, with
faults injected to show each check can fail. Run by make test (after
build/robot_sim)."""
import contextlib
import importlib.util
import io
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("robot", ROOT / "tools" / "robot.py")
robot = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(robot)
SCENARIOS = ROOT / "tools" / "robot_scenarios"

failures = 0


def expect(cond, what):
    global failures
    if not cond:
        print(f"FAIL {what}")
        failures += 1


def pkt(cable, *msg):
    b = bytes(msg) + bytes(3 - len(msg))
    return bytes([cable << 4 | (msg[0] >> 4 if msg[0] < 0xF0 else 0xF)]) + b


def robot_data(records, offset_us=0, rtt=200, status=None):
    """One robot's run data, its clock offset_us ahead of the Mac's."""
    s = sum(1 for r in records if r[0] == "s")
    rv = sum(1 for r in records if r[0] == "r")
    st = {"sent": s, "recv": rv, "log_lost_tx": 0, "log_lost_rx": 0, "not_sent": 0, "unplugs": 0}
    st.update(status or {})
    t_end = max([r[1] for r in records] + [0]) + 1_000_000
    return {"records": sorted(records, key=lambda r: r[1]), "status": st,
            "sync": [(0 - offset_us, 0, rtt), (t_end - offset_us, t_end, rtt)]}


def notes_run(n=50, every=10_000, lat=1000, src="a", dst="a", src_cable=0, dst_cable=1, offset=0,
              drop=(), extra=(), late=None):
    """Note on/off pairs from src's cable to dst's, lat µs later (in Mac time)."""
    recs = {src: [], dst: []} if src != dst else {src: []}
    for i in range(n):
        for j, msg in enumerate(((0x90, 60 + i % 5, 100), (0x80, 60 + i % 5, 0))):
            t = 1_000_000 + i * every + j * every // 2
            k = 2 * i + j
            recs[src].append(("s", t, pkt(src_cable, *msg)))
            if k in drop:
                continue
            d = lat + (late if late is not None and k == n else 0)
            t_dst = t + d + (offset if src != dst else 0)
            recs[dst].append(("r", t_dst, pkt(dst_cable, *msg)))
            if k in extra:
                recs[dst].append(("r", t_dst + 1, pkt(dst_cable, *msg)))
    return recs


ONE = {"name": "t", "robots": ["a"], "routes": [{"from": {"robot": "a", "cable": 0}, "to": {"robot": "a", "cable": 1}}],
       "checks": {"loss_max": 0, "extra_max": 0, "latency_ms_max": 5, "jitter_ms_max": 1}}


def run_check(scn, recs, offsets=None):
    offsets = offsets or {}
    return robot.check(scn, {n: robot_data(r, offsets.get(n, 0)) for n, r in recs.items()})


def failed(res):
    return sorted(k for k, c in res["checks"].items() if not c["ok"])


def test_expand():
    scn = {"robots": ["a"], "streams": [
        {"robot": "a", "cable": 1, "start_ms": 5, "notes": {"count": 2, "every_ms": 10, "channel": 2, "notes": [60, 64], "gate_ms": 3}},
        {"robot": "a", "clock": {"bpm": 120, "ticks": 4}},
        {"robot": "a", "unplug": {"every_ms": 100, "down_ms": 20, "count": 2}}]}
    plan = robot.expand(scn)["a"]
    lines = [l for _, l in plan["at"]]
    expect(lines[:2] == ["at 0 0 FA", "at 0 unplug 20"], f"start and unplug first: {lines[:2]}")
    expect("at 5000 1 913C64" in lines and "at 8000 1 813C00" in lines and "at 15000 1 914064" in lines,
           f"notes on cable 1, channel 2, gate 3 ms: {lines}")
    expect(plan["rep"] == ["rep 0 20833333 4 0 F8"], f"120 BPM clock: {plan['rep']}")
    expect("at 83334 0 FC" in lines, f"stop after the last tick: {lines}")
    expect([t for t, _ in plan["at"]] == sorted(t for t, _ in plan["at"]), "sorted")
    expect(robot.scenario_end_us(robot.expand(scn)) == 100_000, "end is the last unplug")


def test_clean_run_passes():
    res = run_check(ONE, notes_run())
    expect(res["ok"], f"clean run passes: {failed(res)}")
    expect(res["numbers"]["matched"] == 100 and res["numbers"]["latency_ms_max"] == 1.0, f"numbers {res['numbers']}")
    expect(res["numbers"]["latency_basis_us"] == 0, "one robot: exact latency")


def test_each_fault_fails_its_check():
    expect(failed(run_check(ONE, notes_run(drop={7})))[:1] == ["loss"], "a drop is loss")
    expect(run_check(ONE, notes_run(drop={7}))["checks"]["loss"]["value"] == 1, "exactly one lost")
    expect("stuck_notes" in failed(run_check(ONE, notes_run(drop={7}))), "a lost note-off is a stuck note")
    res = run_check(ONE, notes_run(extra={9}))
    expect(failed(res) == ["extra"] and res["checks"]["extra"]["value"] == 1, f"a duplicate is one extra: {failed(res)}")
    expect(failed(run_check(ONE, notes_run(lat=6000))) == ["latency"], "late is latency")
    res = run_check(ONE, notes_run(late=1800))
    expect(failed(res) == ["jitter"] or res["numbers"]["jitter_ms"] >= 0, "one late message shows in latency_ms_max")
    expect(res["numbers"]["latency_ms_max"] == 2.8, f"one 1.8 ms late: {res['numbers']['latency_ms_max']}")
    # B sent 1 ms after A arrives before it.
    a_, b_ = pkt(0, 0xB0, 1, 10), pkt(0, 0xB0, 2, 20)  # sent on cable 0
    recs = {"a": [("s", 1_000_000, a_), ("s", 1_001_000, b_),
                  ("r", 1_001_500, pkt(1, 0xB0, 2, 20)), ("r", 1_001_600, pkt(1, 0xB0, 1, 10))]}
    expect("order" in failed(run_check(ONE, recs)) and "loss" not in failed(run_check(ONE, recs)), f"swapped arrivals are order: {failed(run_check(ONE, recs))}")
    stray = notes_run()
    stray["a"].append(("r", 1_500_000, pkt(0, 0xB0, 7, 100)))
    expect(failed(run_check(ONE, stray)) == ["extra"], "an arrival on an unrouted cable is extra")


def test_accounting():
    recs = notes_run()
    d = {"a": robot_data(recs["a"], status={"sent": 101})}
    expect(failed(robot.check(ONE, d)) == ["accounted"], "a sent count the log doesn't hold")
    d = {"a": robot_data(recs["a"], status={"log_lost_rx": 3})}
    expect(failed(robot.check(ONE, d)) == ["accounted"], "a log overflow")


def test_two_robots_with_clock_offset_and_drift():
    two = {"robots": ["a", "b"], "routes": [{"from": {"robot": "a", "cable": 0}, "to": {"robot": "b", "cable": 0}}],
           "checks": {"latency_ms_max": 5, "jitter_ms_max": 0.2}}
    # b's clock is 7 s ahead of a's; latency 1.5 ms in Mac time.
    recs = notes_run(src="a", dst="b", dst_cable=0, lat=1500, offset=7_000_000)
    res = run_check(two, recs, offsets={"b": 7_000_000})
    expect(res["ok"], f"offset clocks: {failed(res)} {res['numbers']}")
    expect(abs(res["numbers"]["latency_ms_p50"] - 1.5) < 0.01, f"latency through the offset: {res['numbers']}")
    expect(res["numbers"]["latency_basis_us"] == 100, "basis is half the time request's round trip")
    # b runs 50 ppm fast: the end sync carries it.
    recs = notes_run(src="a", dst="b", dst_cable=0, lat=1500)
    recs["b"] = [(k, int(t * 1.00005), p) for k, t, p in recs["b"]]
    db = robot_data(recs["b"])
    (m0, r0, rtt), (m1, r1, _) = db["sync"]
    db["sync"] = [(m0, int(r0 * 1.00005), rtt), (m1, int(r1 * 1.00005), rtt)]
    res = robot.check(two, {"a": robot_data(recs["a"]), "b": db})
    expect(abs(res["numbers"]["latency_ms_max"] - 1.5) < 0.01, f"drift corrected: {res['numbers']}")


def test_merge_and_split():
    merge = {"robots": ["a", "b"], "routes": [
        {"from": {"robot": "a", "cable": 0}, "to": {"robot": "b", "cable": 1}},
        {"from": {"robot": "b", "cable": 0}, "to": {"robot": "b", "cable": 1}}], "checks": {}}
    ra = notes_run(src="a", dst="b")
    rb = notes_run(src="b", dst="b", every=7000)
    recs = {"a": ra["a"], "b": ra["b"] + rb["b"]}
    res = run_check(merge, recs)
    expect(res["ok"] and res["numbers"]["matched"] == 200, f"merge: {failed(res)} {res['numbers']}")
    recs["b"] = [x for x in recs["b"] if not (x[0] == "r" and x[1] == ra["b"][5][1])]
    expect(failed(run_check(merge, recs)) == ["loss", "stuck_notes"] or "loss" in failed(run_check(merge, recs)),
           "a loss in one source of a merge")

    split = json.loads((SCENARIOS / "split.json").read_text())
    sent, got = [], []
    t = 1_000_000
    for i, note in enumerate([40, 59, 60, 72]):
        sent.append(("s", t + i * 10_000, pkt(0, 0x90, note, 100)))
        if note < 60:
            got.append(("r", t + i * 10_000 + 900, pkt(0, 0x91, note, 100)))
        else:
            got.append(("r", t + i * 10_000 + 900, pkt(1, 0x92, note + 12, 100)))
    sent.append(("s", t + 50_000, pkt(0, 0xB0, 1, 64)))
    got += [("r", t + 50_900, pkt(0, 0xB1, 1, 64)), ("r", t + 50_900, pkt(1, 0xB2, 1, 64))]
    split["checks"]["stuck_notes_max"] = 4  # note-ons only here
    res = run_check(split, {"a": sent, "b": got})
    expect(res["ok"] and res["numbers"]["matched"] == 6, f"split: {failed(res)} {res['numbers']}")
    bad = [g for g in got] + [("r", t + 2_900, pkt(1, 0x92, 52, 100))]  # the lower half on the upper synth
    expect("extra" in failed(run_check(split, {"a": sent, "b": bad})), "a note on the wrong side of the split")
    wrong_ch = [("r", x[1], pkt(0, 0x93, x[2][2], 100)) if x[2][0] >> 4 == 0 and x[2][1] == 0x91 else x for x in got]
    expect({"loss", "extra"} <= set(failed(run_check(split, {"a": sent, "b": wrong_ch}))), "the wrong channel map")


def test_clock_listen():
    scn = json.loads((SCENARIOS / "clock-out.json").read_text())
    interval = 60e6 / (120 * 24)
    ta = [("r", int(1_000_000 + i * interval), pkt(0, 0xF8)) for i in range(3000)]
    tb = [("r", int(5_000_000 + i * interval + (300 if i == 1500 else 0)), pkt(0, 0xF8)) for i in range(3000)]
    res = run_check(scn, {"a": ta, "b": tb})
    expect(res["ok"], f"a steady clock: {failed(res)} {res['numbers']}")
    expect(res["numbers"]["a0_clock_bpm"] == 120.0, f"bpm {res['numbers']}")
    expect(abs(res["numbers"]["clock_jitter_ms"] - 0.3) < 0.01, "one tick 0.3 ms late")
    tb[1500] = ("r", tb[1500][1] + 1500, tb[1500][2])
    expect(failed(run_check(scn, {"a": ta, "b": tb})) == ["clock_jitter"], "a tick 1.8 ms late")
    expect("clock_ticks_b0" in failed(run_check(scn, {"a": ta, "b": tb[:100]})), "too few ticks")


def test_unplug_excuses_loss_nearby_only():
    recs = notes_run(n=100, drop={40, 41, 42, 150})
    # a unplugs at 1.2 s for 30 ms; the host mounts it again 250 ms later.
    recs["a"] += [("u", 1_195_000, bytes(4)), ("p", 1_225_000, bytes(4)), ("M", 1_475_000, bytes(4))]
    res = run_check(ONE, recs)
    expect(res["numbers"]["lost_unplugged"] == 3, f"three lost in the outage: {res['numbers']}")
    expect(res["checks"]["loss"]["value"] == 1, "the one far from it is loss")
    expect(res["numbers"]["reconnect_ms_max"] == 250.0, "reconnect time is plug to mount")


# --- whole runs against build/robot_sim ---------------------------------------

def sim_run(name_or_scn, *opts):
    scn = name_or_scn if isinstance(name_or_scn, dict) else json.loads((SCENARIOS / name_or_scn).read_text())
    rb = robot.Robot("a", robot.SimLink(list(opts)))
    try:
        data = robot.run(scn, {"a": rb}, log=lambda *_: None)
    finally:
        rb.link.close()
    return robot.check(scn, data), data


def test_sim_runs():
    if not robot.SIM.exists():
        expect(False, f"{robot.SIM} missing: make build/robot_sim")
        return
    res, data = sim_run("loopback.json", "--jitter-us", "200")
    expect(res["ok"], f"loopback passes: {failed(res)}")
    expect(res["numbers"]["matched"] == 1194, f"every message: {res['numbers']['matched']}")
    expect(data["a"]["id"]["mac"] == "02:00:00:00:00:01", "id parsed")
    for opt, val, want in (("--drop-every", "50", "loss"), ("--dup-every", "100", "extra"),
                           ("--swap-every", "97", "order"), ("--latency-us", "7000", "latency"),
                           ("--jitter-us", "3000", "jitter")):
        res, _ = sim_run("loopback.json", opt, val)
        expect(want in failed(res), f"sim {opt} {val} fails {want}: {failed(res)}")
    res, _ = sim_run("hotplug.json", "--reenum-ms", "250")
    expect(res["ok"] and res["numbers"]["reconnects"] == 4 and res["numbers"]["reconnect_ms_max"] == 250.0,
           f"hotplug: {failed(res)} {res['numbers']}")
    # More events than the robot's schedule holds (2048): run() streams them in.
    big = {"name": "big", "robots": ["a"], "streams": [
        {"robot": "a", "cable": 0, "notes": {"count": 3000, "every_ms": 2, "notes": [60, 61, 62, 63, 64, 65, 66, 67]}}],
        "routes": ONE["routes"], "checks": {"loss_max": 0}}
    res, data = sim_run(big)
    expect(res["ok"] and res["numbers"]["matched"] == 6000, f"streamed schedule: {failed(res)} {res['numbers']}")
    # main() prints its PASS/FAIL report; keep the deliberate failures out of make test's output.
    with tempfile.TemporaryDirectory() as d, contextlib.redirect_stdout(io.StringIO()), \
            contextlib.redirect_stderr(io.StringIO()):
        runs = pathlib.Path(d) / "runs.jsonl"
        code = robot.main(["run", str(SCENARIOS / "loopback.json"), "--sim", "--runs", str(runs),
                           "--save", str(pathlib.Path(d) / "run.json")])
        expect(code == 0 and json.loads(runs.read_text())["ok"], "main run writes a passing runs line")
        expect(robot.main(["check", str(pathlib.Path(d) / "run.json")]) == 0, "a saved run re-checks")
        code = robot.main(["run", str(SCENARIOS / "loopback.json"), "--sim", "--sim-opt=--drop-every=20",
                           "--runs", str(runs)])
        expect(code == 1, "a failing run exits 1")
        expect(robot.main(["run", str(SCENARIOS / "merge.json"), "--sim"]) == 2, "--sim refuses two robots")


def test_scenarios_parse():
    for f in sorted(SCENARIOS.glob("*.json")):
        scn = json.loads(f.read_text())
        plan = robot.expand(scn)
        expect(set(plan) == set(scn["robots"]), f"{f.name}: robots")
        for rt in scn.get("routes", []):
            expect(rt["from"]["robot"] in scn["robots"] and rt["to"]["robot"] in scn["robots"], f"{f.name}: route robots")


def main():
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if failures:
        print(f"test_robot: {failures} failure(s)")
        sys.exit(1)
    print("test_robot: all passed")


if __name__ == "__main__":
    main()
