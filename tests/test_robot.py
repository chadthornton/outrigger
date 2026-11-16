"""tools/robot.py: scenario expansion (lazy, and equal to night 0's), the checks
on made-up logs (two robots, splits, merges, clocks, unplugs), each held to
night 0's checker (tests/robot_check_ref.py), and whole runs against
build/robot_sim, one and two robots, with faults injected to show each check
can fail and the windowed checker's memory staying flat. Run by make test
(after build/robot_sim)."""
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
_rspec = importlib.util.spec_from_file_location("robot_check_ref", ROOT / "tests" / "robot_check_ref.py")
_ref_mod = importlib.util.module_from_spec(_rspec)
_rspec.loader.exec_module(_ref_mod)
ref = _ref_mod.bind(robot)
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


def same_as_ref(res, old, what):
    """The windowed checker's result against night 0's: every check with the same
    verdict and value, every night-0 number with the same value."""
    def close(a, b):
        if isinstance(a, (int, float)) and isinstance(b, (int, float)):
            return abs(a - b) <= 0.002
        return a == b
    for k, c in old["checks"].items():
        mine = res["checks"].get(k)
        expect(mine is not None and mine["ok"] == c["ok"] and close(mine["value"], c["value"]),
               f"{what}: check {k} is {mine}, night 0 {c}")
    for k, v in old["numbers"].items():
        expect(close(res["numbers"].get(k), v), f"{what}: number {k} is {res['numbers'].get(k)}, night 0 {v}")
    expect(set(res["checks"]) - set(old["checks"]) <= {"note_hold"}, f"{what}: new checks {set(res['checks'])}")


def run_check(scn, recs, offsets=None, vs_ref=True):
    offsets = offsets or {}
    data = {n: robot_data(r, offsets.get(n, 0)) for n, r in recs.items()}
    res = robot.check(scn, data)
    if vs_ref:
        same_as_ref(res, ref["check"](scn, data), scn.get("name", "made-up run"))
    return res


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
    expect(robot.scenario_end_us(scn) == 100_000, "end is the last unplug")


def random_scenario(rng):
    streams = []
    for _ in range(rng.randint(1, 4)):
        st = {"robot": rng.choice("ab"), "cable": rng.randint(0, 1), "start_ms": rng.choice([0, 1, 2.5, 10])}
        every = rng.choice([1, 2.5, 5, 10, 12.5])
        if rng.random() < 0.7:
            # gate can be longer than every: offs land after the next ons
            st["notes"] = {"count": rng.randint(1, 60), "every_ms": every, "channel": rng.randint(1, 16),
                           "notes": rng.sample(range(30, 90), rng.randint(1, 5)),
                           "gate_ms": rng.choice([0.5, every / 2, every, every * 3])}
        if rng.random() < 0.5:
            st["cc"] = {"count": rng.randint(1, 40), "every_ms": rng.choice([every, 7, 20]), "cc": rng.randint(0, 119)}
        if rng.random() < 0.3:
            st["clock"] = {"bpm": rng.choice([90, 120, 133]), "ticks": rng.randint(3, 100)}
        if rng.random() < 0.3:
            st["unplug"] = {"every_ms": rng.choice([50, 100]), "down_ms": 20, "count": rng.randint(1, 4)}
        if rng.random() < 0.3:
            st["messages"] = [[rng.choice([0, 5, 5, 30]), rng.choice(["FE", "B07B00", "903C40"])] for _ in range(4)]
        streams.append(st)
    return {"robots": ["a", "b"], "streams": streams}


def test_plan_is_night0s_order():
    """plan() is lazy, but makes exactly the lines night 0's expand() did, in its order."""
    import random
    rng = random.Random(7)
    scns = [json.loads(f.read_text()) for f in sorted(SCENARIOS.glob("*.json"))]
    scns += [random_scenario(rng) for _ in range(300)]
    for i, scn in enumerate(scns):
        old, new = ref["expand"](scn), robot.expand(scn)
        expect(old == new, f"scenario {i} ({scn.get('name')}): plan differs from night 0")
        end = max(ref["scenario_end_us"](old), robot.ms_us(scn.get("duration_ms", 0)))
        expect(robot.scenario_end_us(scn) == end, f"scenario {i}: end {robot.scenario_end_us(scn)} vs {end}")
        for r in new.values():
            ts = [t for t, _ in r["at"]]
            expect(ts == sorted(ts), f"scenario {i}: times go back (the robot says err order)")


def test_plan_lazy_and_repeat():
    huge = {"robots": ["a"], "streams": [{"robot": "a", "notes": {"count": 10 ** 9, "every_ms": 10}}]}
    it = robot.plan(huge)["a"]["at"]
    first = [next(it) for _ in range(4)]
    expect([t for t, _ in first] == [0, 5000, 10000, 15000], f"a billion notes, lazily: {first}")
    expect(robot.scenario_end_us(huge) == (10 ** 9 - 1) * 10_000 + 5000, "end without expanding")
    scn = {"robots": ["a"], "repeat": 3, "period_ms": 1000, "streams": [
        {"robot": "a", "notes": {"count": 2, "every_ms": 100, "gate_ms": 50}},
        {"robot": "a", "clock": {"bpm": 120, "ticks": 4}}]}
    p = robot.expand(scn)["a"]
    ons = [t for t, l in p["at"] if l.split()[2] == "0" and l.split()[3].startswith("90")]
    expect(ons == [0, 100_000, 1_000_000, 1_100_000, 2_000_000, 2_100_000], f"three passes: {ons}")
    expect(p["rep"] == ["rep 0 20833333 101 0 F8"], f"one clock over all passes: {p['rep']}")
    expect(sum(1 for _, l in p["at"] if l.endswith(" FA")) == 1, "one clock start")
    expect(robot.scenario_end_us(scn) == max(t for t, _ in p["at"]), "end of the last pass")
    try:
        robot.plan(dict(scn, period_ms=100))
        expect(False, "a period shorter than a pass is refused")
    except robot.RobotError:
        pass
    long = robot.with_duration(json.loads((SCENARIOS / "controller-to-synth.json").read_text()), 8 * 3600)
    expect(robot.scenario_end_us(long) >= 8 * 3600e6, f"8 h: {robot.scenario_end_us(long) / 3.6e9:.2f} h")
    clock = robot.with_duration(json.loads((SCENARIOS / "clock-out.json").read_text()), 3600)
    expect(robot.scenario_end_us(clock) >= 3600e6, "the clock scenario stretched to an hour")
    eight = robot.with_duration(json.loads((SCENARIOS / "clock-out.json").read_text()), 8 * 3600)
    tmin = eight["listen"][0]["clock"]["ticks_min"]
    grown = 2800 + 48 * (robot.scenario_end_us(eight) - 60e6) / 1e6  # 48 ticks a second at 120 BPM
    expect(abs(tmin - grown) <= 1 and tmin > 1_380_000, f"ticks_min grows with the run, keeping the lead-in: {tmin}")
    # A P4 clock that stops at minute 3 of 8 hours fails, though every interval it sent was perfect.
    interval = 60e6 / (120 * 24)
    ticks = [("r", int(1_000_000 + i * interval), pkt(0, 0xF8)) for i in range(120 * 24 * 3)]
    eight["routes"], eight["streams"] = [], []
    res = run_check(eight, {"a": ticks, "b": ticks}, vs_ref=False)
    expect("clock_ticks_a0" in failed(res) and "clock_jitter" not in failed(res), f"a clock that stops: {failed(res)}")
    expect(robot.duration_s("8h") == 28800 and robot.duration_s("90m") == 5400 and robot.duration_s("15") == 15,
           "durations")


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
    expect(res["checks"]["remounted"]["ok"], "remounted")
    # No mount after the plug (a software unplug the host never saw come back):
    # the window closes 2 s after the plug, so a drop 3 s later is loss.
    recs = notes_run(n=500, drop={40, 41, 42, 900})
    recs["a"] += [("u", 1_195_000, bytes(4)), ("p", 1_225_000, bytes(4))]
    res = run_check(ONE, recs)
    expect(res["checks"]["loss"]["value"] == 1 and res["numbers"]["lost_unplugged"] == 3,
           f"no mount: later loss still counts: {res['checks']['loss']} {res['numbers']}")
    expect(not res["checks"]["remounted"]["ok"] and res["checks"]["remounted"]["value"] == "0/1",
           "no mount fails remounted")


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
        expect(robot.main(["run", str(SCENARIOS / "merge.json"), "--sim", "--runs", str(runs)]) == 0,
               "--sim runs two robots")


def sim_two(name_or_scn, *opts, streaming=False, **kw):
    """A scenario (any number of robots) on robot_sim, routed as its routes say.
    streaming: through run_checked() (periodic syncs, the windowed checker as
    the logs drain), else run(keep=True) and check()."""
    scn = name_or_scn if isinstance(name_or_scn, dict) else json.loads((SCENARIOS / name_or_scn).read_text())
    robots = robot.sim_robots(scn, list(opts))
    if streaming:
        return robot.run_checked(scn, robots, log=lambda *_: None, poll_s=robot.SIM_POLL_S, **kw)
    try:
        data = robot.run(scn, robots, log=lambda *_: None, poll_s=robot.SIM_POLL_S)
    finally:
        for rb in robots.values():
            rb.link.close()
    return robot.check(scn, data), data


def test_sim_options():
    scn = json.loads((SCENARIOS / "split.json").read_text())
    o = robot.sim_options(scn)
    expect(o[:2] == ["--robots", "2"] and "0.0:1.0,notes=0-59,mapch=2" in o and "0.0:1.1,notes=60-127,mapch=3,tr=12" in o,
           f"split's routes for the sim: {o}")
    o = robot.sim_options(json.loads((SCENARIOS / "clock-out.json").read_text()))
    expect(o.count("--p4-clock-to") == 2 and "0.1:1.1" in o, f"clock-out for the sim: {o}")


def test_sim_two_robot_scenarios():
    """Every use-case scenario passes on a clean pretend P4, and each fault fails its check."""
    if not robot.SIM.exists():
        return
    for f in sorted(SCENARIOS.glob("*.json")):
        res, _ = sim_two(f.name, "--jitter-us", "200")
        expect(res["ok"], f"{f.name} on the sim: {failed(res)} {res['numbers']}")
    for name, opt, val, want in (("merge.json", "--drop-every", "50", "loss"), ("merge.json", "--dup-every", "100", "extra"),
                                 ("merge.json", "--latency-us", "7000", "latency"), ("merge.json", "--jitter-us", "3000", "jitter"),
                                 ("split.json", "--drop-every", "40", "loss"),
                                 ("controller-to-synth.json", "--swap-every", "97", "order"),  # with latency 3 ms below
                                 ("clock-out.json", "--p4-clock-jitter-us", "1500", "clock_jitter"),
                                 ("clock-out.json", "--drop-every", "100", "loss"),
                                 ("hotplug-song.json", "--reenum-ms", "2500", "remounted")):
        # A swap needs two deliveries in flight at once: 3 ms latency against 2 ms spacing.
        extra = ("--latency-us", "3000") if opt == "--swap-every" else ()
        res, _ = sim_two(name, opt, val, *extra)
        expect(want in failed(res), f"{name} with {opt} {val} fails {want}: {failed(res)}")
    # The P4 releases a departed controller's notes: released, not extra. Without
    # it the note is held for the whole outage (reenum 247 ms: the replug lands
    # between notes, so no note-off for the key comes soon after).
    res, _ = sim_two("hotplug-song.json", "--reenum-ms", "247")
    expect(res["ok"] and res["numbers"]["released"] == 20 and res["numbers"]["reconnects"] == 50,
           f"hotplug-song releases: {failed(res)} {res['numbers']}")
    res, _ = sim_two("hotplug-song.json", "--reenum-ms", "247", "--no-release")
    expect(failed(res) == ["note_hold"] and res["numbers"]["note_hold_ms_max"] > 500, f"no release: {failed(res)} {res['numbers']}")
    # Robot b's clock 7 s ahead and 80 ppm fast: the clock model still gives 1 ms.
    res, _ = sim_two("controller-to-synth.json", "--offset-us", "1:7000000", "--drift-ppm", "1:80")
    expect(res["ok"] and abs(res["numbers"]["latency_ms_p50"] - 1.0) <= 0.002, f"offset clocks: {res['numbers']}")


def test_windowed_equals_in_memory():
    """The same run (same seed) checked as it drains, with a sync every second,
    and checked whole afterwards: the same verdicts and numbers."""
    if not robot.SIM.exists():
        return
    for name, opts in (("merge.json", ("--jitter-us", "300", "--drop-every", "300", "--offset-us", "1:3000000",
                                       "--drift-ppm", "1:-40")),
                       ("hotplug-song.json", ("--reenum-ms", "247", "--jitter-us", "150")),
                       ("clock-out.json", ("--p4-clock-jitter-us", "400", "--dup-every", "500")),
                       ("split.json", ("--swap-every", "61",))):
        batch, _ = sim_two(name, *opts)
        stream, _ = sim_two(name, *opts, streaming=True, sync_s=1.0)
        same_as_ref(stream, batch, f"{name} windowed vs whole")
        expect(failed(stream) == failed(batch), f"{name}: {failed(stream)} vs {failed(batch)}")


def test_windowed_memory_is_flat():
    """A 4-minute soak keeps a few seconds of records, not the run: the
    checker's buffers stay the same size from the first minute to the last."""
    if not robot.SIM.exists():
        return
    scn = robot.with_duration(json.loads((SCENARIOS / "hotplug-song.json").read_text()), 240)
    robots = robot.sim_robots(scn, ["--jitter-us", "200"])
    sizes = []
    checker_box = {}
    orig = robot.Checker

    class Spy(orig):
        def __init__(self, *a):
            super().__init__(*a)
            checker_box["c"] = self

        def advance(self):
            r = super().advance()
            sizes.append(self.snapshot()["buffered"] + len(self.held_on))
            return r
    robot.Checker = Spy
    try:
        res, _ = robot.run_checked(scn, robots, log=lambda *_: None, poll_s=robot.SIM_POLL_S)
    finally:
        robot.Checker = orig
    expect(res["ok"] and res["numbers"]["matched"] > 25_000, f"4-min soak: {failed(res)} {res['numbers']}")
    q = len(sizes) // 4
    early, late = max(sizes[q:2 * q]), max(sizes[3 * q:])
    expect(late <= 2 * early + 50 and late < 5000, f"held records stay flat: early {early}, late {late}")


def test_wifi_clock_ties():
    """Clock ties as poor as Wi-Fi's (each tie's round trip 4-50 ms, the reading
    anywhere in it): the cross-robot scenarios still pass, jitter still sees the
    P4's own spread, and too much of it still fails."""
    if not robot.SIM.exists():
        return
    scn = robot.with_duration(json.loads((SCENARIOS / "merge.json").read_text()), 120)
    res, _ = robot.run_checked(scn, robot.sim_robots(scn, ["--jitter-us", "200"], tie_ms=(4, 50)), log=lambda *_: None,
                               poll_s=robot.SIM_POLL_S, sync_s=5)
    # (0.093 ms with perfect ties; the 1 ms limit is far off either way)
    expect(res["ok"] and res["numbers"]["jitter_ms"] < 0.3 and res["numbers"]["latency_basis_us"] > 1000,
           f"merge over Wi-Fi ties: {failed(res)} {res['numbers']}")
    res, _ = robot.run_checked(scn, robot.sim_robots(scn, ["--jitter-us", "3000"], tie_ms=(4, 50)), log=lambda *_: None,
                               poll_s=robot.SIM_POLL_S, sync_s=5)
    expect("jitter" in failed(res), f"3 ms of P4 jitter over Wi-Fi ties: {failed(res)} {res['numbers']}")


def test_slow_ties_dont_stall_the_checker():
    """One lucky 2 ms tie at the start, then every tie 20 ms: the run skips a
    few, then takes them, and the checker keeps up instead of waiting."""
    if not robot.SIM.exists():
        return
    scn = robot.with_duration(json.loads((SCENARIOS / "controller-to-synth.json").read_text()), 60)
    robots = robot.sim_robots(scn)
    robots = {n: robot.TieNoiseRobot(rb, rtts=[2000] + [20000] * 200) for n, rb in robots.items()}
    reached = []
    orig = robot.Checker

    class Spy(orig):
        def advance(self):
            r = super().advance()
            reached.append(self.ingested_mac)
            return r
    robot.Checker = Spy
    try:
        res, data = robot.run_checked(scn, robots, log=lambda *_: None, poll_s=robot.SIM_POLL_S, sync_s=1.0)
    finally:
        robot.Checker = orig
    end_mac = data["a"]["sync"][-1][0]
    # Skipping every slow tie (night 1's first rule) left it the whole minute behind.
    expect(res["ok"] and max(reached) > end_mac - 6e6, f"checker kept up: {(end_mac - max(reached)) / 1e6:.1f} s behind, {failed(res)} {res['numbers']}")
    expect(0 < res["numbers"]["a_syncs_rejected"] <= 20, f"some skipped, not all: {res['numbers']['a_syncs_rejected']}")


def test_saved_run_rechecks():
    if not robot.SIM.exists():
        return
    with tempfile.TemporaryDirectory() as d:
        save, prog = pathlib.Path(d) / "run.jsonl", pathlib.Path(d) / "progress.jsonl"
        res, _ = sim_two("merge.json", "--drop-every", "200", streaming=True, save=str(save), progress=str(prog),
                         progress_s=2.0)
        scn, again = robot.check_saved(str(save))
        expect(scn["name"] == "merge" and again["checks"] == res["checks"], f"saved run re-checks: {failed(again)}")
        lines = [json.loads(l) for l in prog.read_text().splitlines()]
        expect(len(lines) >= 4 and lines[-1]["done"] and "matched" in lines[1], f"progress lines: {len(lines)}")
        # night 0's one-object format still reads
        rb = robot.Robot("a", robot.SimLink([]))
        scn1 = json.loads((SCENARIOS / "loopback.json").read_text())
        try:
            data = robot.run(scn1, {"a": rb}, log=lambda *_: None)
        finally:
            rb.link.close()
        old = pathlib.Path(d) / "old.json"
        old.write_text(json.dumps({"scenario": scn1, "data": {
            n: dict(x, records=[(k_, t, p.hex()) for k_, t, p in x["records"]]) for n, x in data.items()}}))
        expect(robot.check_saved(str(old))[1]["ok"], "a night-0 saved run re-checks")
        part = pathlib.Path(d) / "part.jsonl"
        part.write_text("\n".join(save.read_text().splitlines()[:5]) + "\n")
        try:
            robot.check_saved(str(part))
            expect(False, "an unfinished save is refused")
        except robot.RobotError:
            pass


def test_scenarios_parse():
    for f in sorted(SCENARIOS.glob("*.json")):
        scn = json.loads(f.read_text())
        plan = robot.expand(scn)
        expect(set(plan) == set(scn["robots"]), f"{f.name}: robots")
        for rt in scn.get("routes", []):
            expect(rt["from"]["robot"] in scn["robots"] and rt["to"]["robot"] in scn["robots"], f"{f.name}: route robots")


def fake_wifi_robot(stale=b""):
    """A TCP server speaking the robot's id/ping replies, with stale output left
    queued by an earlier client. Returns (host, port, thread)."""
    import socket
    import threading
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)

    def serve():
        c, _ = srv.accept()
        if stale:
            c.sendall(stale)
        buf = b""
        while True:
            chunk = c.recv(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if line == b"id":
                    c.sendall(b"ok id mac=aa:bb:cc:dd:a5:55 fw=00000c2 link=wifi ip=10.0.0.9 rssi=-50 "
                              b"host=robot-a555.local cables=2\n")
                elif line == b"ping":
                    c.sendall(b"# a comment line\nok pong\n")
        c.close()
        srv.close()

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    return "127.0.0.1", srv.getsockname()[1], t


def test_wifi_link():
    host, port, t = fake_wifi_robot(stale=b"ok log more=0\nok status t=5\n")
    rb = robot.Robot("a", robot.open_link(f"{host}:{port}"))
    ident = rb.ident()  # the stale "ok" lines must not answer this
    expect(ident.get("mac") == "aa:bb:cc:dd:a5:55", f"tcp id mac: {ident}")
    expect(ident.get("host") == "robot-a555.local" and ident.get("fw") == "00000c2", f"tcp id fields: {ident}")
    _, r = rb.cmd("ping")
    expect(r == "ok pong", f"tcp ping: {r}")
    rb.link.close()
    t.join(2)


def test_wifi_check_mac():
    host, port, t = fake_wifi_robot()
    robot.check_mac(f"{host}:{port}", "AA:BB:CC:DD:A5:55")  # case doesn't matter
    t.join(2)
    host, port, t = fake_wifi_robot()
    try:
        robot.check_mac(f"{host}:{port}", "aa:bb:cc:dd:00:01")
        expect(False, "tcp check-mac accepted the wrong MAC")
    except robot.RobotError:
        pass
    t.join(2)


def test_nvs_wifi_check():
    # Entry keys in NVS are 16-byte fields, null-padded: names are findable, values aren't read.
    nvs = b"\xff" * 64 + b"wifi" + b"\x00" * 12 + b"\xff" * 16 + b"ssid" + b"\x00" * 12 + b"\xff" * 32
    expect(robot.nvs_has_wifi(nvs), "nvs with wifi/ssid")
    expect(not robot.nvs_has_wifi(b"\xff" * 0x5000), "erased nvs")
    expect(not robot.nvs_has_wifi(b"boot\x00computer\x00" + b"\xff" * 100), "nvs without wifi")


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
