#!/usr/bin/env python3
"""Drive the Vibe bench on Big-P4 over UART-C and summarise its VIBE lines.

    ~/.espressif/python_env/idf5.5_py3.13_env/bin/python -I firmware/vibe_p4/tools/vibe_console.py \\
        /dev/cu.usbmodem5CF70432541 CMDS TIMEOUT_S LOG

Opening UART-C resets Big-P4 (night 0), so this waits for "BENCH ready" before it sends anything. CMDS is a
string of bench letters sent one at a time, each after the previous one finished: i (info), m (memory),
b (block 128 <-> 64), B (the sweep; done at "BENCH sweep done"), s / f (one Vibe; done at "BENCH ... done").
Every line is printed and appended to LOG. At the end: one table row per VIBE line, and a check that every
interpreter and AOT row's hash matched the Mac's (match=yes). Needs pyserial (in ESP-IDF's Python env).
"""
import sys
import time

import serial

DONE = {"B": "BENCH sweep done", "s": "BENCH example-synth done", "f": "BENCH faust-synth done",
        "i": "BENCH module faust-synth", "m": "BENCH memory now", "b": "BENCH block"}


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    port, cmds, secs, logp = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
    log = open(logp, "a")
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False
    s.open()
    end, buf, rows, boots = time.time() + secs, b"", [], 0
    ready, queue, waiting = False, list(cmds), None
    while time.time() < end:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip("\r")
            print(text, flush=True)
            log.write(text + "\n")
            log.flush()
            if text.startswith("ESP-ROM"):
                boots += 1
            if text.startswith("BENCH ready"):
                ready = True
            if text.startswith("VIBE fw="):
                rows.append(dict(kv.split("=", 1) for kv in text.split()[1:] if "=" in kv))
            if waiting and text.startswith(DONE[waiting]):
                waiting = None
        if ready and waiting is None:
            if not queue:
                break
            waiting = queue.pop(0)
            time.sleep(0.2)
            s.write(waiting.encode())
    s.close()
    print("SUMMARY boots=%d rows=%d finished=%s" % (boots, len(rows), "yes" if waiting is None and not queue else "NO (timeout)"))
    bad = 0
    for r in rows:
        if r.get("match") == "NO":
            bad += 1
        print("ROW %-13s %-12s notes=%-2s block=%-3s match=%-3s us_held=%-8s us_max=%-8s held_pct=%-6s %s" % (
            r.get("fw"), r.get("be"), r.get("notes"), r.get("block"), r.get("match"), r.get("us_held"),
            r.get("us_max"), r.get("held_pct"), ("vs_interp_db=" + r["vs_interp_db"]) if "vs_interp_db" in r else ""))
    print("SUMMARY hash mismatches=%d (interp and aot rows must all be match=yes)" % bad)


if __name__ == "__main__":
    main()
