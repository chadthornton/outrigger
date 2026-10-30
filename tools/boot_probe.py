#!/usr/bin/env python3
"""Times how soon the hub answers HTTP after it boots, and which step is slow.

usage: boot_probe.py [--host fm1hub.local] [--seconds 120]

Start it, then reset the board (RST, or a job restart). Every half second it
tries two clients side by side and prints one line per attempt:

  resolve  getaddrinfo() for the host, how long it took and what it returned
  v4 / v6  a TCP connect plus GET /status to each address on its own, 1.5 s limit
  urllib   what tools/diag.py does (urllib, 10 s limit), the client that saw
           the ~20 s first answer

The board's uptime in the first good answer dates its boot, so the summary
says how long after boot each client first got an answer. Read only.
"""
import argparse
import http.client
import json
import socket
import sys
import threading
import time
import urllib.request


def timed(fn):
    t = time.monotonic()
    try:
        return fn(), time.monotonic() - t, None
    except Exception as e:  # noqa: BLE001 - every failure is a data point here
        return None, time.monotonic() - t, f"{type(e).__name__}: {e}"


def get_status(addr, family, host, port):
    """GET /status from one address, with the Host header the board accepts."""
    sock = socket.socket(family, socket.SOCK_STREAM)
    sock.settimeout(1.5)
    sock.connect(addr)  # a v6 address keeps its scope id (fe80::...%en0)
    conn = http.client.HTTPConnection(host, port, timeout=1.5)
    conn.sock = sock
    conn.request("GET", "/status", headers={"Host": host})
    body = conn.getresponse().read()
    conn.close()
    return json.loads(body)["uptime_ms"]


class Urllib(threading.Thread):
    """diag.py's client, in the background: one request after another."""

    def __init__(self, host, port):
        super().__init__(daemon=True)
        self.host, self.events = f"{host}:{port}" if port != 80 else host, []

    def run(self):
        while True:
            start = time.time()
            r, took, err = timed(lambda: json.loads(urllib.request.urlopen(
                f"http://{self.host}/status", timeout=10).read())["uptime_ms"])
            self.events.append((start, took, r, err))
            time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="fm1hub.local")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--port", type=int, default=80, help="for testing against a stub")
    a = ap.parse_args()
    t0 = time.time()
    lib = Urllib(a.host, a.port)
    lib.start()
    printed = 0
    boot_at = None      # wall time of the board's latest boot, from its uptime
    first = {}          # client -> wall time of its first answer after that boot
    last_uptime = None
    shown = ""
    while time.time() - t0 < a.seconds:
        now = time.time()
        addrs, took, err = timed(lambda: socket.getaddrinfo(a.host, a.port, 0, socket.SOCK_STREAM))
        fams = ",".join(("v6" if f == socket.AF_INET6 else "v4") for f, *_ in addrs or [])
        parts = [f"{now - t0:7.1f}s resolve {took * 1000:6.0f} ms {fams or err}"]
        for family, *_rest, addr in addrs or []:
            name = "v6" if family == socket.AF_INET6 else "v4"
            up, t, e = timed(lambda: get_status(addr, family, a.host, a.port))
            parts.append(f"{name} {t * 1000:5.0f} ms {('up ' + str(up)) if up is not None else e}")
            if up is not None:
                if last_uptime is None or up < last_uptime:  # a (new) boot
                    boot_at, first = now + t - up / 1000, {}
                last_uptime = up
                first.setdefault(name, now + t)
        while printed < len(lib.events):
            start, t, up, e = lib.events[printed]
            printed += 1
            parts.append(f"urllib {t * 1000:5.0f} ms {('up ' + str(up)) if up is not None else e}")
            if up is not None and boot_at and start + t >= boot_at:
                first.setdefault("urllib", start + t)
        print(" | ".join(parts), flush=True)
        if boot_at:
            done = ", ".join(f"{k} {v - boot_at:.1f} s" for k, v in sorted(first.items()))
            if done and done != shown:
                print(f"          first answer after boot: {done}", flush=True)
                shown = done
        time.sleep(max(0, 0.5 - (time.time() - now)))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
