#!/usr/bin/env python3
"""C0-1 on the bench: Big-P4's hub host with both Wi-Fi robots (spike night 1, HW1).

    python3 -I tools/c0_1.py enum OUTDIR          the P4's view of each robot: cables, channels
    python3 -I tools/c0_1.py cables OUTDIR        both robots x both cables through the P4's echo
    python3 -I tools/c0_1.py hotplug OUTDIR       100 software unplug/replugs per robot (c0-1-hotplug100.json)
    python3 -I tools/c0_1.py port4 OUTDIR         hub port status bits, port 4 power-cycled with longer settles
    python3 -I tools/c0_1.py all OUTDIR

Everything goes over Wi-Fi: the P4's diag page (tools/p4_diag.py's routes) and the
robots' TCP control port, through tools/robot.py from spike/platform-p4-e-robot
(ROBOT_PY, default ~/Projects/usb-midi-host-e/tools/robot.py). No serial port
is opened. Each step writes JSON into OUTDIR and prints the numbers.
"""

import json
import os
import pathlib
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import p4_diag  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
ROBOT_PY = os.environ.get("ROBOT_PY", os.path.expanduser("~/Projects/usb-midi-host-e/tools/robot.py"))
ROBOTS = {"a": ("robot-2b28.local", "44:bd:8d:61:2b:28"), "b": ("robot-a23c.local", "a0:85:e3:f0:a2:3c")}
SERIAL = {n: mac.replace(":", "").upper() for n, (_, mac) in ROBOTS.items()}
P4 = p4_diag.resolve(os.environ.get("P4HUB_HOST", "p4hub.local"))


def p4(path, method="GET", timeout=15):
    return p4_diag.get_json(P4, path, method=method, timeout=timeout)


def robot_cmd(name, *lines):
    host, _ = ROBOTS[name]
    out = subprocess.run([sys.executable, "-I", ROBOT_PY, "cmd", "--port", host, *lines], capture_output=True,
                         text=True, timeout=60)
    return out.stdout.strip().splitlines()


def robot_status(name):
    for line in robot_cmd(name, "status"):
        if line.startswith("ok status"):
            return dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)
    return {}


def save(out, name, data):
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, name), "w") as f:
        json.dump(data, f, indent=1)


def robot_devices(usb):
    found = {}
    for d in usb["devices"]:
        for n, s in SERIAL.items():
            if d["serial"].upper() == s:
                found[n] = d
    return found


def step_enum(out):
    usb = p4("/usb")
    st = {n: robot_status(n) for n in ROBOTS}
    save(out, "enum.json", {"usb": usb, "robots": st})
    print(f"P4: {len(usb['devices'])} devices, channels {usb['channels_used']}/{usb['channels_max']}, "
          f"short_desc_failed {usb['short_desc_failed']}, port_disabled {usb['port_disabled']}")
    for d in usb["devices"]:
        print(f"  addr {d['address']} parent {d['parent']} port {d['port']} {d['vid']}:{d['pid']} {d['product']!r} "
              f"serial {d['serial']} hub={d['hub']} midi in/out cables {d['midi_in_cables']}/{d['midi_out_cables']} "
              f"channels {d['channels']}")
    rd = robot_devices(usb)
    for n in ROBOTS:
        print(f"robot {n}: mounted={st[n].get('mounted')} on P4: {'yes addr %d' % rd[n]['address'] if n in rd else 'NO'}")
    return usb, rd


def run_scenario(out, scn):
    args = [sys.executable, "-I", ROBOT_PY, "run", str(HERE / "robot_scenarios" / f"{scn}.json"),
            "--runs", os.path.join(out, "runs.jsonl"), "--save", os.path.join(out, f"{scn}.run.json")]
    for n, (host, mac) in ROBOTS.items():
        args += ["--robot", f"{n}={host}", "--mac", f"{n}={mac}"]
    t0 = time.time()
    r = subprocess.run(args, capture_output=True, text=True)
    with open(os.path.join(out, f"{scn}.out.txt"), "w") as f:
        f.write(r.stdout + r.stderr)
    print(r.stdout[-4000:], r.stderr[-2000:])
    print(f"{scn}: exit {r.returncode} in {time.time() - t0:.0f} s")
    return r.returncode


def step_cables(out):
    p4("/bench/echo?on=1", "POST")
    p4("/bench/reset", "POST")
    rc = run_scenario(out, "c0-1-cables")
    usb = p4("/usb")
    save(out, "cables-usb.json", usb)
    for n, d in robot_devices(usb).items():
        print(f"P4 counted from robot {n}: rx per cable {d['rx']} other {d['rx_other_cable']} echoed {d['echoed']} "
              f"echo_fail {d['echo_fail']}")
    return rc


def step_hotplug(out):
    p4("/bench/echo?on=1", "POST")
    p4("/bench/reset", "POST")
    before = {"status": p4("/status"), "usb": p4("/usb")}
    save(out, "hotplug-before.json", before)
    rc = run_scenario(out, "c0-1-hotplug100")
    time.sleep(5)
    after = {"status": p4("/status"), "usb": p4("/usb"), "robots": {n: robot_status(n) for n in ROBOTS}}
    save(out, "hotplug-after.json", after)
    b, a = before["status"], after["status"]
    print(f"P4 heap internal {b['heap']['internal_free']} -> {a['heap']['internal_free']}, "
          f"channels {b['usb']['channels_used']} -> {a['usb']['channels_used']}, "
          f"devices {b['usb']['devices']} -> {a['usb']['devices']}, "
          f"short_desc_failed {b['usb']['short_desc_failed']} -> {a['usb']['short_desc_failed']}")
    for k in after["usb"]["identities"]:
        print(f"  {k['product']!r} {k['serial']}: connects {k['connects']} disconnects {k['disconnects']} "
              f"P4 side disconnect->connect (includes the robot's 300 ms down) n={k['remount_n']} min/mean/max "
              f"{k['remount_ms_min']}/{k['remount_ms_mean']}/{k['remount_ms_max']} ms")
    return rc


def step_port4(out):
    res = {"before": {}, "cycles": []}
    for port in range(1, 5):
        res["before"][port] = p4(f"/bench/hubport?port={port}")
        print("port", port, res["before"][port])
    for settle_ms in (500, 2000, 5000):
        c0 = p4("/usb")["short_desc_failed"]
        off = p4("/bench/hubport?port=4&power=0", "POST")
        time.sleep(1.0)
        on = p4("/bench/hubport?port=4&power=1", "POST")
        time.sleep(settle_ms / 1000 + 4)
        usb = p4("/usb")
        st = p4("/bench/hubport?port=4")
        rob = {n: robot_status(n) for n in ROBOTS}
        rec = {"settle_ms": settle_ms, "off": off, "on": on, "status": st,
               "short_desc_failed_delta": usb["short_desc_failed"] - c0, "devices": len(usb["devices"]),
               "robots": {n: {k: rob[n].get(k) for k in ("mounted", "t", "mounts", "unplugs")} for n in rob}}
        res["cycles"].append(rec)
        print(json.dumps(rec))
    save(out, "port4.json", res)


def main():
    what, out = sys.argv[1], sys.argv[2]
    usb, rd = step_enum(out)
    if what == "enum":
        return
    if len(rd) < 2:
        sys.exit("not both robots on the P4: stopping here")
    if what in ("cables", "all"):
        step_cables(out)
    if what in ("hotplug", "all"):
        step_hotplug(out)
    if what in ("port4", "all"):
        step_port4(out)


if __name__ == "__main__":
    main()
