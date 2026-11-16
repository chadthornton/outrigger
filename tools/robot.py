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
import heapq
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
SIM_POLL_S = 0.5    # the same on robot_sim, whose logs hold 4,096 records (virtual time, so it's only speed)
SCHED_MARGIN = 16   # sched_free kept spare
UNPLUG_GRACE_US = 100_000
SYNC_S = 10.0       # a long run ties the robots' clocks to the Mac's this often
PROGRESS_S = 60.0   # and prints (and writes) its numbers so far this often


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


class SimHub:
    """build/robot_sim on a pipe, with one or more robots on its pretend P4. Its
    time is virtual, and the Mac's clock is the sim's."""

    def __init__(self, opts=()):
        if not SIM.exists():
            raise RobotError(f"{SIM} isn't built: make build/robot_sim")
        self.p = subprocess.Popen([str(SIM), *opts], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.users = 0

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

    def close(self):
        self.users -= 1
        if self.users <= 0 and self.p.poll() is None:
            self.p.stdin.close()
            self.p.wait(timeout=5)


class SimLink:
    """One robot in build/robot_sim: its lines go to robot k (@k), its clock is the sim's."""

    def __init__(self, opts=(), hub=None, k=0):
        self.hub = hub or SimHub(opts)
        self.hub.users += 1
        self.k = k

    def write(self, text):
        if self.k:
            text = "".join(f"@{self.k} {l}\n" for l in text.splitlines())
        self.hub.write(text)

    def readline(self, timeout):
        return self.hub.readline(timeout)

    def now_us(self):
        return self.hub._sim("now")

    def sleep(self, s):
        self.hub._sim(f"advance {int(s * 1e6)}")

    def close(self):
        self.hub.close()


def sim_options(scn):
    """robot_sim's options for a scenario: one robot per robot it names, its
    routes as the pretend P4's, and the P4's own clock to the cables that listen
    for one."""
    names = list(scn["robots"])
    opts = ["--robots", str(len(names))]
    kinds = {"note": "note", "cc": "cc", "clock": "clock", "transport": "transport", "other": "other"}
    for rt in scn.get("routes", []):
        f, m = rt.get("filter") or {}, rt.get("map") or {}
        spec = (f"{names.index(rt['from']['robot'])}.{rt['from'].get('cable', 0)}:"
                f"{names.index(rt['to']['robot'])}.{rt['to'].get('cable', 0)}")
        if "notes" in f:
            spec += f",notes={f['notes'][0]}-{f['notes'][1]}"
        if "channels" in f:
            spec += ",ch=" + "+".join(str(c) for c in f["channels"])
        if "types" in f:
            spec += ",types=" + "+".join(kinds[t] for t in f["types"])
        if "channel" in m:
            spec += f",mapch={m['channel']}"
        if "transpose" in m:
            spec += f",tr={m['transpose']}"
        opts += ["--route", spec]
    if not scn.get("routes"):
        opts.append("--no-route")
    for ls in scn.get("listen", []):
        if "clock" in ls:
            opts += ["--p4-clock-bpm", str(ls["clock"]["bpm"]),
                     "--p4-clock-to", f"{names.index(ls['robot'])}.{ls.get('cable', 0)}"]
    return opts


def sim_robots(scn, extra_opts=()):
    """{name: Robot} for a scenario on one robot_sim."""
    hub = SimHub(sim_options(scn) + list(extra_opts))
    return {n: Robot(n, SimLink(hub=hub, k=i)) for i, n in enumerate(scn["robots"])}


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


def _rep_period_us(scn):
    """(passes, period in µs) for a scenario's repeat: the whole pattern of
    streams played back to back, so a 10 s pattern makes an 8-hour soak."""
    passes = int(scn.get("repeat", 1))
    if passes < 1:
        raise RobotError("repeat must be 1 or more")
    end = _pattern_end_us(scn)
    period = ms_us(scn["period_ms"]) if "period_ms" in scn else end + ms_us(scn.get("gap_ms", 100))
    if passes > 1 and period <= end:
        raise RobotError(f"period_ms {period / 1000} must be longer than one pass ({end / 1000} ms)")
    return passes, period


def _clock_interval_ns(c):
    return round(60e9 / (c["bpm"] * 24))


def _clock_ticks(scn, c, passes, period):
    """A clock runs once, from its start to the end of the last pass."""
    if passes == 1:
        return c["ticks"]
    ns = _clock_interval_ns(c)
    return c["ticks"] + ((passes - 1) * period * 1000 + ns - 1) // ns


def _pattern_end_us(scn):
    """The last scheduled time in one pass of the streams (clocks excluded:
    they run once over the whole scenario)."""
    end = 0
    for st in scn.get("streams", []):
        t0 = ms_us(st.get("start_ms", 0))
        if "notes" in st:
            n = st["notes"]
            last = t0 + ms_us((n["count"] - 1) * n["every_ms"])
            end = max(end, last, last + ms_us(n.get("gate_ms", n["every_ms"] / 2)))
        if "cc" in st:
            end = max(end, t0 + ms_us((st["cc"]["count"] - 1) * st["cc"]["every_ms"]))
        if "unplug" in st:
            end = max(end, t0 + ms_us((st["unplug"]["count"] - 1) * st["unplug"]["every_ms"]))
        for t_ms, _ in st.get("messages", []):
            end = max(end, t0 + ms_us(t_ms))
    return end


def _stream_parts(scn, si, st, passes, period):
    """Iterables of ((t, pass, stream, part, i, sub), line), each sorted. The key
    reproduces night 0's stable sort: by time, then insertion order (a stream's
    notes, note i's on before its off, then its CCs, clock start, clock stop,
    unplugs, messages)."""
    t0 = ms_us(st.get("start_ms", 0))
    cable = st.get("cable", 0)
    parts = []

    def line(t, *b):
        return f"at {t} {cable} {bytes(b).hex().upper()}"

    if "notes" in st:
        n = st["notes"]
        ch = n.get("channel", 1) - 1
        keys = n.get("notes", [60])
        gate = ms_us(n.get("gate_ms", n["every_ms"] / 2))
        vel = n.get("velocity", 100)

        def ons(sub, n=n, ch=ch, keys=keys, gate=gate, vel=vel):
            for p in range(passes):
                for i in range(n["count"]):
                    t = t0 + ms_us(i * n["every_ms"]) + p * period
                    k = keys[i % len(keys)]
                    if sub == 0:
                        yield (t, p, si, 0, i, 0), line(t, 0x90 | ch, k, vel)
                    else:
                        yield (t + gate, p, si, 0, i, 1), line(t + gate, 0x80 | ch, k, 0)
        parts += [ons(0), ons(1)]
    if "cc" in st:
        c = st["cc"]
        ch = c.get("channel", 1) - 1
        lo, hi, cnt = c.get("from", 0), c.get("to", 127), c["count"]

        def ccs(c=c, ch=ch, lo=lo, hi=hi, cnt=cnt):
            for p in range(passes):
                for i in range(cnt):
                    t = t0 + ms_us(i * c["every_ms"]) + p * period
                    yield (t, p, si, 1, i, 0), line(t, 0xB0 | ch, c["cc"], lo + round((hi - lo) * i / max(cnt - 1, 1)))
        parts.append(ccs())
    if "clock" in st:
        c = st["clock"]
        ticks = _clock_ticks(scn, c, passes, period)
        ns = _clock_interval_ns(c)
        if c.get("start", True):
            parts.append([((t0, 0, si, 2, 0, 0), line(t0, 0xFA))])
        if c.get("stop", True):
            t = t0 + (ticks * ns) // 1000 + 1
            parts.append([((t, 0, si, 3, 0, 0), line(t, 0xFC))])
    if "unplug" in st:
        u = st["unplug"]

        def unplugs(u=u):
            for p in range(passes):
                for i in range(u["count"]):
                    t = t0 + ms_us(i * u["every_ms"]) + p * period
                    yield (t, p, si, 4, i, 0), f"at {t} unplug {int(u['down_ms'])}"
        parts.append(unplugs())
    msgs = sorted(((t0 + ms_us(t_ms), i, hx) for i, (t_ms, hx) in enumerate(st.get("messages", []))))
    if msgs:
        def messages(msgs=msgs):
            for p in range(passes):
                for t, i, hx in msgs:
                    t += p * period
                    yield (t, p, si, 5, i, 0), f"at {t} {cable} {hx.upper()}"
        parts.append(messages())
    return parts


def plan(scn):
    """{robot: {"at": iterator of (t_rel_us, line) in time order, "rep": [line]}}.
    Lazy: an 8-hour scenario is generated as the run takes it, never held."""
    for st in scn.get("streams", []):
        if st["robot"] not in scn["robots"]:
            raise RobotError(f"stream for robot {st['robot']}, which the scenario doesn't list")
    passes, period = _rep_period_us(scn)
    out = {}
    for r in scn["robots"]:
        parts, reps = [], []
        for si, st in enumerate(scn.get("streams", [])):
            if st["robot"] != r:
                continue
            parts += _stream_parts(scn, si, st, passes, period)
            if "clock" in st:
                c = st["clock"]
                reps.append(f"rep {ms_us(st.get('start_ms', 0))} {_clock_interval_ns(c)} "
                            f"{_clock_ticks(scn, c, passes, period)} {st.get('cable', 0)} F8")
        merged = heapq.merge(*parts, key=lambda e: e[0])
        out[r] = {"at": ((k[0], ln) for k, ln in merged), "rep": reps}
    return out


def expand(scn):
    """plan(), held in lists: for small scenarios and the tests."""
    return {r: {"at": list(p["at"]), "rep": p["rep"]} for r, p in plan(scn).items()}


def scenario_end_us(scn):
    """The scenario's last scheduled time (or its duration_ms), worked out without expanding it."""
    passes, period = _rep_period_us(scn)
    end = _pattern_end_us(scn) + (passes - 1) * period if scn.get("streams") else 0
    for st in scn.get("streams", []):
        if "clock" in st:
            c = st["clock"]
            t0 = ms_us(st.get("start_ms", 0))
            ns = _clock_interval_ns(c)
            ticks = _clock_ticks(scn, c, passes, period)
            end = max(end, t0 + ns * ticks // 1000, t0 + (ticks * ns) // 1000 + 1 if c.get("stop", True) else 0, t0)
    return max(end, ms_us(scn.get("duration_ms", 0)))


def with_duration(scn, seconds):
    """The scenario stretched to at least `seconds`: its pattern repeated (a soak),
    or, with no streams (the P4's own clock), its duration_ms set."""
    scn = dict(scn)
    want = int(seconds * 1e6)
    if not scn.get("streams"):
        scn["duration_ms"] = want / 1000
        return scn
    scn["repeat"] = 1
    _, period = _rep_period_us(scn)
    scn["repeat"] = max(1, -(-want // period))
    return scn


# --- running -----------------------------------------------------------------

def run(scn, robots, log=print, expect_macs=None, keep=True, on_sync=None, on_records=None, on_progress=None,
        sync_s=SYNC_S, progress_s=PROGRESS_S, poll_s=POLL_S):
    """Plays the scenario on robots ({name: Robot}) and returns the run's data.

    keep=True holds every record in data[name]["records"] for check(). A long
    run passes keep=False and takes them as they come: on_records(name, recs,
    complete_us) (complete_us: the robot time its log is drained through, or
    None) and on_sync(name, (mac_us, robot_us, rtt_us)), every sync_s. on_progress()
    is called every progress_s."""
    pl = plan(scn)
    expect_macs = expect_macs or {}
    on_sync = on_sync or (lambda *_: None)
    on_records = on_records or (lambda *_: None)
    data = {}
    for name, rb in robots.items():
        ident = rb.ident()
        want = expect_macs.get(name)
        if want and ident.get("mac", "").lower() != want.lower():
            raise RobotError(f"robot {name} has MAC {ident.get('mac')}, not {want}: wrong port?")
        rb.cmd("clear")
        s = rb.sync()
        data[name] = {"id": ident, "records": [], "sync": [s], "syncs_rejected": 0}
        on_sync(name, s)
    pending = {n: pl[n]["at"] for n in robots}
    head = {n: next(pending[n], None) for n in robots}

    def top_up(name, rb, free):
        lines = []
        while head[name] is not None and free - len(lines) > SCHED_MARGIN:
            lines.append(head[name][1])
            head[name] = next(pending[name], None)
        for i in range(0, len(lines), BATCH):
            rb.cmds(lines[i:i + BATCH])

    def take(name, recs, complete):
        if keep:
            data[name]["records"] += recs
        on_records(name, recs, complete)

    for name, rb in robots.items():
        top_up(name, rb, rb.status()["sched_free"])
        for line in pl[name]["rep"]:
            rb.cmd(line)
    for name, rb in robots.items():
        _, r = rb.cmd(f"go {ms_us(scn.get('lead_ms', 200))}")
        data[name]["t0"] = int(r.split("t0=")[1])
    end_us = scenario_end_us(scn) + ms_us(scn.get("settle_ms", 300))
    log(f"running {scn.get('name', 'scenario')}: {end_us / 1e6:.1f} s")
    clock = next(iter(robots.values())).link
    next_sync = next_progress = clock.now_us()
    while True:
        rb_done = True
        now = clock.now_us()
        if now >= next_sync:
            next_sync = now + int(sync_s * 1e6)
            for name, rb in robots.items():
                s = rb.sync()
                best = min(x[2] for x in data[name]["sync"])
                # A slow round trip is a poor tie between the clocks: skip it.
                if s[2] > max(3 * best, best + 5000):
                    data[name]["syncs_rejected"] += 1
                    continue
                data[name]["sync"] = data[name]["sync"][:1] + [s] if not keep else data[name]["sync"] + [s]
                on_sync(name, s)
        for name, rb in robots.items():
            st = rb.status()
            top_up(name, rb, st["sched_free"])
            recs, more = rb.drain()
            take(name, recs, None if more else st["t"])
            elapsed = st["t"] - data[name]["t0"]
            if elapsed < end_us or head[name] is not None or st["sched"] or st["reps"]:
                rb_done = False
        if on_progress and now >= next_progress:
            next_progress = now + int(progress_s * 1e6)
            on_progress()
        if rb_done:
            break
        clock.sleep(poll_s)
    for name, rb in robots.items():
        rb.cmd("stop")
        while True:
            recs, more = rb.drain()
            take(name, recs, None)
            if not more:
                break
        data[name]["status"] = rb.status()
        take(name, [], data[name]["status"]["t"])
        s = rb.sync()
        data[name]["sync"].append(s)
        on_sync(name, s)
    for name in robots:
        # Mount events are logged by the robot's next poll, a little after
        # sends stamped later than them: put each robot's log in time order.
        data[name]["records"].sort(key=lambda r: r[1])
    return data


# --- checking ----------------------------------------------------------------

class ClockModel:
    """Robot µs -> Mac µs, piecewise linear through the time requests (syncs):
    between two syncs by interpolation, past the ends by the nearest segment.
    One segment between the start and end syncs is night 0's model."""

    def __init__(self):
        self.pts = []    # (robot_us, mac_us)
        self.rtt_max = 0

    def add(self, sync):
        mac, rob, rtt = sync
        if self.pts and rob <= self.pts[-1][0]:
            return
        self.pts.append((rob, mac))
        self.rtt_max = max(self.rtt_max, rtt)

    def known_through(self):
        return self.pts[-1][0] if self.pts else None

    def __call__(self, t):
        p = self.pts
        if len(p) == 1:
            return p[0][1] + (t - p[0][0])
        i = bisect.bisect_right(p, (t, float("inf"))) - 1
        i = min(max(i, 0), len(p) - 2)
        (r0, m0), (r1, m1) = p[i], p[i + 1]
        return m0 + (t - r0) * ((m1 - m0) / (r1 - r0))



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


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]


def pct_counter(c, p):
    """pct() over a Counter of values: the same element, without the list."""
    n = sum(c.values())
    k = min(n - 1, int(round(p / 100 * (n - 1))))
    for v in sorted(c):
        k -= c[v]
        if k < 0:
            return v
    raise ValueError("empty")


def pstdev_counter(c):
    n = sum(c.values())
    mean = sum(v * k for v, k in c.items()) / n
    return (sum(k * (v - mean) ** 2 for v, k in c.items()) / n) ** 0.5


def is_note_off(msg):
    return msg[0] < 0xF0 and (msg[0] >> 4 == 0x8 or (msg[0] >> 4 == 0x9 and msg[2] == 0))


def is_all_notes_off(msg):
    return msg[0] >> 4 == 0xB and msg[1] in (120, 123)


class Outages:
    """One robot's outage windows, from its u/p/M records in time order: from an
    unplug to the host's next mount, plus UNPLUG_GRACE_US each side. A mount that
    never comes closes the window reenum_us after the plug."""

    def __init__(self, reenum_us):
        self.reenum_us = reenum_us
        self.closed = collections.deque()   # (start, end), Mac µs
        self.start = self.plug = None       # the open one

    def record(self, kind, t_mac):
        if kind == "u" and self.start is None:
            self.start, self.plug = t_mac, None
        elif kind == "p" and self.start is not None:
            self.plug = t_mac
        elif kind == "M" and self.start is not None and (self.plug is None or t_mac <= self.plug + self.reenum_us):
            self._close(t_mac + UNPLUG_GRACE_US)
        else:
            self.passed(t_mac)

    def passed(self, t_mac):
        """Every record up to t_mac is in: an open window past its reenum limit closes."""
        if self.start is not None and self.plug is not None and t_mac > self.plug + self.reenum_us:
            self._close(self.plug + self.reenum_us + UNPLUG_GRACE_US)

    def _close(self, end):
        self.closed.append((self.start - UNPLUG_GRACE_US, end))
        self.start = self.plug = None

    def finish(self):
        if self.start is not None:
            self._close(self.plug + self.reenum_us + UNPLUG_GRACE_US if self.plug is not None else float("inf"))

    def covers(self, t_mac):
        """True / False, or None while an open window might still cover t_mac."""
        if any(a <= t_mac <= b for a, b in self.closed):
            return True
        if self.start is not None and self.start - UNPLUG_GRACE_US <= t_mac:
            return None
        return False

    def prune(self, before_mac):
        while self.closed and self.closed[0][1] < before_mac:
            self.closed.popleft()


class Checker:
    """The checks for one run, windowed: it takes records as the robots' logs are
    drained, checks them in Mac-time order once every robot's log and clock tie
    are past them, and keeps only what a later record could still change (a few
    seconds' worth). So an 8-hour run fits in memory. The checks are night 0's
    (accounted, loss, extra, order, latency, jitter, clock_jitter, stuck_notes,
    remounted), with the same numbers, plus a note-off release rule for hot-plug."""

    REORDER_US = 200_000   # M/U records are logged a poll after their time
    EST_KEEP = 4096        # recent latency estimates the match window comes from

    def __init__(self, scn):
        self.scn = scn
        self.lim = lim = scn.get("checks", {})
        self.names = list(scn["robots"])
        self.routes = scn.get("routes", [])
        self.reenum_us = ms_us(lim.get("reenum_ms_max", 2000))
        self.model = {n: ClockModel() for n in self.names}
        self.buf = {n: [] for n in self.names}          # (t, kind, pkt) not yet checked
        self.complete = {n: None for n in self.names}   # robot µs each log is drained through
        self.down = {n: Outages(self.reenum_us) for n in self.names}
        self.counts = {n: collections.Counter() for n in self.names}
        self.dest_cables = {(rt["to"]["robot"], rt["to"].get("cable", 0)) for rt in self.routes}
        self.listens = {(ls["robot"], ls.get("cable", 0)): ls for ls in scn.get("listen", [])}
        self.sources = collections.defaultdict(set)   # dest (robot, cable) -> source robots
        for rt in self.routes:
            self.sources[(rt["to"]["robot"], rt["to"].get("cable", 0))].add(rt["from"]["robot"])
        self.seq = collections.Counter()
        self.expect = collections.defaultdict(collections.deque)
        self.last_exp = {}
        self.est = collections.deque(maxlen=self.EST_KEEP)
        self.arrivals = collections.deque()           # (t_mac, t, robot, cable, msg) waiting to be matched
        self.missing = collections.deque()            # (dest robot, expectation) waiting for the outage windows
        self.ingested_mac = float("-inf")
        self.extras = self.lost = self.lost_unplugged = self.order_bad = self.released = self.late = 0
        self.cross = False
        self.lat = collections.Counter()
        self.last_seq = {}
        self.held = collections.Counter()
        self.held_on = collections.defaultdict(collections.deque)   # note-on times still held
        self.hold_max_us = 0
        self.clock_rx = {}      # (route, robot, cable) -> [last t, Counter of intervals, n]
        self.clock_tx = {}      # (robot, cable) -> [first t, last t, n]
        self.heard = {}         # (robot, cable) -> [n heard, first tick, last tick, ticks, Counter of intervals]
        self.plug_t = {n: None for n in self.names}
        self.plugs = 0
        self.reconnect = collections.Counter()
        self.typical, self.window = 0, 5000

    # --- feeding

    def sync(self, name, s):
        self.model[name].add(s)

    def records(self, name, recs, complete_us=None):
        self.buf[name] += recs
        if complete_us is not None:
            self.complete[name] = complete_us

    def advance(self):
        """Checks everything every robot is settled past; returns the Mac time reached."""
        horizon = []
        for n in self.names:
            m, c = self.model[n], self.complete[n]
            if m.known_through() is None or c is None:
                return self.ingested_mac
            horizon.append(m(min(c - self.REORDER_US, m.known_through())))
        self._ingest(min(horizon))
        return self.ingested_mac

    def finish(self, statuses):
        self._ingest(float("inf"))
        for n in self.names:
            self.down[n].finish()
        self._match(float("inf"))
        for k, q in self.expect.items():
            self.missing += [(k[0], e) for e in q]
        self.expect.clear()
        self._classify(final=True)
        return self._result(statuses)

    # --- the passes

    def _ingest(self, upto_mac):
        evs = []
        for i, n in enumerate(self.names):
            b = self.buf[n]
            if not b:
                continue
            b.sort(key=lambda r: r[1])
            to_mac = self.model[n]
            cut = len(b) if upto_mac == float("inf") else 0
            if not cut:
                while cut < len(b) and to_mac(b[cut][1]) <= upto_mac:
                    cut += 1
            for kind, t, pkt in b[:cut]:
                evs.append((to_mac(t), 0 if kind == "s" else 1, i, t, kind, pkt))
            del b[:cut]
        evs.sort(key=lambda e: e[:3])
        for t_mac, _, i, t, kind, pkt in evs:
            n = self.names[i]
            if t_mac < self.ingested_mac:
                self.late += 1
            self._event(n, t_mac, t, kind, pkt)
        if upto_mac != float("inf"):
            self.ingested_mac = max(self.ingested_mac, upto_mac)
            for n in self.names:
                self.down[n].passed(upto_mac)
            self._update_window()
            self._match(upto_mac - self.window + min(self.typical, 0))
            self._expire(upto_mac - self.window + min(self.typical, 0))
            self._classify()
            self._prune()
        elif evs:
            self.ingested_mac = max(self.ingested_mac, evs[-1][0])
        self._update_window()

    def _event(self, n, t_mac, t, kind, pkt):
        self.counts[n][kind] += 1
        if kind in "upMU":
            self.down[n].record(kind, t_mac)
            if kind == "u":
                # A receiver that leaves the bus lets go of what it held, as a synth would.
                for k in [k for k in self.held if k[0] == n]:
                    del self.held[k]
                for k in [k for k in self.held_on if k[0] == n]:
                    del self.held_on[k]
            if kind == "p":
                self.plugs += 1
                self.plug_t[n] = t
            elif kind == "M" and self.plug_t[n] is not None:
                if t - self.plug_t[n] <= self.reenum_us:
                    self.reconnect[round((t - self.plug_t[n]) / 1000, 1)] += 1
                self.plug_t[n] = None
            return
        cable = pkt[0] >> 4
        msg = pkt[1:1 + msg_len(pkt[1])]
        if kind == "s":
            if msg[0] == 0xF8:
                c = self.clock_tx.setdefault((n, cable), [t, t, 0])
                c[1], c[2] = t, c[2] + 1
            for ri, rt in enumerate(self.routes):
                src, dst = rt["from"], rt["to"]
                if src["robot"] != n or src.get("cable", 0) != cable or not passes(rt.get("filter"), msg):
                    continue
                key = (dst["robot"], dst.get("cable", 0), mapped(rt.get("map"), msg))
                self.expect[key].append({"route": ri, "seq": self.seq[ri], "t": t, "t_mac": t_mac, "src": n})
                self.last_exp[key] = t_mac
                self.seq[ri] += 1
            return
        if kind != "r":
            return
        ls = self.listens.get((n, cable))
        if ls and passes(ls.get("filter", {"types": ["clock", "transport"]}), msg):
            h = self.heard.setdefault((n, cable), [0, None, None, 0, collections.Counter()])
            h[0] += 1
            if msg[0] == 0xF8:
                if h[2] is not None:
                    h[4][t - h[2]] += 1
                if h[1] is None:
                    h[1] = t
                h[2], h[3] = t, h[3] + 1
            return
        if (n, cable) not in self.dest_cables:
            self.extras += 1
            return
        key = (n, cable, msg)
        if key in self.last_exp:
            self.est.append(t_mac - self.last_exp[key])
        self.arrivals.append((t_mac, t, n, cable, msg))
        # Stuck notes: what each receiver holds down, in arrival order.
        if kind_of(msg) == "note" and msg[0] < 0xA0:
            k = (n, cable, msg[0] & 0x0F, msg[1])
            if msg[0] >> 4 == 0x9 and msg[2] > 0:
                self.held[k] += 1
                self.held_on[k].append(t)
            elif self.held[k] > 0:
                self.held[k] -= 1
                self._hold(t - self.held_on[k].popleft())
        elif is_all_notes_off(msg):
            for k in [k for k in self.held if k[:3] == (n, cable, msg[0] & 0x0F)]:
                del self.held[k]
                for on in self.held_on.pop(k, ()):
                    self._hold(t - on)

    def _hold(self, us):
        """How long a receiver held a note down: a note the P4 failed to release
        shows here even when a later note-off for the same key ends it."""
        self.hold_max_us = max(self.hold_max_us, us)

    def _update_window(self):
        if self.est:
            self.typical = statistics.median(self.est)
            mad = statistics.median(abs(x - self.typical) for x in self.est)
        else:
            self.typical, mad = 0, 0
        self.window = ms_us(self.lim["match_window_ms"]) if "match_window_ms" in self.lim else max(5000, 8 * mad)

    def _match(self, upto_mac):
        """Each arrival takes the unmatched expectation nearest (arrival - typical
        latency); one further off than the window is an extra (a duplicate), and
        expectations passed over are lost. As night 0's checker."""
        while self.arrivals and self.arrivals[0][0] <= upto_mac:
            a = self.arrivals.popleft()
            t_mac, t, n, cable, msg = a
            k = (n, cable, msg)
            q = self.expect.get(k)
            if not q:
                self._extra(a)
                continue
            target = t_mac - self.typical
            j = 0
            while j + 1 < len(q) and abs(q[j + 1]["t_mac"] - target) < abs(q[j]["t_mac"] - target):
                j += 1
            if abs(q[j]["t_mac"] - target) > self.window:
                self._extra(a)
                continue
            for _ in range(j):
                self.missing.append((n, q.popleft()))
            e = q.popleft()
            same = e["src"] == n
            self.cross |= not same
            self.lat[(t - e["t"]) if same else round(t_mac - e["t_mac"])] += 1
            key = (e["route"], n, cable)
            if e["seq"] < self.last_seq.get(key, -1):
                self.order_bad += 1
            self.last_seq[key] = max(e["seq"], self.last_seq.get(key, -1))
            if msg[0] == 0xF8:
                c = self.clock_rx.setdefault(key, [None, collections.Counter(), 0])
                if c[0] is not None:
                    c[1][t - c[0]] += 1
                c[0], c[2] = t, c[2] + 1

    def _extra(self, a):
        """An arrival no route explains. A note-off (or all-notes-off) at a
        destination while one of its sources is unplugged is the P4 releasing
        what that source held: released, not extra."""
        t_mac, _, n, cable, msg = a
        if (is_note_off(msg) or is_all_notes_off(msg)) and any(
                self.down[s].covers(t_mac) is not False for s in self.sources[(n, cable)]):
            self.released += 1
        else:
            self.extras += 1

    def _expire(self, matched_through):
        """Expectations no later arrival can reach are lost (night 0 counted them
        at the end): an arrival after matched_through aims past them by more than
        the window."""
        before = matched_through - self.typical - self.window - 1
        for k, q in list(self.expect.items()):
            while q and q[0]["t_mac"] < before:
                self.missing.append((k[0], q.popleft()))
            if not q:
                del self.expect[k]

    def _classify(self, final=False):
        keep = collections.deque()
        while self.missing:
            dr, e = self.missing.popleft()
            if not final and e["t_mac"] + UNPLUG_GRACE_US >= self.ingested_mac:
                keep.append((dr, e))  # an unplug just after it may not be in yet
                continue
            v = [self.down[r].covers(e["t_mac"]) for r in {dr, e["src"]}]
            if True in v:
                self.lost_unplugged += 1
            elif None in v and not final:
                keep.append((dr, e))
            else:
                self.lost += 1
        self.missing = keep

    def _prune(self):
        oldest = min([e["t_mac"] for _, e in self.missing] + [self.ingested_mac - 60e6])
        for n in self.names:
            self.down[n].prune(oldest)
        for k in [k for k, t in self.last_exp.items() if t < self.ingested_mac - 60e6]:
            del self.last_exp[k]

    # --- the result

    def snapshot(self):
        """Numbers so far, for a long run's progress lines."""
        out = {"checked_to_s": round(self.ingested_mac / 1e6, 1) if self.ingested_mac > float("-inf") else None,
               "matched": sum(self.lat.values()), "lost": self.lost, "lost_unplugged": self.lost_unplugged,
               "extra": self.extras, "order": self.order_bad, "released": self.released,
               "held_now": sum(v for v in self.held.values() if v > 0),
               "note_hold_ms_max": round(self.hold_max_us / 1000, 3),
               "buffered": sum(len(b) for b in self.buf.values()) + len(self.arrivals) +
               sum(len(q) for q in self.expect.values())}
        if self.lat:
            out.update(latency_ms_p50=round(pct_counter(self.lat, 50) / 1000, 3),
                       latency_ms_max=round(max(self.lat) / 1000, 3))
        return out

    def _result(self, statuses):
        lim = self.lim
        checks, numbers = {}, {}

        def put(name, value, limit, ok):
            checks[name] = {"ok": ok, "value": value, "limit": limit}

        gaps = []
        for n in self.names:
            st = statuses[n]
            s, r = self.counts[n]["s"], self.counts[n]["r"]
            if s != st["sent"] or r != st["recv"] or st["log_lost_tx"] or st["log_lost_rx"]:
                gaps.append(f"{n}: sent {st['sent']} logged {s}, recv {st['recv']} logged {r}, "
                            f"log lost {st['log_lost_tx']}+{st['log_lost_rx']}")
        put("accounted", "; ".join(gaps) or "all", "all", not gaps)
        numbers["match_window_ms"] = round(self.window / 1000, 3)
        put("loss", self.lost, lim.get("loss_max", 0), self.lost <= lim.get("loss_max", 0))
        numbers["lost_unplugged"] = self.lost_unplugged
        put("extra", self.extras, lim.get("extra_max", 0), self.extras <= lim.get("extra_max", 0))
        if lim.get("order", True):
            put("order", self.order_bad, 0, self.order_bad == 0)
        numbers["matched"] = sum(self.lat.values())
        if self.lat:
            p50, p99 = (pct_counter(self.lat, p) / 1000 for p in (50, 99))
            numbers.update(latency_ms_p50=round(p50, 3), latency_ms_p99=round(p99, 3),
                           latency_ms_max=round(max(self.lat) / 1000, 3), latency_ms_min=round(min(self.lat) / 1000, 3),
                           jitter_ms=round(p99 - p50, 3))
            basis = round(max(self.model[n].rtt_max for n in self.names) / 2) if self.cross else 0
            numbers["latency_basis_us"] = basis
            if "latency_ms_max" in lim:
                # Across two robots the clock tie is only good to the basis: fail
                # only a latency over the limit by more than that.
                put("latency", numbers["latency_ms_max"], lim["latency_ms_max"],
                    numbers["latency_ms_max"] - basis / 1000 <= lim["latency_ms_max"])
            if "jitter_ms_max" in lim:
                put("jitter", numbers["jitter_ms"], lim["jitter_ms_max"], numbers["jitter_ms"] <= lim["jitter_ms_max"])
        worst = None
        for (ri, n, cable), (_, ivs, cnt) in self.clock_rx.items():
            src = self.routes[ri]["from"]
            tx = self.clock_tx.get((src["robot"], src.get("cable", 0)))
            if cnt < 3 or not tx or tx[2] < 3:
                continue
            nominal = (tx[1] - tx[0]) / (tx[2] - 1)
            worst = max(worst or 0, max(abs(v - nominal) for v in ivs) / 1000)
            numbers["clock_interval_ms_stdev"] = round(pstdev_counter(ivs) / 1000, 4)
            devs = collections.Counter()
            for v, c in ivs.items():
                devs[abs(v - nominal)] += c
            numbers[f"{n}{cable}_clock_dev_ms_p99"] = round(pct_counter(devs, 99) / 1000, 4)
        for key, (n_heard, first, last, ticks, ivs) in self.heard.items():
            ls = self.listens[key]
            numbers[f"{key[0]}{key[1]}_heard"] = n_heard
            if "clock" in ls and ticks >= 3:
                nominal = 60e6 / (ls["clock"]["bpm"] * 24)
                worst = max(worst or 0, max(abs(v - nominal) for v in ivs) / 1000)
                numbers[f"{key[0]}{key[1]}_clock_bpm"] = round(60e6 / ((last - first) / (ticks - 1)) / 24, 3)
                devs = collections.Counter()
                for v, c in ivs.items():
                    devs[abs(v - nominal)] += c
                numbers[f"{key[0]}{key[1]}_clock_dev_ms_p99"] = round(pct_counter(devs, 99) / 1000, 4)
                numbers[f"{key[0]}{key[1]}_clock_interval_ms_stdev"] = round(pstdev_counter(ivs) / 1000, 4)
                if "ticks_min" in ls["clock"]:
                    put(f"clock_ticks_{key[0]}{key[1]}", ticks, ls["clock"]["ticks_min"], ticks >= ls["clock"]["ticks_min"])
        if worst is not None:
            numbers["clock_jitter_ms"] = round(worst, 4)
            if "clock_jitter_ms_max" in lim:
                put("clock_jitter", round(worst, 4), lim["clock_jitter_ms_max"], worst <= lim["clock_jitter_ms_max"])
        if self.hold_max_us:
            numbers["note_hold_ms_max"] = round(self.hold_max_us / 1000, 3)
        if "note_hold_ms_max" in lim:
            put("note_hold", round(self.hold_max_us / 1000, 3), lim["note_hold_ms_max"],
                self.hold_max_us / 1000 <= lim["note_hold_ms_max"])
        stuck = sum(v for v in self.held.values() if v > 0)
        put("stuck_notes", stuck, lim.get("stuck_notes_max", 0), stuck <= lim.get("stuck_notes_max", 0))
        for n in self.names:
            numbers[f"{n}_not_sent"] = statuses[n]["not_sent"]
            numbers[f"{n}_unplugs"] = statuses[n]["unplugs"]
        nrec = sum(self.reconnect.values())
        if self.plugs:
            put("remounted", f"{nrec}/{self.plugs}", f"all, within {self.reenum_us // 1000} ms", nrec == self.plugs)
        if nrec:
            numbers.update(reconnect_ms_max=max(self.reconnect), reconnect_ms_p50=pct_counter(self.reconnect, 50),
                           reconnects=nrec)
        if self.released:
            numbers["released"] = self.released
        if self.late:
            numbers["late_records"] = self.late
        return {"ok": all(c["ok"] for c in checks.values()), "checks": checks, "numbers": numbers}


def check(scn, data):
    """The checks for one run held in memory (run(keep=True), a saved run, the
    tests): {"checks": {name: {ok, value, limit}}, "numbers": {...}, "ok": bool}.
    Night 0's sync model: the first and last time requests."""
    c = Checker(scn)
    for n, d in data.items():
        for s in (d["sync"][0], d["sync"][-1]):
            c.sync(n, s)
        c.records(n, list(d["records"]))
    return c.finish({n: d["status"] for n, d in data.items()})


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
        # 1 MB at a time, each retried, so one dropped transfer costs a megabyte.
        chunk = part.with_suffix(".chunk")
        with open(part, "wb") as out:
            for mb in range(int(size.group(1))):
                for attempt in range(3):
                    try:
                        # The ROM's own reader (--no-stub): the stub's reads over
                        # USB-Serial-JTAG stop at the same address every time on
                        # some images (Big-S3 at 0x14c000, 2026-11-16).
                        esptool(port, "--no-stub", "read-flash", str(mb << 20), str(1 << 20), str(chunk),
                                after="no-reset", timeout=180)
                        break
                    except RobotError:
                        if attempt == 2:
                            raise
                data = chunk.read_bytes()
                if len(data) != 1 << 20:
                    raise RobotError(f"{port}: MB {mb} read {len(data)} bytes")
                out.write(data)
        chunk.unlink()
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

def duration_s(v):
    """8h, 90m, 600s or 600 -> seconds."""
    m = re.fullmatch(r"(\d+(?:\.\d+)?)([hms]?)", v.strip())
    if not m:
        raise argparse.ArgumentTypeError(f"not a duration: {v}")
    return float(m.group(1)) * {"h": 3600, "m": 60, "s": 1, "": 1}[m.group(2)]


def run_checked(scn, robots, log=print, expect_macs=None, save=None, progress=None, **kw):
    """run() with the windowed Checker taking the records as they're drained, so
    nothing is held. --save streams the logs to a JSON-lines file as they come;
    --progress appends the numbers so far, so a crash at hour 7 keeps them."""
    checker = Checker(scn)
    out = open(save, "w") if save else None
    prog = open(progress, "a") if progress else None
    started = time.monotonic()

    def w(obj):
        if out:
            out.write(json.dumps(obj) + "\n")

    w({"scenario": scn})

    def on_sync(n, s):
        checker.sync(n, s)
        w({"robot": n, "sync": list(s)})

    def on_records(n, recs, complete):
        checker.records(n, recs, complete)
        if recs or complete is not None:
            w({"robot": n, "records": [[k, t, p.hex()] for k, t, p in recs], "complete": complete})
        checker.advance()

    def on_progress():
        snap = dict(checker.snapshot(), wall_s=round(time.monotonic() - started, 1), scenario=scn.get("name"))
        log("progress: " + ", ".join(f"{k}={v}" for k, v in snap.items()))
        if prog:
            prog.write(json.dumps(snap) + "\n")
            prog.flush()
        if out:
            out.flush()

    try:
        data = run(scn, robots, log=log, expect_macs=expect_macs, keep=False, on_sync=on_sync,
                   on_records=on_records, on_progress=on_progress, **kw)
        for n, d in data.items():
            w({"robot": n, "id": d["id"], "status": d["status"], "syncs_rejected": d["syncs_rejected"]})
        result = checker.finish({n: d["status"] for n, d in data.items()})
        for n, d in data.items():
            result["numbers"][f"{n}_syncs_rejected"] = d["syncs_rejected"]
        w({"result": result})
        if prog:
            prog.write(json.dumps(dict(checker.snapshot(), done=True, ok=result["ok"])) + "\n")
        return result, data
    finally:
        for rb in robots.values():
            rb.link.close()
        for f in (out, prog):
            if f:
                f.close()


def check_saved(path):
    """Re-checks a run saved by --save (JSON lines, streamed through the Checker)
    or by night 0's robot.py (one JSON object, held in memory)."""
    with open(path) as f:
        first = json.loads(f.readline())
        if "data" in first:
            data = {n: dict(d, records=[(k_, t, bytes.fromhex(p)) for k_, t, p in d["records"]])
                    for n, d in first["data"].items()}
            return first["scenario"], check(first["scenario"], data)
        scn = first["scenario"]
        checker, statuses = Checker(scn), {}
        for line in f:
            obj = json.loads(line)
            n = obj.get("robot")
            if "sync" in obj:
                checker.sync(n, tuple(obj["sync"]))
            elif "records" in obj:
                checker.records(n, [(k_, t, bytes.fromhex(p)) for k_, t, p in obj["records"]], obj["complete"])
                checker.advance()
            elif "status" in obj:
                statuses[n] = obj["status"]
        if set(statuses) != set(scn["robots"]):
            raise RobotError(f"{path}: the run didn't finish (no final status for every robot)")
        return scn, checker.finish(statuses)


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
    r.add_argument("--save", help="stream the run's logs and syncs here as JSON lines (robot.py check reads it)")
    r.add_argument("--duration", type=duration_s,
                   help="stretch the scenario to this long (8h, 90m, 600s): its pattern repeated, for a soak")
    r.add_argument("--repeat", type=int, help="play the scenario's pattern this many times back to back")
    r.add_argument("--sync-s", type=float, default=SYNC_S, help="seconds between clock ties to the robots")
    r.add_argument("--progress", help="append the numbers so far here as JSON lines, every --progress-s")
    r.add_argument("--progress-s", type=float, default=PROGRESS_S)
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
            scn, result = check_saved(a.saved)
            report(scn.get("name", a.saved), result)
            return 0 if result["ok"] else 1
        scn = json.loads(pathlib.Path(a.scenario).read_text())
        if a.repeat:
            scn = dict(scn, repeat=a.repeat)
        if a.duration:
            scn = with_duration(scn, a.duration)
        name = scn.get("name", pathlib.Path(a.scenario).stem)
        if a.sim:
            robots = sim_robots(scn, [o for s_ in a.sim_opt for o in s_.split("=", 1)])
        else:
            ports = kv(a.robot)
            missing = [n for n in scn["robots"] if n not in ports]
            if missing:
                raise RobotError(f"no --robot for {', '.join(missing)}")
            robots = {n: Robot(n, open_link(ports[n], a.baud)) for n in scn["robots"]}
        result, data = run_checked(scn, robots, expect_macs=kv(a.mac), save=a.save, progress=a.progress,
                                   sync_s=a.sync_s, progress_s=a.progress_s, poll_s=SIM_POLL_S if a.sim else POLL_S)
        report(name, result)
        line = {"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "scenario": name, "sim": a.sim,
                "robots": {n: d["id"] for n, d in data.items()}, **result}
        runs = pathlib.Path(a.runs)
        runs.parent.mkdir(parents=True, exist_ok=True)
        with runs.open("a") as f:
            f.write(json.dumps(line) + "\n")
        return 0 if result["ok"] else 1
    except RobotError as e:
        print(f"robot.py: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
