#!/usr/bin/env python3
"""Drives the FM-1 test bench from the Mac: a spare board running a BENCH=1
build (make build APPS=hub-loader DIAG_WEB=1 BENCH=1) wired to the bench FM-1.

usage:
  bench.py state                      what the board and its Firmware page say
  bench.py switch "Skiff 2.1.1"       switch firmware, watch the FM-1 come back, back it up
  bench.py backup                     back up the FM-1
  [--board waveshare-1] [--host fm1bench.local] [--runs build/bench/runs.jsonl] [--dry-run]

A switch presses what the screen would: it opens the row's plan, checks the
plan is for that firmware, then holds. The Loader decides keep or fresh start
itself, as it does for a person; the plan's lines go into the run's record.
Each job runs in boots of its own, so the board drops off Wi-Fi in between;
the runner waits it out and reads the result from /status.

Safety: the board answers /bench/ only on Chad's Waveshares (lib/web_core/src/bench.cpp),
and this script refuses any board but the one --board names, by its MAC. It
refuses FM-1-1 (Chad's main FM-1) by unit ID: the bench switches FM-1-2 only.
--dry-run talks to an in-process fake board and never opens a socket.
Standard library only. One JSON line per run goes to --runs.
"""
import argparse
import json
import pathlib
import sys
import time
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
WAVESHARES = {"waveshare-1": "d8:3b:da:a0:67:d0", "waveshare-2": "a0:85:e3:f0:a2:3c"}
FM1_1_UNIT = "69bb070aaa87bd10"  # FM-1-1, Chad's main FM-1: the bench never switches it
JOB_TIMEOUT_S = 20 * 60          # a switch chain is a backup, then an install: minutes each
COME_BACK_S = 60                 # the FM-1 must re-enumerate as MIDI within this
WATCH_S = 60                     # then stay put this long: a boot loop shows as disconnects


class BenchError(Exception):
    pass


class Board:
    """The bench board over HTTP. get() returns parsed JSON, or None while it's off Wi-Fi."""

    def __init__(self, host, timeout=8):
        self.host, self.timeout = host, timeout

    def _req(self, path, method):
        req = urllib.request.Request(f"http://{self.host}{path}", method=method, data=b"" if method == "POST" else None)
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            return json.loads(r.read())

    def get(self, path):
        try:
            return self._req(path, "GET")
        except (urllib.error.URLError, OSError, ValueError):
            return None

    def post(self, path):
        try:
            return self._req(path, "POST")
        except urllib.error.HTTPError as e:
            raise BenchError(f"POST {path}: {e.code} {e.read().decode(errors='replace')}")

    def sleep(self, s):
        time.sleep(s)

    def now(self):
        return time.monotonic()


def check_board(board, want):
    st = board.get("/bench/state")
    if st is None:
        raise BenchError(f"{board.host} doesn't answer /bench/state: not a bench build, or off Wi-Fi")
    mac = st.get("mac", "").lower()
    if want not in WAVESHARES or mac != WAVESHARES[want] or st.get("board") != want:
        raise BenchError(f"board {mac} says it is '{st.get('board')}', not {want}: refusing")
    return st


def fm1_devices(status):
    """The FM-1's faces on USB as the board sees them: (vid, pid, name) for each non-hub device."""
    return sorted((d["vid"], d["pid"], d.get("name", "")) for d in status.get("devices", []) if not d.get("hub"))


def fw(status):
    return status.get("firmware") or {}


def wait_status(board, until, what, timeout):
    """Polls /status until until(status) is true; None answers (the board restarting) are waited out."""
    end = board.now() + timeout
    while board.now() < end:
        s = board.get("/status")
        if s is not None and until(s):
            return s
        board.sleep(2)
    raise BenchError(f"timed out after {timeout} s waiting for {what}")


def idle_play(s):
    return s.get("usb", {}).get("mode") == "play" and not fw(s).get("busy") and not fw(s).get("unfinished")


def run_job(board, act_path, started_uptime):
    """Posts the hold, then waits for the job's last boot: a Play boot with a result,
    or a Load boot an install stopped in (unfinished: the FM-1 sits in ROM boot)."""
    t0 = board.now()
    board.post(act_path)

    def ended(s):
        f = fw(s)
        newer = s.get("uptime_ms", 0) / 1000 < board.now() - t0 + 1 and s.get("uptime_ms", 0) != started_uptime
        if not newer or f.get("busy"):
            return False
        return (idle_play(s) and (f.get("result") or {}).get("headline")) or f.get("unfinished")

    s = wait_status(board, ended, "the job to finish", JOB_TIMEOUT_S)
    return s, round(board.now() - t0)


def watch_fm1(board):
    """After a job: the FM-1 must come back on USB and stay. Returns (devices, changes)."""
    s = wait_status(board, lambda s: fm1_devices(s), "the FM-1 on USB", COME_BACK_S)
    first = fm1_devices(s)
    changes, last, end = 0, first, board.now() + WATCH_S
    while board.now() < end:
        board.sleep(2)
        s = board.get("/status")
        if s is None:
            continue
        now = fm1_devices(s)
        if now != last:
            changes += 1
            last = now
    return first, changes


def refuse_fm1_1(board):
    store = board.get("/store") or {}
    for u in store.get("units", []):
        if str(u.get("id", "")).startswith(FM1_1_UNIT):
            raise BenchError(f"the board's store knows unit {FM1_1_UNIT}: that is FM-1-1, refusing")


def do_backup(board, want):
    check_board(board, want)
    s = wait_status(board, idle_play, "an idle Play boot", 60)
    if not fw(s).get("can_back_up"):
        raise BenchError("the Firmware page can't back up now: " + json.dumps(fw(s).get("blocked")))
    s, secs = run_job(board, "/bench/act?kind=backup", s.get("uptime_ms"))
    refuse_fm1_1(board)
    return {"kind": "backup", "seconds": secs, "result": fw(s).get("result"), "lines": fw(s).get("lines"),
            "ok": bool((fw(s).get("result") or {}).get("ok")) and not fw(s).get("unfinished")}


def do_switch(board, label, want):
    check_board(board, want)
    refuse_fm1_1(board)
    s = wait_status(board, idle_play, "an idle Play boot", 60)
    labels = [i["label"] for i in fw(s).get("items", [])]
    if label not in labels:
        raise BenchError(f"'{label}' isn't in the board's library: {labels}")
    index = labels.index(label)
    before = {"running": next((i["label"] for i in fw(s)["items"] if i.get("note") == "running"), ""),
              "devices": fm1_devices(s)}
    board.post(f"/bench/select?index={index}")
    s = wait_status(board, lambda s: fw(s).get("plan", {}).get("item") == index, "the plan to open", 30)
    plan = fw(s)["plan"]
    if not plan.get("can_act"):
        raise BenchError(f"the plan for {label} can't be held: {plan.get('lines')}")
    if label not in plan.get("hold", ""):
        raise BenchError(f"the plan's hold says '{plan.get('hold')}', not {label}")
    s, secs = run_job(board, f"/bench/act?kind=switch&index={index}", s.get("uptime_ms"))
    rec = {"kind": "switch", "to": label, "before": before, "plan": plan["lines"], "seconds": secs,
           "result": fw(s).get("result"), "unfinished": fw(s).get("unfinished"), "title": fw(s).get("title")}
    if fw(s).get("unfinished"):
        rec["ok"] = False
        return rec
    rec["devices"], rec["usb_changes"] = watch_fm1(board)
    rec["after_backup"] = do_backup(board, want)
    running = next((i["label"] for i in fw(board.get("/status") or {}).get("items", []) if i.get("note") == "running"), "")
    rec["running"] = running
    rec["ok"] = (bool((rec["result"] or {}).get("ok")) and running == label and rec["usb_changes"] == 0
                 and rec["after_backup"]["ok"])
    return rec


def main(argv=None, board=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["state", "switch", "backup"])
    ap.add_argument("firmware", nargs="?", help="the library row to switch to, as the screen labels it")
    ap.add_argument("--board", default="waveshare-1", choices=sorted(WAVESHARES),
                    help="the Waveshare the run expects; any other board is refused")
    ap.add_argument("--host", default="fm1bench.local")
    ap.add_argument("--runs", default=str(ROOT / "build" / "bench" / "runs.jsonl"))
    ap.add_argument("--dry-run", action="store_true", help="a fake board in-process; no network")
    a = ap.parse_args(argv)
    if board is None:
        if a.dry_run:
            sys.path.insert(0, str(ROOT / "tests"))
            from bench_fake import FakeBoard
            board = FakeBoard()
        else:
            board = Board(a.host)
    try:
        if a.command == "state":
            rec = {"bench": check_board(board, a.board), "status": board.get("/status")}
            print(json.dumps(rec, indent=1))
            return 0
        if a.command == "switch":
            if not a.firmware:
                ap.error("switch needs a firmware label")
            rec = do_switch(board, a.firmware, a.board)
        else:
            rec = do_backup(board, a.board)
    except BenchError as e:
        rec = {"kind": a.command, "to": a.firmware, "ok": False, "error": str(e)}
    rec["at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    rec["dry_run"] = a.dry_run
    line = json.dumps(rec)
    print(line)
    if not a.dry_run:
        pathlib.Path(a.runs).parent.mkdir(parents=True, exist_ok=True)
        with open(a.runs, "a") as f:
            f.write(line + "\n")
    return 0 if rec.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
