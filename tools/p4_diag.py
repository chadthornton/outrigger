#!/usr/bin/env python3
"""Read Big-P4's diag page over Wi-Fi (E1): never touches the board's ports.

    python3 -I tools/p4_diag.py [--host p4hub.local] all OUTDIR
    python3 -I tools/p4_diag.py status | log [--since N] | crash | graph | usb
    python3 -I tools/p4_diag.py screen OUT.bmp
    python3 -I tools/p4_diag.py post /bench/replug | /bench/crash?kind=abort ...

`all` saves status.json, log.json, crash.json, graph.json, usb.json and
screen.bmp into OUTDIR and prints a one-line summary of each. The host's name
is resolved once (mDNS on macOS can take seconds); P4HUB_HOST overrides it.
Standard library only.
"""

import argparse
import json
import os
import socket
import sys
import time
import urllib.request

DEFAULT_HOST = os.environ.get("P4HUB_HOST", "p4hub.local")


def resolve(host):
    try:
        return socket.getaddrinfo(host, 80, socket.AF_INET)[0][4][0]
    except OSError as e:
        raise SystemExit(f"can't resolve {host}: {e}")


def get(addr, path, timeout=15, method="GET"):
    req = urllib.request.Request(f"http://{addr}{path}", method=method, data=b"" if method == "POST" else None)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def get_json(addr, path, **kw):
    return json.loads(get(addr, path, **kw))


def bmp_size(data):
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    w = int.from_bytes(data[18:22], "little", signed=True)
    h = int.from_bytes(data[22:26], "little", signed=True)
    return w, abs(h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("what")
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--since", type=int, default=0)
    a = ap.parse_args()
    addr = resolve(a.host)
    if a.what == "all":
        out = a.arg or "."
        os.makedirs(out, exist_ok=True)
        t0 = time.time()
        res = {}
        for name, path in (("status", "/status"), ("log", "/log?since=0"), ("crash", "/crash"), ("graph", "/graph"),
                           ("usb", "/usb")):
            d = get_json(addr, path)
            res[name] = d
            with open(os.path.join(out, f"{name}.json"), "w") as f:
                json.dump(d, f, indent=1)
        shot = get(addr, "/screen.bmp", timeout=30)
        with open(os.path.join(out, "screen.bmp"), "wb") as f:
            f.write(shot)
        s, l, c, g = res["status"], res["log"], res["crash"], res["graph"]
        print(f"host {a.host} ({addr}), read in {time.time() - t0:.1f} s")
        print(f"status: build {s['build']} up {s['uptime_ms'] / 1000:.1f} s boot {s['boot']} reset {s['reset_reason']} "
              f"heap {s['heap']['internal_free']} psram {s['heap']['psram_free']} usb {s['usb']['devices']} dev "
              f"ch {s['usb']['channels_used']}/{s['usb']['channels_max']}")
        print(f"log: {len(l['lines'])} lines, next {l['next']}, lost {l['lost']}")
        lc = c.get("last_crash")
        print(f"crash: this boot {c['this_boot']} reset {c['reset_reason']}; last crash boot {c['last_crash_boot']}"
              f" {lc.get('reset_reason') if lc else None} {lc.get('panic', {}).get('reason') if lc else ''};"
              f" coredump {'%d B task %s pc %s' % (c['coredump']['size'], c['coredump'].get('task'), c['coredump'].get('pc')) if c['coredump'] else None}")
        print(f"graph: {json.dumps(g)[:120]}")
        w, h = bmp_size(shot)
        print(f"screen: {w}x{h} BMP, {len(shot)} B")
        return
    if a.what == "screen":
        data = get(addr, "/screen.bmp", timeout=30)
        bmp_size(data)
        with open(a.arg or "screen.bmp", "wb") as f:
            f.write(data)
        print(f"{a.arg or 'screen.bmp'}: {len(data)} B")
        return
    if a.what == "post":
        print(json.dumps(get_json(addr, a.arg, method="POST")))
        return
    path = {"log": f"/log?since={a.since}"}.get(a.what, "/" + a.what)
    d = get_json(addr, path)
    if a.what == "log":
        for seq, ms, src, text in d["lines"]:
            print(seq, ms, src, text)
    else:
        print(json.dumps(d, indent=1))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as e:
        sys.exit(f"p4_diag: {e}")
