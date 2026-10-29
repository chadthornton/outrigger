#!/usr/bin/env python3
"""Reads the hub's web diagnostics (firmware/waveshare_169/web.cpp) from the Mac.

usage: diag.py [--host fm1hub.local] status|log|crash|store|coredump|all [--follow]

  status    build, heap, tasks' stack, USB devices and refusals, the Firmware page
  log       the log lines; --follow keeps polling every second
  crash     the last abnormal reset's breadcrumbs, and whether a core dump is stored
  store     units, backups, saves and packages in the Loader's store
  coredump  saves the raw coredump partition and prints the esp-coredump command
  all       saves all of the above into build/diag/<time>/

Read only: every request is a GET. Standard library only.
"""
import argparse
import datetime
import json
import pathlib
import sys
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
# The settings page's app-set label -> the build directory `make build` uses.
BUILD_DIRS = {"hub": "hub", "hub+W": "hub-web", "hub+L": "hub-loader", "hub+LW": "hub-loader-web",
              "all": "everything"}


def get(host, path, timeout=10):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
        return r.read()


def get_json(host, path):
    return json.loads(get(host, path))


def kb(n):
    return f"{n / 1024:.0f} KB"


def print_status(s):
    b, h = s["build"], s["heap"]
    print(f"commit {b['commit']}{'+' if b['dirty'] else ''} {b['apps']}  writes {'ON' if b['writes'] else 'off'}"
          f"{'  SAFE MODE' if b['safe_mode'] else ''}  up {s['uptime_ms'] / 1000:.0f} s")
    print(f"internal {kb(h['internal_free'])} (min {kb(h['internal_min'])}, largest {kb(h['internal_largest'])})  "
          f"DMA {kb(h['dma_free'])} (min {kb(h['dma_min'])}, largest {kb(h['dma_largest'])})  "
          f"PSRAM {kb(h['psram_free'])} (min {kb(h['psram_min'])})")
    n = s["net"]
    print(f"net {n['ip']} {n['ssid']} {n['rssi']} dBm{'  hotspot on' if n['hotspot'] else ''}  "
          f"log dropped {s['log']['dropped']}")
    c = s["crumbs"]
    print(f"now: loop {c['loop']} | usb {c['usb']} | loader {c['loader']}")
    print("tasks (stack never used):")
    for t in sorted(s["tasks"], key=lambda t: t["stack_free"]):
        print(f"  {t['name']:<16} {t['stack_free']:>6} B  prio {t['prio']}")
    u = s["usb"]
    print(f"USB {'on' if u['running'] else 'OFF'}, {u['mode']} mode, {u['channels']}/8 channels, sent {u['sent']}, "
          f"blocked {u['blocked']}, failed {u['failed_sends']}; hub {u['hub']}")
    if u["problem"]:
        print("  problem: " + " / ".join(u["problem"]))
    for d in s["devices"]:
        refusal = " / ".join(x for x in d["refusal"] if x)
        print(f"  #{d['addr']} {d['vid']}:{d['pid']} {d['name']}{' (hub)' if d['hub'] else ''}: "
              f"{d['channels']} ch, {d['claimed']}/{d['interfaces']} interfaces claimed"
              f"{'  REFUSED ' + refusal if refusal else ''}")
    print(f"  FM-1 {u['fm1_in']}; {u['fm1_own']}")
    f = s.get("firmware")
    if f:
        busy = f"  [busy {f['percent']}%]" if f["busy"] else ""
        print(f"Firmware page: {f['title']}{busy}")
        for line in f["lines"]:
            if line:
                print(f"  {line}")
        for i, it in enumerate(f["items"]):
            print(f"  {'>' if i == f['selected'] else ' '}{'*' if it['pinned'] else ' '} {it['label']}  {it['note']}")
        print(f"  top row: {f.get('top') or 'Back up this FM-1'}")
        print(f"  preview: {' / '.join(f['preview'])}; action: {f['action']}{'' if f['can_act'] else ' (off)'}")


def log_lines(host, since):
    """Every line after `since`, across as many pages as the server needs."""
    out = []
    while True:
        r = get_json(host, f"/log?since={since}")
        if r["lost"]:
            out.append(f"... {r['lost']} lines lost")
        out += [f"{ms / 1000:10.3f} {src} {text}" for _, ms, src, text in r["lines"]]
        since = r["next"] - 1
        if not r["lines"]:
            return out, since


def print_log(host, follow):
    since = 0
    while True:
        lines, since = log_lines(host, since)
        for line in lines:
            print(line, flush=True)
        if not follow:
            return
        time.sleep(1)


def save_coredump(host, out_dir, status):
    data = get(host, "/coredump", timeout=30)
    commit = status["build"]["commit"] + ("+" if status["build"]["dirty"] else "")
    out = out_dir / f"coredump-{commit}.bin"
    out.write_bytes(data)
    if data[:4] == b"\xff\xff\xff\xff":
        print(f"coredump: partition is blank (no dump stored); saved anyway to {out}")
        return
    elf = ROOT / "build" / "waveshare_169" / BUILD_DIRS.get(status["build"]["apps"], "?") / "waveshare_169.ino.elf"
    print(f"coredump: {len(data)} bytes saved to {out}")
    print(f"  It decodes only against the ELF of commit {commit} (build dir {elf.parent.name}).")
    print(f"  esp-coredump info_corefile --core-format raw --core {out} {elf}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["status", "log", "crash", "store", "coredump", "all"])
    ap.add_argument("--host", default="fm1hub.local")
    ap.add_argument("--follow", action="store_true", help="log: keep polling")
    ap.add_argument("--json", action="store_true", help="print the raw JSON")
    a = ap.parse_args()

    if a.what == "log":
        print_log(a.host, a.follow)
        return
    if a.what in ("status", "crash", "store") and a.json:
        print(json.dumps(get_json(a.host, "/" + a.what), indent=2))
        return
    if a.what == "status":
        print_status(get_json(a.host, "/status"))
    elif a.what == "crash":
        c = get_json(a.host, "/crash")
        print(f"reset: {c['reset_reason']}{'  (safe mode now)' if c['safe_mode'] else ''}")
        for line in c["lines"]:
            if line:
                print(f"  {line}")
        if c.get("tail"):
            print("last log lines (kept across resets; \"boot:\" starts each boot):")
            for line in c["tail"]:
                print(f"  {line}")
        d = c["coredump"]
        print(f"core dump: {str(d['size']) + ' bytes' if d['present'] else 'none'}")
    elif a.what == "store":
        print(json.dumps(get_json(a.host, "/store"), indent=2))
    else:
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        out_dir = ROOT / "build" / "diag" / stamp
        out_dir.mkdir(parents=True, exist_ok=True)
        status = get_json(a.host, "/status")
        if a.what == "all":
            (out_dir / "status.json").write_text(json.dumps(status, indent=2))
            for name in ("crash", "store"):
                (out_dir / f"{name}.json").write_text(json.dumps(get_json(a.host, "/" + name), indent=2))
            lines, _ = log_lines(a.host, 0)
            (out_dir / "log.txt").write_text("".join(line + "\n" for line in lines))
            print_status(status)
            print(f"saved status, crash, store and log to {out_dir}")
        save_coredump(a.host, out_dir, status)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as e:
        sys.exit(f"diag: {e}")
