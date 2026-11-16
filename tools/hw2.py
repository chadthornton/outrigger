#!/usr/bin/env python3
"""C1-1 and C1-2 on the bench: track D's graph on Big-P4 routing the Wi-Fi robots (spike night 1, HW2).

    python3 -I tools/hw2.py run SCENARIO OUTDIR [robot.py run options, e.g. --duration 8h]
    python3 -I tools/hw2.py all OUTDIR          loopback, controller-to-synth, merge, split, clock-out
    python3 -I tools/hw2.py route SCENARIO      only set the P4's routing for it
    python3 -I tools/hw2.py graph               the P4's /graph now

For each scenario: set the P4's routing (POST /bench/graph, the `about` of each
scenario JSON says what it needs), zero the P4's counters, run the scenario
through tools/robot.py (ROBOT_PY, default ~/Projects/usb-midi-host-n1-s3, branch
spike/platform-p4-e-robot), then save /graph and /usb from before and after.
Everything goes over Wi-Fi; no serial port is opened.
"""

import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.request

ROBOT_PY = os.environ.get("ROBOT_PY", os.path.expanduser("~/Projects/usb-midi-host-n1-s3/tools/robot.py"))
SCN_DIR = pathlib.Path(ROBOT_PY).parent / "robot_scenarios"
P4 = os.environ.get("P4HUB_HOST", "192.168.68.88")
ROBOTS = {"a": ("robot-2b28.local", "44:bd:8d:61:2b:28"), "b": ("robot-a23c.local", "a0:85:e3:f0:a2:3c")}

# The P4 routing each scenario's `about` asks for (endpoints: robot + 0-based cable).
ROUTES = {
    "loopback": "app=thru&in0=a0&out0=a1",
    "controller-to-synth": "app=thru&in0=a0&out0=b0",
    "merge": "app=merge&in0=a0&in1=b0&out0=b1",
    "split": "app=split&in0=a0&out0=b0&out1=b1&split_key=60&ch_lower=1&ch_upper=2&split_t=12",
    "clock-out": "app=clock&plain=1&bpm=120&in0=a1&out0=b1&out1=a0,b0",
    "hotplug-song": "app=thru&in0=a0&out0=b0",
    "hotplug": "app=thru&in0=a0&out0=a1",
}
ALL = ["loopback", "controller-to-synth", "merge", "split", "clock-out"]


def http(path, method="GET", timeout=10):
    req = urllib.request.Request(f"http://{P4}{path}", method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def graph():
    http("/graph")  # the first take after a while returns an older snapshot; the second is fresh
    time.sleep(0.25)
    return http("/graph")


def set_route(name):
    q = ROUTES[name]
    r = http(f"/bench/graph?{q}", method="POST")
    if not r.get("ok"):
        raise SystemExit(f"P4 refused route {q}: {r}")
    g = graph()
    for e in g["in"]:
        if not e["addr"]:
            raise SystemExit(f"P4 in-endpoint {e['ep']} has no USB device: {g['in']}")
    for o in g["out"]:
        for ep, a in zip(o["to"], o["addr"]):
            if ep and not a:
                raise SystemExit(f"P4 out-endpoint {ep} has no USB device: {g['out']}")
    return g


def summary(g):
    s = g.get("stats", {})
    return {
        "app": g["app"],
        "in": [(e["ep"], e["pushed"], e["refused"]) for e in g["in"]],
        "out": [(o["to"], o["sent"]) for o in g["out"]],
        "client": g["client"],
        "wire": {k: g["wire"][k] for k in ("nodest", "send_fail", "late_n", "late_mean_us", "late_max_us", "late_hist")},
        "block_clock": {k: g["block_clock"][k] for k in
                        ("runs", "frames48", "frames_other", "frames_min", "frames_max", "wake_min_us", "wake_max_us",
                         "wake_hist")},
        "drops": {k: s.get(k) for k in ("ev_dropped", "in_dropped", "out_dropped", "late", "resyncs")},
        "events": (s.get("in_events"), s.get("out_events")),
        "block_max_us": s.get("block_max_us"),
        "nodes": [(n["name"], n["mean_us"], n["max_us"]) for n in s.get("nodes", [])],
    }


def run(name, out, extra=()):
    d = pathlib.Path(out) / name
    d.mkdir(parents=True, exist_ok=True)
    scn = SCN_DIR / f"{name}.json"
    robots = json.loads(scn.read_text()).get("robots", ["a"])
    set_route(name)
    http("/bench/graph?reset=1", method="POST")
    time.sleep(0.3)
    before = graph()
    usb_before = http("/usb")
    (d / "graph-before.json").write_text(json.dumps(before, indent=1))
    cmd = [sys.executable, "-I", ROBOT_PY, "run", str(scn), "--runs", str(pathlib.Path(out) / "runs.jsonl"),
           "--save", str(d / "save.jsonl"), "--progress", str(d / "progress.jsonl")]
    for r in robots:
        cmd += ["--robot", f"{r}={ROBOTS[r][0]}", "--mac", f"{r}={ROBOTS[r][1]}"]
    cmd += list(extra)
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    wall = time.time() - t0
    (d / "robot.out.txt").write_text(p.stdout + p.stderr)
    after = graph()
    usb_after = http("/usb")
    (d / "graph-after.json").write_text(json.dumps(after, indent=1))
    (d / "usb.json").write_text(json.dumps({"before": usb_before, "after": usb_after}, indent=1))
    checks = [l for l in p.stdout.splitlines() if l.startswith(("PASS", "FAIL"))]
    numbers = next((l for l in p.stdout.splitlines() if l.startswith("numbers:")), "")
    res = {"scenario": name, "route": ROUTES[name], "extra": list(extra), "rc": p.returncode, "wall_s": round(wall, 1),
           "ok": p.returncode == 0, "checks": checks, "numbers": numbers, "p4": summary(after),
           "t": time.strftime("%Y-%m-%d %H:%M:%S")}
    (d / "result.json").write_text(json.dumps(res, indent=1))
    with open(pathlib.Path(out) / "results.jsonl", "a") as f:
        f.write(json.dumps(res) + "\n")
    print(f"== {name}: {'PASS' if res['ok'] else 'FAIL'} (rc {p.returncode}, {wall:.1f} s)")
    for c in checks:
        print("  " + c)
    print("  " + numbers)
    print("  P4: " + json.dumps(res["p4"]))
    if p.returncode and not checks:
        print(p.stdout[-2000:] + p.stderr[-2000:])
    return res


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == "graph":
        print(json.dumps(graph(), indent=1))
    elif argv[0] == "route":
        print(json.dumps(set_route(argv[1]), indent=1))
    elif argv[0] == "run":
        return 0 if run(argv[1], argv[2], argv[3:])["ok"] else 1
    elif argv[0] == "all":
        bad = [n for n in ALL if not run(n, argv[1])["ok"]]
        print("all: " + ("PASS" if not bad else "FAIL " + " ".join(bad)))
        return 1 if bad else 0
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
