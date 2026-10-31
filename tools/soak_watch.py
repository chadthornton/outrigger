#!/usr/bin/env python3
"""Watches a soak build (make build ... SOAK=1) from the Mac and keeps its evidence.

usage: soak_watch.py [--host fm1hub.local] [--out build/diag/soak-<time>]

Every few seconds it reads GET /crash: the RTC log tail, which survives the
board's job restarts and crashes. It saves every new log line to log.txt,
reports the soak's run count, and after each crash (the board waits 90 s in
safe mode) saves /crash and /coredump into crash-<n>/. Stops at "soak finished".
Read only: every request is a GET. Standard library only.
"""
import argparse
import importlib.util
import json
import os
import pathlib
import re
import sys
import time

# Makes the calls do nothing where claude-progress is not installed.
_bytecode, sys.dont_write_bytecode = sys.dont_write_bytecode, True
try:
    sys.path.insert(0, os.environ["CLAUDE_PROGRESS_DIR"])
    from claude_progress import claude_progress
except Exception:
    def claude_progress(*args):
        pass
finally:
    sys.dont_write_bytecode = _bytecode

ROOT = pathlib.Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("diag", ROOT / "tools" / "diag.py")
diag = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(diag)

RUN = re.compile(r"soak run (\d+): (.*)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="fm1hub.local")
    ap.add_argument("--out", default=str(ROOT / "build" / "diag" / time.strftime("soak-%Y%m%d-%H%M%S")))
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    log = (out / "log.txt").open("a")
    seen = []          # the tail as last read, to find what's new
    crashes = 0
    saved_crash = False
    total = 50
    while True:
        try:
            c = diag.get_json(a.host, "/crash")
        except Exception as e:
            time.sleep(3)
            continue
        tail = c.get("tail", [])
        # New lines: the tail is a ring, so find where the last read ends in it.
        start = 0
        for k in range(len(tail), 0, -1):
            if seen[-k:] == tail[:k]:
                start = k
                break
        for line in tail[start:]:
            log.write(time.strftime("%H:%M:%S ") + line + "\n")
            log.flush()
            m = RUN.search(line)
            if m:
                claude_progress("-n", "soak", f"{m.group(1)}/{total}", f"{m.group(2)}, crashes {crashes}")
            if "soak finished" in line:
                claude_progress("-n", "soak", "done", line.split(" L ", 1)[-1])
                print(line)
                return
        seen = tail
        if c.get("safe_mode") and not saved_crash:
            crashes += 1
            d = out / f"crash-{crashes}"
            d.mkdir(exist_ok=True)
            (d / "crash.json").write_text(json.dumps(c, indent=2))
            try:
                (d / "coredump.bin").write_bytes(diag.get(a.host, "/coredump", timeout=60))
            except Exception as e:
                (d / "coredump-error.txt").write_text(str(e))
            print(f"crash {crashes} saved to {d}", flush=True)
            saved_crash = True
        elif not c.get("safe_mode"):
            saved_crash = False
        time.sleep(3)


if __name__ == "__main__":
    main()
