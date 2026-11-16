#!/usr/bin/env python3
"""Drives bench robots (firmware/bench_robot, docs/bench-robot.md) from the Mac:
ESP32-S3 DevKitC boards on the P4's hub as USB MIDI devices, each controlled
over its UART port. It runs a scenario file and checks what came out.

usage:
  robot.py run SCENARIO.json --robot a=/dev/cu.usbserial-X [--robot b=...] [--mac a=aa:bb:..]
                                      play the scenario, collect both robots' logs, check them
  robot.py run SCENARIO.json --sim [--sim-opt=--drop-every=10 ...]
                                      the same against build/robot_sim (one robot, cable 0 routed to 1)
  robot.py check LOG.json             re-check a saved run (--save writes one)
  robot.py cmd --port P LINE...       send protocol lines, print the replies (status, log, unplug 500 ...)
  robot.py check-mac PORT MAC         exit 0 only if PORT is an ESP32-S3 with that MAC (make flash-bench-robot)
                                      (or, for a Wi-Fi robot's host, if its id reply has that MAC)
  robot.py flash-wifi PORT MAC BUILD BOOT_APP0   a Wi-Fi robot's first flash by cable: backs up the
                                      whole flash, checks NVS knows a network, never touches NVS
  robot.py ota HOST MAC IMAGE [--expect-fw HEX]   reflash a Wi-Fi robot over the network

A Wi-Fi robot (ROBOT_WIFI builds: Big-S3, Small-V2) is named by host instead of a port anywhere a
port goes: --robot a=robot-a555.local, cmd --port robot-a555.local.
  [--runs build/robot/runs.jsonl] [--baud 921600] [--save run.json]

A scenario says what each robot plays (streams), what the P4 is meant to do
with it (routes: from a robot's cable, through an optional filter and map, to
a robot's cable), and the limits for the checks. It doesn't set the P4 up:
the routes are what the P4's own config must already do.

The checks, like bench.py's, are read from the robots' logs, never from a person:
  accounted     every packet the robots counted is in the logs, and no log overflowed
  loss          routed messages that never arrived (except around an unplug: lost_unplugged)
  extra         arrivals no route explains (duplicates, strays)
  order         arrivals out of their send order, per route
  latency       the worst arrival time (latency_ms_max), and jitter (p99 - p50, jitter_ms_max)
  clock_jitter  the worst clock interval's distance from the sent one
  stuck_notes   note-ons left without a note-off at a receiver
Latency is exact when both ends are one robot (cable 0 out, cable 1 back in:
one clock). Between robots the clocks are tied through the UART (a
time request at the start and the end, the fastest of several), and the
result carries that uncertainty as latency_basis_us.

Safety: a robot is opened only on the port named for it. --mac refuses a
robot whose MAC differs, by its id reply. The port is opened with DTR and RTS
held, so opening it doesn't reset the board (its auto-reset circuit).
Standard library only, except pyserial for a real port (make's tools venv has it).
"""
import argparse
import bisect
import collections
import glob
import json
import os
import pathlib
import re
import socket
import statistics
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIM = ROOT / "build" / "robot_sim"
BAUD = 921600
BATCH = 64          # at lines per write: well under the robot's 4 KB UART receive buffer
POLL_S = 0.1        # how often a run tops up the schedules and drains the logs
SCHED_MARGIN = 16   # sched_free kept spare
UNPLUG_GRACE_US = 100_000


class RobotError(Exception):
    pass


# --- links: how lines reach a robot ----------------------------------------

class SerialLink:
    """A robot's UART port. pyserial is imported only here, so the tests and --sim never need it."""

    def __init__(self, port, baud=BAUD):
        import serial  # noqa: PLC0415
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, baud, 0.05
        s.dtr = False  # before open(): no reset pulse through the auto-reset circuit
        s.rts = False
        s.exclusive = True
        s.open()
        self.s = s
        self.buf = b""

    def write(self, text):
        self.s.write(text.encode())

    def readline(self, timeout):
        end = time.monotonic() + timeout
        while b"\n" not in self.buf:
            if time.monotonic() > end:
                raise RobotError(f"no reply from {self.s.port} in {timeout} s")
            self.buf += self.s.read(4096)
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace").strip()

    def now_us(self):
        return time.monotonic_ns() // 1000

    def sleep(self, s):
        time.sleep(s)

    def close(self):
        self.s.close()


class TcpLink:
    """A Wi-Fi robot's control line (ROBOT_WIFI builds): the same protocol on TCP port 7000.
    Times are stamped on the robot, so Wi-Fi latency only widens the time-sync basis."""

    def __init__(self, host, port=7000, timeout=5.0):
        self.where = f"{host}:{port}"
        try:
            self.s = socket.create_connection((host, port), timeout=timeout)
        except OSError as e:
            raise RobotError(f"can't reach robot at {self.where}: {e}") from e
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        # A previous client may have left output in the robot's queue: drop it,
        # or a stale "ok" would answer this client's first command.
        self.s.settimeout(0.3)
        try:
            while self.s.recv(65536):
                pass
        except (socket.timeout, BlockingIOError):
            pass

    def write(self, text):
        self.s.sendall(text.encode())

    def readline(self, timeout):
        end = time.monotonic() + timeout
        while b"\n" not in self.buf:
            left = end - time.monotonic()
            if left <= 0:
                raise RobotError(f"no reply from {self.where} in {timeout} s")
            self.s.settimeout(left)
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                raise RobotError(f"{self.where} closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace").strip()

    def now_us(self):
        return time.monotonic_ns() // 1000

    def sleep(self, s):
        time.sleep(s)

    def close(self):
        self.s.close()


def open_link(spec, baud=BAUD):
    """/dev/... is a UART robot; anything else is a Wi-Fi robot, host or host:port."""
    if spec.startswith("/dev/"):
        return SerialLink(spec, baud)
    host, _, port = spec.rpartition(":") if spec.count(":") == 1 else (spec, "", "")
    return TcpLink(host, int(port) if port else 7000)


class SimLink:
    """build/robot_sim on a pipe. Its time is virtual, and the Mac's clock is the sim's."""

    def __init__(self, opts=()):
        if not SIM.exists():
            raise RobotError(f"{SIM} isn't built: make build/robot_sim")
        self.p = subprocess.Popen([str(SIM), *opts], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def write(self, text):
        self.p.stdin.write(text)
        self.p.stdin.flush()

    def readline(self, timeout):
        line = self.p.stdout.readline()
        if not line:
            raise RobotError("robot_sim exited")
        return line.strip()

    def _sim(self, what):
        self.write(f"#sim {what}\n")
        while True:
            line = self.readline(1)
            if line.startswith("#sim t="):
                return int(line[7:])

    def now_us(self):
        return self._sim("now")

    def sleep(self, s):
        self._sim(f"advance {int(s * 1e6)}")

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=5)


# --- one robot ---------------------------------------------------------------

STATUS_RE = re.compile(r"(\w+)=(-?\d+)")


class Robot:
    def __init__(self, name, link):
        self.name, self.link = name, link

    def cmds(self, lines, timeout=2.0):
        """Sends lines, returns (data lines, reply) for each. An err reply raises."""
        self.link.write("".join(f"{l}\n" for l in lines))
        out = []
        for l in lines:
            data = []
            while True:
                r = self.link.readline(timeout)
                if r.startswith("#") or not r:
                    continue
                if r.startswith("ok") or r.startswith("err"):
                    break
                data.append(r)
            if r.startswith("err"):
                raise RobotError(f"{self.name}: '{l}' -> {r}")
            out.append((data, r))
        return out

    def cmd(self, line, timeout=2.0):
        return self.cmds([line], timeout)[0]

    def ident(self):
        _, r = self.cmd("id")
        return dict(kv.split("=", 1) for kv in r.split()[2:] if "=" in kv)

    def status(self):
        _, r = self.cmd("status")
        return {k: int(v) for k, v in STATUS_RE.findall(r)}

    def sync(self, n=8):
        """(mac_us, robot_us, rtt_us) from the fastest of n time requests."""
        best = None
        for _ in range(n):
            a = self.link.now_us()
            _, r = self.cmd("time")
            b = self.link.now_us()
            sample = ((a + b) // 2, int(r.split()[2]), b - a)
            if best is None or sample[2] < best[2]:
                best = sample
        return best

    def drain(self, max_records=2000):
        data, r = self.cmd(f"log {max_records}", timeout=10)
        recs = [parse_record(l) for l in data]
        more = int(r.split("more=")[1])
        return recs, more


def parse_record(line):
    kind, t, hexpkt = line.split()
    return (kind, int(t), bytes.fromhex(hexpkt))


# --- scenarios ---------------------------------------------------------------

def msg_len(status):
    if status < 0xF0:
        return 2 if status >> 4 in (0xC, 0xD) else 3
    return {0xF1: 2, 0xF3: 2, 0xF2: 3}.get(status, 1)


def kind_of(msg):
    s = msg[0]
    if s < 0xF0:
        return {0x8: "note", 0x9: "note", 0xA: "note", 0xB: "cc"}.get(s >> 4, "other")
    if s == 0xF8:
        return "clock"
    if s in (0xFA, 0xFB, 0xFC):
        return "transport"
    return "other"


def ms_us(v):
    return int(round(float(v) * 1000))


def expand(scn):
    """{robot: {"at": [(t_rel_us, line)], "rep": [line]}} for a scenario's streams."""
    out = {r: {"at": [], "rep": []} for r in scn["robots"]}
    for st in scn.get("streams", []):
        rb = st["robot"]
        if rb not in out:
            raise RobotError(f"stream for robot {rb}, which the scenario doesn't list")
        t0 = ms_us(st.get("start_ms", 0))
        cable = st.get("cable", 0)
        at = out[rb]["at"]

        def midi(t, *b):
            at.append((t, f"at {t} {cable} {bytes(b).hex().upper()}"))
        if "notes" in st:
            n = st["notes"]
            ch = n.get("channel", 1) - 1
            keys = n.get("notes", [60])
            for i in range(n["count"]):
                t = t0 + ms_us(i * n["every_ms"])
                k = keys[i % len(keys)]
                midi(t, 0x90 | ch, k, n.get("velocity", 100))
                midi(t + ms_us(n.get("gate_ms", n["every_ms"] / 2)), 0x80 | ch, k, 0)
        if "cc" in st:
            c = st["cc"]
            ch = c.get("channel", 1) - 1
            lo, hi, cnt = c.get("from", 0), c.get("to", 127), c["count"]
            for i in range(cnt):
                v = lo + round((hi - lo) * i / max(cnt - 1, 1))
                midi(t0 + ms_us(i * c["every_ms"]), 0xB0 | ch, c["cc"], v)
        if "clock" in st:
            c = st["clock"]
            interval_ns = round(60e9 / (c["bpm"] * 24))
            if c.get("start", True):
                midi(t0, 0xFA)
            out[rb]["rep"].append(f"rep {t0} {interval_ns} {c['ticks']} {cable} F8")
            if c.get("stop", True):
                midi(t0 + (c["ticks"] * interval_ns) // 1000 + 1, 0xFC)
        if "unplug" in st:
            u = st["unplug"]
            for i in range(u["count"]):
                t = t0 + ms_us(i * u["every_ms"])
                at.append((t, f"at {t} unplug {int(u['down_ms'])}"))
        for t_ms, hx in st.get("messages", []):
            t = t0 + ms_us(t_ms)
            at.append((t, f"at {t} {cable} {hx.upper()}"))
    for r in out.values():
        r["at"].sort(key=lambda e: e[0])
    return out


def scenario_end_us(plan):
    end = 0
    for r in plan.values():
        if r["at"]:
            end = max(end, r["at"][-1][0])
        for line in r["rep"]:
            _, start, ns, count, _, _ = line.split()
            end = max(end, int(start) + int(ns) * int(count) // 1000)
    return end


# --- running -----------------------------------------------------------------

def run(scn, robots, log=print, expect_macs=None):
    """Plays the scenario on robots ({name: Robot}) and returns the run's data for check()."""
    plan = expand(scn)
    expect_macs = expect_macs or {}
    data = {}
    for name, rb in robots.items():
        ident = rb.ident()
        want = expect_macs.get(name)
        if want and ident.get("mac", "").lower() != want.lower():
            raise RobotError(f"robot {name} has MAC {ident.get('mac')}, not {want}: wrong port?")
        rb.cmd("clear")
        data[name] = {"id": ident, "records": [], "sync": [rb.sync()]}
    pending = {n: list(plan[n]["at"]) for n in robots}

    def top_up(name, rb, free):
        lines = []
        while pending[name] and free - len(lines) > SCHED_MARGIN:
            lines.append(pending[name].pop(0)[1])
        for i in range(0, len(lines), BATCH):
            rb.cmds(lines[i:i + BATCH])

    for name, rb in robots.items():
        top_up(name, rb, rb.status()["sched_free"])
        for line in plan[name]["rep"]:
            rb.cmd(line)
    for name, rb in robots.items():
        _, r = rb.cmd(f"go {ms_us(scn.get('lead_ms', 200))}")
        data[name]["t0"] = int(r.split("t0=")[1])
    end_us = scenario_end_us(plan) + ms_us(scn.get("settle_ms", 300))
    log(f"running {scn.get('name', 'scenario')}: {end_us / 1e6:.1f} s")
    while True:
        rb_done = True
        for name, rb in robots.items():
            st = rb.status()
            top_up(name, rb, st["sched_free"])
            recs, _ = rb.drain()
            data[name]["records"] += recs
            elapsed = st["t"] - data[name]["t0"]
            if elapsed < end_us or pending[name] or st["sched"] or st["reps"]:
                rb_done = False
        if rb_done:
            break
        next(iter(robots.values())).link.sleep(POLL_S)
    for name, rb in robots.items():
        rb.cmd("stop")
        while True:
            recs, more = rb.drain()
            data[name]["records"] += recs
            if not more:
                break
        # Mount events are logged by the robot's next poll, a little after
        # sends stamped later than them: put each robot's log in time order.
        data[name]["records"].sort(key=lambda r: r[1])
        data[name]["status"] = rb.status()
        data[name]["sync"].append(rb.sync())
    return data


# --- checking ----------------------------------------------------------------

def clock_model(d):
    """robot µs -> Mac µs, and its uncertainty, from the start and end time requests."""
    (m0, r0, rtt0), (m1, r1, rtt1) = d["sync"][0], d["sync"][-1]
    scale = (m1 - m0) / (r1 - r0) if r1 != r0 else 1.0
    return (lambda t: m0 + (t - r0) * scale), max(rtt0, rtt1) / 2


def passes(f, msg):
    if not f:
        return True
    ch = (msg[0] & 0x0F) + 1 if msg[0] < 0xF0 else None
    if "types" in f and kind_of(msg) not in f["types"]:
        return False
    if "channels" in f and ch is not None and ch not in f["channels"]:
        return False
    if "notes" in f and kind_of(msg) == "note":
        lo, hi = f["notes"]
        if not lo <= msg[1] <= hi:
            return False
    return True


def mapped(m, msg):
    if not m or msg[0] >= 0xF0:
        return msg
    b = bytearray(msg)
    if "channel" in m:
        b[0] = (b[0] & 0xF0) | (m["channel"] - 1)
    if "transpose" in m and kind_of(msg) == "note":
        b[1] = max(0, min(127, b[1] + m["transpose"]))
    return bytes(b)


def outages(recs, to_mac, reenum_us):
    """(start, end) Mac µs windows from each unplug to the host's next mount, plus a grace.
    A mount that never comes closes the window reenum_us after the plug, so a
    robot whose host never sees it again can't excuse every later loss."""
    out, start, plug = [], None, None
    for kind, t, _ in sorted(recs, key=lambda r: r[1]):
        if kind == "u" and start is None:
            start, plug = to_mac(t), None
        elif kind == "p" and start is not None:
            plug = to_mac(t)
        elif kind == "M" and start is not None and (plug is None or to_mac(t) <= plug + reenum_us):
            out.append((start - UNPLUG_GRACE_US, to_mac(t) + UNPLUG_GRACE_US))
            start = plug = None
        elif start is not None and plug is not None and to_mac(t) > plug + reenum_us:
            out.append((start - UNPLUG_GRACE_US, plug + reenum_us + UNPLUG_GRACE_US))
            start = plug = None
    if start is not None:
        end = plug + reenum_us + UNPLUG_GRACE_US if plug is not None else float("inf")
        out.append((start - UNPLUG_GRACE_US, end))
    return out


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]


def check(scn, data):
    """The checks for one run: {"checks": {name: {ok, value, limit}}, "numbers": {...}, "ok": bool}."""
    lim = scn.get("checks", {})
    models = {n: clock_model(d) for n, d in data.items()}
    checks, numbers = {}, {}

    def put(name, value, limit, ok):
        checks[name] = {"ok": ok, "value": value, "limit": limit}

    # accounted: the logs hold every packet the robots counted.
    gaps = []
    for n, d in data.items():
        st = d["status"]
        s = sum(1 for k, _, _ in d["records"] if k == "s")
        r = sum(1 for k, _, _ in d["records"] if k == "r")
        if s != st["sent"] or r != st["recv"] or st["log_lost_tx"] or st["log_lost_rx"]:
            gaps.append(f"{n}: sent {st['sent']} logged {s}, recv {st['recv']} logged {r}, "
                        f"log lost {st['log_lost_tx']}+{st['log_lost_rx']}")
    put("accounted", "; ".join(gaps) or "all", "all", not gaps)

    reenum_us = ms_us(lim.get("reenum_ms_max", 2000))
    down = {n: outages(d["records"], models[n][0], reenum_us) for n, d in data.items()}

    def in_outage(robot_names, t_mac):
        return any(a <= t_mac <= b for n in robot_names for a, b in down[n])

    # What each route expects, keyed by where it lands and what it is.
    expect = collections.defaultdict(list)
    routes = scn.get("routes", [])
    for ri, rt in enumerate(routes):
        src, dst = rt["from"], rt["to"]
        to_mac = models[src["robot"]][0]
        seq = 0
        for kind, t, pkt in data[src["robot"]]["records"]:
            if kind != "s" or pkt[0] >> 4 != src.get("cable", 0):
                continue
            msg = pkt[1:1 + msg_len(pkt[1])]
            if not passes(rt.get("filter"), msg):
                continue
            want = mapped(rt.get("map"), msg)
            expect[(dst["robot"], dst.get("cable", 0), want)].append(
                {"route": ri, "seq": seq, "t": t, "t_mac": to_mac(t), "src": src["robot"]})
            seq += 1
    for key in expect:
        expect[key] = sorted(expect[key], key=lambda e: e["t_mac"])

    # Arrivals, each matched to an expectation of the same content. First an
    # estimate of the latency: each arrival against the latest expectation sent
    # before it. Then each arrival takes the unmatched expectation nearest
    # (arrival - that latency); one further off than the match window is an
    # extra (a duplicate), and expectations passed over are lost. So one lost
    # or doubled message among identical ones (clock ticks) is the one counted,
    # as long as identical messages are further apart than latency + jitter.
    dest_cables = {(rt["to"]["robot"], rt["to"].get("cable", 0)) for rt in routes}
    # listen: a cable that takes what the P4 makes itself (its clock), checked
    # against the scenario's numbers rather than a robot's sends.
    listens = {(ls["robot"], ls.get("cable", 0)): ls for ls in scn.get("listen", [])}
    heard = collections.defaultdict(list)
    arrivals, extras = [], 0
    for n, d in data.items():
        to_mac = models[n][0]
        for kind, t, pkt in d["records"]:
            if kind != "r":
                continue
            cable = pkt[0] >> 4
            msg = pkt[1:1 + msg_len(pkt[1])]
            ls = listens.get((n, cable))
            if ls and passes(ls.get("filter", {"types": ["clock", "transport"]}), msg):
                heard[(n, cable)].append((t, msg))
            elif (n, cable) not in dest_cables:
                extras += 1
            else:
                arrivals.append((to_mac(t), t, n, cable, msg))
    arrivals.sort(key=lambda a: a[0])

    times = {k: [e["t_mac"] for e in q] for k, q in expect.items()}
    est = []
    for a in arrivals:
        ts = times.get((a[2], a[3], a[4]))
        if ts:
            i = bisect.bisect_right(ts, a[0]) - 1
            if i >= 0:
                est.append(a[0] - ts[i])
    typical = statistics.median(est) if est else 0
    mad = statistics.median(abs(x - typical) for x in est) if est else 0
    window = ms_us(lim["match_window_ms"]) if "match_window_ms" in lim else max(5000, 8 * mad)
    numbers["match_window_ms"] = round(window / 1000, 3)

    pos = collections.defaultdict(int)
    pairs, missing = [], []
    for a in arrivals:
        k = (a[2], a[3], a[4])
        q = expect.get(k)
        i = pos[k]
        if not q or i >= len(q):
            extras += 1
            continue
        target = a[0] - typical
        j = i
        while j + 1 < len(q) and abs(q[j + 1]["t_mac"] - target) < abs(q[j]["t_mac"] - target):
            j += 1
        if abs(q[j]["t_mac"] - target) > window:
            extras += 1
            continue
        missing += [(a[2], e) for e in q[i:j]]
        pairs.append((a, q[j]))
        pos[k] = j + 1
    missing += [(k[0], e) for k, q in expect.items() for e in q[pos[k]:]]

    lat, order_bad, cross = [], 0, False
    last_seq = {}
    clock_rx = collections.defaultdict(list)
    held = collections.Counter()
    for (t_mac, t, n, cable, msg), e in pairs:
        same = e["src"] == n
        cross |= not same
        lat.append((t - e["t"]) if same else (t_mac - e["t_mac"]))
        key = (e["route"], n, cable)
        if e["seq"] < last_seq.get(key, -1):
            order_bad += 1
        last_seq[key] = max(e["seq"], last_seq.get(key, -1))
        if msg[0] == 0xF8:
            clock_rx[key].append(t)
    # Stuck notes: what each receiver holds down, in arrival order.
    for t_mac, t, n, cable, msg in arrivals:
        if kind_of(msg) == "note" and msg[0] < 0xA0:
            k = (n, cable, msg[0] & 0x0F, msg[1])
            if msg[0] >> 4 == 0x9 and msg[2] > 0:
                held[k] += 1
            elif held[k] > 0:
                held[k] -= 1

    lost = lost_unplugged = 0
    for dr, e in missing:
        if in_outage({dr, e["src"]}, e["t_mac"]):
            lost_unplugged += 1
        else:
            lost += 1
    put("loss", lost, lim.get("loss_max", 0), lost <= lim.get("loss_max", 0))
    numbers["lost_unplugged"] = lost_unplugged
    put("extra", extras, lim.get("extra_max", 0), extras <= lim.get("extra_max", 0))
    if lim.get("order", True):
        put("order", order_bad, 0, order_bad == 0)
    numbers["matched"] = len(lat)
    if lat:
        ms = [x / 1000 for x in lat]
        numbers.update(latency_ms_p50=round(pct(ms, 50), 3), latency_ms_p99=round(pct(ms, 99), 3),
                       latency_ms_max=round(max(ms), 3), latency_ms_min=round(min(ms), 3),
                       jitter_ms=round(pct(ms, 99) - pct(ms, 50), 3))
        numbers["latency_basis_us"] = (round(max(models[n][1] for n in data)) if cross else 0)
        if "latency_ms_max" in lim:
            put("latency", numbers["latency_ms_max"], lim["latency_ms_max"],
                numbers["latency_ms_max"] <= lim["latency_ms_max"])
        if "jitter_ms_max" in lim:
            put("jitter", numbers["jitter_ms"], lim["jitter_ms_max"], numbers["jitter_ms"] <= lim["jitter_ms_max"])
    worst = None
    for (ri, n, cable), ts in clock_rx.items():
        # The nominal interval is the sender's own: the rep line's grid.
        sent = [e for e in (r for r in data[routes[ri]["from"]["robot"]]["records"])
                if e[0] == "s" and e[2][1] == 0xF8 and e[2][0] >> 4 == routes[ri]["from"].get("cable", 0)]
        if len(ts) < 3 or len(sent) < 3:
            continue
        nominal = (sent[-1][1] - sent[0][1]) / (len(sent) - 1)
        devs = [abs((b - a) - nominal) / 1000 for a, b in zip(ts, ts[1:])]
        worst = max(worst or 0, max(devs))
        numbers["clock_interval_ms_stdev"] = round(statistics.pstdev([(b - a) / 1000 for a, b in zip(ts, ts[1:])]), 4)
    for key, got in heard.items():
        ls = listens[key]
        ts = [t for t, m in got if m[0] == 0xF8]
        numbers[f"{key[0]}{key[1]}_heard"] = len(got)
        if "clock" in ls and len(ts) >= 3:
            nominal = 60e6 / (ls["clock"]["bpm"] * 24)
            devs = [abs((b - a) - nominal) / 1000 for a, b in zip(ts, ts[1:])]
            worst = max(worst or 0, max(devs))
            numbers[f"{key[0]}{key[1]}_clock_bpm"] = round(60e6 / ((ts[-1] - ts[0]) / (len(ts) - 1)) / 24, 3)
            if "ticks_min" in ls["clock"]:
                put(f"clock_ticks_{key[0]}{key[1]}", len(ts), ls["clock"]["ticks_min"], len(ts) >= ls["clock"]["ticks_min"])
    if worst is not None:
        numbers["clock_jitter_ms"] = round(worst, 4)
        if "clock_jitter_ms_max" in lim:
            put("clock_jitter", round(worst, 4), lim["clock_jitter_ms_max"], worst <= lim["clock_jitter_ms_max"])
    stuck = sum(v for v in held.values() if v > 0)
    put("stuck_notes", stuck, lim.get("stuck_notes_max", 0), stuck <= lim.get("stuck_notes_max", 0))

    reconnect, plugs = [], 0
    for n, d in data.items():
        plug_t = None
        for kind, t, _ in sorted(d["records"], key=lambda r: r[1]):
            if kind == "p":
                plugs += 1
                plug_t = t
            elif kind == "M" and plug_t is not None:
                if t - plug_t <= reenum_us:
                    reconnect.append((t - plug_t) / 1000)
                plug_t = None
        numbers[f"{n}_not_sent"] = d["status"]["not_sent"]
        numbers[f"{n}_unplugs"] = d["status"]["unplugs"]
    if plugs:
        # Every replug must end in the host mounting the robot again, in time.
        put("remounted", f"{len(reconnect)}/{plugs}", f"all, within {reenum_us // 1000} ms", len(reconnect) == plugs)
    if reconnect:
        numbers.update(reconnect_ms_max=round(max(reconnect), 1), reconnect_ms_p50=round(pct(reconnect, 50), 1),
                       reconnects=len(reconnect))
    return {"ok": all(c["ok"] for c in checks.values()), "checks": checks, "numbers": numbers}


def report(name, result, log=print):
    for k, c in result["checks"].items():
        log(f"{'PASS' if c['ok'] else 'FAIL'} {k}: {c['value']} (limit {c['limit']})")
    log("numbers: " + ", ".join(f"{k}={v}" for k, v in result["numbers"].items()))
    log(f"{name}: {'ok' if result['ok'] else 'FAILED'}")


# --- check-mac (make flash-bench-robot) --------------------------------------

def esptool(port, *args, after="hard-reset", timeout=60):
    tools = sorted(glob.glob(str(pathlib.Path.home() / "Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool")))
    if not tools:
        raise RobotError("no esptool under ~/Library/Arduino15")
    out = subprocess.run([tools[-1], "--chip", "esp32s3", "--port", port, "--after", after, *args],
                         capture_output=True, text=True, timeout=timeout)
    if out.returncode:
        raise RobotError(f"{port}: esptool {args[0]} failed ({(out.stdout + out.stderr).strip().splitlines()[-1:]})")
    return out.stdout


def check_mac(port, want, after="hard-reset"):
    """A UART or download-mode port: esptool reads the chip's MAC (a P4 fails the
    chip check, another S3 the MAC). A Wi-Fi robot: its own id reply."""
    if not port.startswith("/dev/"):
        rb = Robot("robot", open_link(port))
        try:
            got = rb.ident().get("mac", "")
        finally:
            rb.link.close()
    else:
        try:
            m = re.search(r"MAC:\s+([0-9a-fA-F:]{17})", esptool(port, "read-mac", after=after))
        except RobotError as e:
            raise RobotError(f"{port}: not an ESP32-S3 in reach ({e})") from e
        got = m.group(1) if m else ""
    if got.lower() != want.lower():
        raise RobotError(f"{port} has MAC {got or '?'}, not {want}: not flashing")


# --- Wi-Fi robots: the first flash by cable, then OTA ------------------------

NVS = (0x9000, 0x5000)  # Outrigger's and Arduino's default tables both put NVS here


def nvs_has_wifi(nvs):
    """True when an NVS dump holds Outrigger's saved network (namespace wifi, key
    ssid). Only names are looked at: the values never leave this function."""
    return b"wifi\x00" in nvs and b"ssid\x00" in nvs


def flash_wifi(port, mac, build, boot_app0, backups, allow_no_wifi=False, log=print):
    """Backs up the board's whole flash (once per MAC: the first backup is the one
    that restores Outrigger), checks it knows a Wi-Fi network, then writes the
    bootloader, partition table, otadata and app at their own offsets. Never a
    merged image at 0x0: its padding would erase NVS, and the robot could never
    join Wi-Fi to be reached. The board must be in download or Computer mode."""
    check_mac(port, mac, after="no-reset")
    backups = pathlib.Path(backups)
    backups.mkdir(parents=True, exist_ok=True)
    tag = mac.replace(":", "").lower()
    done = sorted(backups.glob(f"{tag}-*.bin"))
    if done:
        image = done[0]
        log(f"backup already taken: {image}")
    else:
        size = re.search(r"Detected flash size:\s*(\d+)MB", esptool(port, "flash-id", after="no-reset"))
        if not size:
            raise RobotError(f"{port}: flash size not reported")
        image = backups / f"{tag}-{time.strftime('%Y%m%d-%H%M%S')}.bin"
        part = image.with_suffix(".part")
        log(f"backing up {size.group(1)} MB to {image} (a few minutes)")
        esptool(port, "read-flash", "0", str(int(size.group(1)) << 20), str(part), after="no-reset", timeout=1800)
        os.replace(part, image)
    with open(image, "rb") as f:
        f.seek(NVS[0])
        nvs = f.read(NVS[1])
    if not nvs_has_wifi(nvs) and not allow_no_wifi:
        raise RobotError(f"{image}: NVS has no saved Wi-Fi network (namespace wifi, key ssid). The robot would "
                         "never be reachable: join Wi-Fi from Outrigger first, or pass --allow-no-wifi")
    build = pathlib.Path(build)
    parts = [("0x0", build / "bench_robot.ino.bootloader.bin"), ("0x8000", build / "bench_robot.ino.partitions.bin"),
             ("0xe000", pathlib.Path(boot_app0)), ("0x10000", build / "bench_robot.ino.bin")]
    for _, f in parts:
        if not f.exists():
            raise RobotError(f"{f} is missing: make bench-robot-wifi")
    args = [x for off, f in parts for x in (off, str(f))]
    esptool(port, "write-flash", *args, after="hard-reset", timeout=300)
    log(f"{port}: flashed {mac}; it joins Wi-Fi as robot-{tag[-4:]}.local")


def ota(host, mac, image, expect_fw=None, log=print):
    """Pushes an app image to a Wi-Fi robot with ArduinoOTA (espota.py, port 3232),
    after its id proves it's the robot meant, then waits for it to come back and
    checks the new build is running."""
    check_mac(host, mac)
    espota = sorted(glob.glob(str(pathlib.Path.home() / "Library/Arduino15/packages/esp32/hardware/esp32/*/tools/espota.py")))
    if not espota:
        raise RobotError("no espota.py under ~/Library/Arduino15")
    ip = socket.gethostbyname(host.split(":")[0])
    out = subprocess.run([sys.executable, "-I", espota[-1], "-i", ip, "-p", "3232", "-f", str(image)],
                         capture_output=True, text=True, timeout=300)
    if out.returncode:
        raise RobotError(f"{host}: OTA failed ({(out.stdout + out.stderr).strip().splitlines()[-1:]})")
    end = time.monotonic() + 90
    while True:
        time.sleep(3)
        try:
            rb = Robot("robot", open_link(host))
            try:
                ident = rb.ident()
            finally:
                rb.link.close()
            break
        except (RobotError, OSError):
            if time.monotonic() > end:
                raise RobotError(f"{host} didn't come back within 90 s of the OTA") from None
    if ident.get("mac", "").lower() != mac.lower():
        raise RobotError(f"{host} came back as {ident.get('mac')}, not {mac}")
    if expect_fw is not None and int(ident.get("fw", "-1"), 16) != expect_fw:
        raise RobotError(f"{host} runs fw={ident.get('fw')} after the OTA, not {expect_fw:07x}")
    log(f"{host}: running fw={ident.get('fw')} ({ident.get('ip')})")


# --- main --------------------------------------------------------------------

def kv(items):
    out = {}
    for it in items or []:
        k, _, v = it.partition("=")
        if not v:
            raise SystemExit(f"expected name=value, got {it}")
        out[k] = v
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="what", required=True)
    r = sub.add_parser("run")
    r.add_argument("scenario")
    r.add_argument("--robot", action="append",
                   help="name=/dev/cu.... or name=robot-xxxx.local (one per robot in the scenario)")
    r.add_argument("--mac", action="append", help="name=aa:bb:cc:dd:ee:ff: refuse a robot with another MAC")
    r.add_argument("--sim", action="store_true", help="run against build/robot_sim instead")
    r.add_argument("--sim-opt", action="append", default=[], help="an option for robot_sim, e.g. --sim-opt=--drop-every=10")
    r.add_argument("--baud", type=int, default=BAUD)
    r.add_argument("--runs", default=str(ROOT / "build" / "robot" / "runs.jsonl"))
    r.add_argument("--save", help="write the run's logs and result here (robot.py check reads it)")
    c = sub.add_parser("check")
    c.add_argument("saved")
    m = sub.add_parser("cmd")
    m.add_argument("--port", required=True)
    m.add_argument("--baud", type=int, default=BAUD)
    m.add_argument("lines", nargs="+")
    k = sub.add_parser("check-mac")
    k.add_argument("port")
    k.add_argument("mac")
    fw = sub.add_parser("flash-wifi", help="first flash of a Wi-Fi robot by cable (backs up the whole flash first)")
    fw.add_argument("port")
    fw.add_argument("mac")
    fw.add_argument("build")
    fw.add_argument("boot_app0")
    fw.add_argument("--backups", default=str(ROOT / "build" / "robot-backups"))
    fw.add_argument("--allow-no-wifi", action="store_true")
    o = sub.add_parser("ota", help="reflash a Wi-Fi robot over the network")
    o.add_argument("host")
    o.add_argument("mac")
    o.add_argument("image")
    o.add_argument("--expect-fw", type=lambda v: int(v, 16))
    a = ap.parse_args(argv)
    try:
        if a.what == "flash-wifi":
            flash_wifi(a.port, a.mac, a.build, a.boot_app0, a.backups, a.allow_no_wifi)
            return 0
        if a.what == "ota":
            ota(a.host, a.mac, a.image, a.expect_fw)
            return 0
        if a.what == "check-mac":
            check_mac(a.port, a.mac)
            print(f"{a.port}: ESP32-S3 {a.mac}")
            return 0
        if a.what == "cmd":
            rb = Robot("robot", open_link(a.port, a.baud))
            for line in a.lines:
                data, reply = rb.cmd(line, timeout=10)
                for d in data:
                    print(d)
                print(reply)
            return 0
        if a.what == "check":
            saved = json.loads(pathlib.Path(a.saved).read_text())
            data = {n: dict(d, records=[(k_, t, bytes.fromhex(p)) for k_, t, p in d["records"]])
                    for n, d in saved["data"].items()}
            result = check(saved["scenario"], data)
            report(saved["scenario"].get("name", a.saved), result)
            return 0 if result["ok"] else 1
        scn = json.loads(pathlib.Path(a.scenario).read_text())
        if a.sim:
            if len(scn["robots"]) != 1:
                raise RobotError("--sim runs one-robot scenarios (cable 0 routed back to cable 1)")
            opts = [o for s in a.sim_opt for o in s.split("=", 1)]
            robots = {scn["robots"][0]: Robot(scn["robots"][0], SimLink(opts))}
        else:
            ports = kv(a.robot)
            missing = [n for n in scn["robots"] if n not in ports]
            if missing:
                raise RobotError(f"no --robot for {', '.join(missing)}")
            robots = {n: Robot(n, open_link(ports[n], a.baud)) for n in scn["robots"]}
        try:
            data = run(scn, robots, expect_macs=kv(a.mac))
        finally:
            for rb in robots.values():
                rb.link.close()
        result = check(scn, data)
        name = scn.get("name", pathlib.Path(a.scenario).stem)
        report(name, result)
        line = {"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "scenario": name, "sim": a.sim,
                "robots": {n: d["id"] for n, d in data.items()}, **result}
        runs = pathlib.Path(a.runs)
        runs.parent.mkdir(parents=True, exist_ok=True)
        with runs.open("a") as f:
            f.write(json.dumps(line) + "\n")
        if a.save:
            pathlib.Path(a.save).write_text(json.dumps({
                "scenario": scn, "result": result,
                "data": {n: dict(d, records=[(k_, t, p.hex()) for k_, t, p in d["records"]]) for n, d in data.items()}}))
        return 0 if result["ok"] else 1
    except RobotError as e:
        print(f"robot.py: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
