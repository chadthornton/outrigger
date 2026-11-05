#!/usr/bin/env python3
"""Drives the FM-1 test bench from the Mac: a spare board running a BENCH=1
build (make build APPS=hub-loader DIAG_WEB=1 BENCH=1) wired to the bench FM-1.

usage:
  bench.py fm1                        read only: the FM-1 on USB, its running firmware, the last result and backup
  bench.py state                      what the board and its Firmware page say
  bench.py switch "Skiff 2.1.1"       switch firmware, watch the FM-1 come back, back it up
  bench.py backup                     back up the FM-1
  bench.py update [--image app.bin] [--recovery recovery.bin]
                                      put a build on the board over Wi-Fi (docs/ota-updates.md)
  [--board Small-V2] [--fm1 FM-B] [--host small-v2.local] [--runs build/bench/runs.jsonl] [--dry-run]

A switch presses what the screen would: it opens the row's plan, checks the
plan is for that firmware, then holds. The Loader decides keep or fresh start
itself, as it does for a person; the plan's lines go into the run's record.
Each job runs in boots of its own, so the board drops off Wi-Fi in between;
the runner waits it out and reads the result from /status.

Safety: the board answers /bench/ only on the boards in the device registry (lib/web_core/src/bench.cpp).
This script refuses any board but the one --board names, by its MAC, and any
FM-1 but the one --fm1 names, by its unit ID, both from the device registry
(tools/devices.py): a check that the run has the devices it asked for. The
store's last unit is only the FM-1 it last backed up, so every switch starts
with a backup: that reads the FM-1 attached now. Switches and backups take a
lock (--lock, one file for the whole Mac, not per worktree), so two sessions
can't drive the board at once.

A switch's record carries its checks, each read from the board, never from a
person: result_ok (the result page says it worked), fm1_back (the FM-1
re-enumerated within 60 s), fm1_steady (no USB changes for the next 60 s),
backup_ok (the follow-up backup passed) and running (the board identifies the
backed-up app as the firmware asked for, by its hash). ok is all of them.
--dry-run talks to an in-process fake board and never opens a socket.
An update sends the image (default: make build APPS=hub-loader DIAG_WEB=1
BENCH=1's) to POST /bench/update. The board saves it in its store, restarts
through the recovery app, and the new build confirms itself; the run waits
for /status to show the image's commit, confirmed. A board without the
recovery app, or with another one than --recovery, gets that first (slot=recovery).

Standard library only. One JSON line per run goes to --runs.
"""
import argparse
import fcntl
import hashlib
import json
import pathlib
import sys
import time
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import devices  # noqa: E402
from build_site import read_build_tag  # noqa: E402
# Beside the device registry, outside every worktree's build/.
LOCK = devices.DEFAULT.parent / "bench.lock"
JOB_TIMEOUT_S = 20 * 60          # a switch chain is a backup, then an install: minutes each
REFUSED_S = 15                   # a hold the Loader accepts restarts the board within a second or two
COME_BACK_S = 60                 # the FM-1 must re-enumerate as MIDI within this
WATCH_S = 60                     # then stay put this long: a boot loop shows as disconnects
# Upload, the copy (about 20 s), and up to 3 trial boots of 120 s each before a rollback.
UPDATE_S = 10 * 60
UPLOAD_TIMEOUT_S = 180


def default_parts(board_tag):
    """make build APPS=hub-loader DIAG_WEB=1 BENCH=1's image and make recovery's, for the board profile with this tag_id."""
    import boards
    bid = next((b for b, p in boards.load()["boards"].items() if p.get("tag_id") == board_tag), None)
    if bid is None:
        raise BenchError(f"no board profile has tag_id {board_tag}: pass --image")
    return (ROOT / "build" / bid / "hub-loader-web-bench" / "waveshare_169.ino.bin",
            ROOT / "build" / bid / "recovery" / "recovery.ino.bin")


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
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise BenchError(f"POST {path}: {e}")

    def upload(self, path, data):
        req = urllib.request.Request(f"http://{self.host}{path}", method="POST", data=data,
                                     headers={"Content-Type": "application/octet-stream"})
        try:
            with urllib.request.urlopen(req, timeout=UPLOAD_TIMEOUT_S) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            raise BenchError(f"POST {path.split('?')[0]}: {e.code} {e.read().decode(errors='replace')}")
        except (urllib.error.URLError, OSError) as e:
            raise BenchError(f"POST {path.split('?')[0]}: {e}")
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise BenchError(f"POST {path}: {e}")

    def sleep(self, s):
        time.sleep(s)

    def now(self):
        return time.monotonic()


def check_board(board, want, reg):
    st = board.get("/bench/state")
    if st is None:
        raise BenchError(f"{board.host} doesn't answer /bench/state: not a bench build, or off Wi-Fi")
    try:
        d = reg.expect_esp32(want, st.get("mac", ""))
    except devices.DeviceError as e:
        raise BenchError(str(e)) from None
    if str(st.get("board", "")).lower() != d["label"].lower():
        raise BenchError(f"board {d['mac']} calls itself '{st.get('board')}', not {d['label']}: refusing")
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


def run_job(board, act_path, started_uptime, started_seq):
    """Posts the hold, then waits for the job's last boot: a Play boot with a result,
    or a Load boot an install stopped in (unfinished: the FM-1 sits in ROM boot).
    A hold the Loader refuses never restarts the board: its result shows on the
    same boot, and that ends the wait at once."""
    t0 = board.now()
    board.post(act_path)

    def refused(s):
        same_boot = s.get("uptime_ms", 0) / 1000 >= board.now() - t0
        r = fw(s).get("result") or {}
        return same_boot and not fw(s).get("busy") and r.get("seq") != started_seq and not r.get("ok")

    end = board.now() + REFUSED_S
    while board.now() < end:
        s = board.get("/status")
        if s is not None and refused(s):
            raise BenchError("the Loader refused the hold: " + json.dumps(fw(s).get("result")))
        board.sleep(2)

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


def check_fm1(board, want, reg):
    """The FM-1 the Loader last saw (the one attached) must be the one the run
    asked for. A board may hold other FM-1s' old backups; only the attached one counts."""
    store = board.get("/store")
    if store is None:
        raise BenchError("no /store answer: can't tell which FM-1 is attached")
    unit = str(store.get("last_unit", ""))
    try:
        reg.expect_fm1(want, unit)
    except devices.DeviceError as e:
        raise BenchError(str(e)) from None
    return unit


def running_label(status):
    """The library row the board marks running: it identifies the latest backup's
    app by hash (loader_app.cpp, known_unit), so it's read from the FM-1 itself."""
    return next((i["label"] for i in fw(status).get("items", []) if i.get("note") == "running"), "")


def do_backup(board, want, fm1, reg):
    check_board(board, want, reg)
    s = wait_status(board, idle_play, "an idle Play boot", 60)
    if not fw(s).get("can_back_up"):
        raise BenchError("the Firmware page can't back up now: " + json.dumps(fw(s).get("blocked")))
    s, secs = run_job(board, "/bench/act?kind=backup", s.get("uptime_ms"), (fw(s).get("result") or {}).get("seq"))
    unit = check_fm1(board, fm1, reg)
    return {"kind": "backup", "seconds": secs, "unit": unit, "result": fw(s).get("result"), "lines": fw(s).get("lines"),
            "running": running_label(s), "ok": bool((fw(s).get("result") or {}).get("ok")) and not fw(s).get("unfinished")}


def do_switch(board, label, want, fm1, reg):
    first = do_backup(board, want, fm1, reg)  # identifies the FM-1 attached now
    if not first["ok"]:
        raise BenchError("the backup before the switch failed: " + json.dumps(first.get("result")))
    s = wait_status(board, idle_play, "an idle Play boot", 60)
    labels = [i["label"] for i in fw(s).get("items", [])]
    if label not in labels:
        raise BenchError(f"'{label}' isn't in the board's library: {labels}")
    index = labels.index(label)
    before = {"running": running_label(s), "devices": fm1_devices(s), "unit": first["unit"]}
    board.post(f"/bench/select?index={index}")
    s = wait_status(board, lambda s: fw(s).get("plan", {}).get("item") == index, "the plan to open", 30)
    plan = fw(s)["plan"]
    if not plan.get("can_act"):
        raise BenchError(f"the plan for {label} can't be held: {plan.get('lines')}")
    if label not in plan.get("hold", ""):
        raise BenchError(f"the plan's hold says '{plan.get('hold')}', not {label}")
    s, secs = run_job(board, f"/bench/act?kind=switch&index={index}", s.get("uptime_ms"),
                      (fw(s).get("result") or {}).get("seq"))
    rec = {"kind": "switch", "to": label, "before": before, "plan": plan["lines"], "seconds": secs,
           "result": fw(s).get("result"), "unfinished": fw(s).get("unfinished"), "title": fw(s).get("title")}
    checks = rec["checks"] = {"result_ok": bool((rec["result"] or {}).get("ok")) and not fw(s).get("unfinished"),
                              "fm1_back": False, "fm1_steady": False, "backup_ok": False, "running": False}
    if fw(s).get("unfinished"):
        rec["ok"] = False
        return rec
    try:
        rec["devices"], rec["usb_changes"] = watch_fm1(board)
    except BenchError as e:
        rec["error"], rec["ok"] = str(e), False
        return rec
    checks["fm1_back"], checks["fm1_steady"] = True, rec["usb_changes"] == 0
    rec["after_backup"] = do_backup(board, want, fm1, reg)
    rec["running"] = rec["after_backup"]["running"]
    checks["backup_ok"] = rec["after_backup"]["ok"]
    checks["running"] = rec["running"] == label
    rec["ok"] = all(checks.values())
    return rec


def fm1_report(board, want, reg):
    """Read only: what an agent would otherwise ask someone to read off the screen."""
    st = check_board(board, want, reg)
    s = board.get("/status")
    if s is None:
        raise BenchError(f"{board.host} answered /bench/state but not /status")
    store = board.get("/store") or {}
    last = store.get("last_unit", "")
    unit = next((u for u in store.get("units", []) if u.get("id") == last), {})
    known = reg.by_unit(last) if last else None
    r = fw(s).get("result") or {}
    return {"kind": "fm1", "board": st.get("board"), "build": (s.get("build") or {}).get("commit"),
            "mode": (s.get("usb") or {}).get("mode"), "fm1_on_usb": fm1_devices(s), "running": running_label(s),
            "busy": bool(fw(s).get("busy")), "unfinished": bool(fw(s).get("unfinished")),
            "last_result": {"headline": r.get("headline"), "ok": r.get("ok"), "seq": r.get("seq")},
            "unit": last, "fm1": known["label"] if known else None, "installed": unit.get("installed"),
            "latest_backup": unit.get("latest"), "ok": True}


def update_info(status):
    return status.get("update") or {}


def image_digest(data):
    """What /status's update.recovery shows for an image: the SHA-256 ESP-IDF appends
    to it (its last 32 bytes, esp_partition_get_sha256), first 16 hex digits."""
    return data[-32:].hex()[:16]


def send_image(board, data, label, slot):
    sha = hashlib.sha256(data).hexdigest()
    ans = board.upload(f"/bench/update?sha256={sha}&label={label}&slot={slot}", data)
    if not ans.get("accepted"):
        raise BenchError(f"the board didn't accept the {slot} image: {ans}")
    return sha


def do_update(board, want, reg, image=None, recovery=None):
    """Sends a build (and, when the board needs it, the recovery app), then waits
    for the board to run it, confirmed. The record says what happened either way.
    image and recovery default to this worktree's builds for the board's profile;
    the recovery app is sent only when the board's differs."""
    check_board(board, want, reg)
    s0 = wait_status(board, lambda s: True, "the board's /status", 30)
    board_tag = s0.get("build", {}).get("board_tag")
    if image is None or recovery is None:
        if board_tag is None:
            raise BenchError("the board's build doesn't say its board profile: pass --image and --recovery")
        d_image, d_recovery = default_parts(board_tag)
        image = image or d_image
        if recovery is None and d_recovery.exists():
            recovery = d_recovery
    if not image.exists():
        raise BenchError(f"{image} doesn't exist: make build APPS=hub-loader DIAG_WEB=1 BENCH=1 (BOARD=...)")
    data = image.read_bytes()
    tag = read_build_tag(data)
    if tag is None:
        raise BenchError(f"{image} has no build tag: not an Outrigger app image")
    label = tag["commit"] + ("+" if tag["dirty"] else "")
    if board_tag is not None and tag["board"] != board_tag:
        raise BenchError(f"{image} is built for board profile {tag['board']}; the board runs profile {board_tag}")
    rec = {"kind": "update", "board": want, "from": s0.get("build", {}).get("commit"), "to": tag["commit"],
           "image": str(image), "image_sha256": hashlib.sha256(data).hexdigest()}
    if update_info(s0).get("pending_verify"):
        raise BenchError("the board's own image hasn't confirmed itself yet: wait for it")
    on_board = update_info(s0).get("recovery", "none")
    if recovery is not None:
        rdata = recovery.read_bytes()
        rsha = hashlib.sha256(rdata).hexdigest()
        rec["recovery_sha256"] = rsha
        want = image_digest(rdata)
        if on_board != want:
            send_image(board, rdata, "recovery", "recovery")
            on_board = update_info(wait_status(board, lambda s: True, "/status after the recovery app", 30)).get("recovery")
            if on_board != want:
                raise BenchError(f"the recovery slot reads {on_board} after writing {want}")
            rec["recovery_sent"] = True
    if on_board in (None, "none"):
        raise BenchError("the board has no recovery app: pass --recovery (make recovery builds it)")
    t0 = board.now()
    send_image(board, data, label, "app")
    rec["uploaded_s"] = round(board.now() - t0, 1)

    def settled(s):
        # Answers from the boot that took the upload don't count: a later boot is younger than the upload.
        if s.get("uptime_ms", 0) >= (board.now() - t0) * 1000:
            return False
        if "update" not in s:  # rolled back to a build from before updates
            return True
        u = update_info(s)
        return not u.get("pending_verify") and u.get("state") == "idle"

    s = wait_status(board, settled, f"the board to settle on {label} or roll back", UPDATE_S)
    u = update_info(s)
    rec.update({"result": u.get("result"), "attempts": u.get("attempts"), "running": s.get("build", {}).get("commit"),
                "took_s": round(board.now() - t0, 1)})
    rec["ok"] = u.get("result") == "updated" and rec["running"] == tag["commit"]
    if not rec["ok"]:
        crash = board.get("/crash") or {}
        rec["recovery_log"] = crash.get("recovery_log", [])
    return rec


def bench_lock(path):
    """One driver at a time on this Mac: a second switch or backup fails at once."""
    path.parent.mkdir(parents=True, exist_ok=True)
    f = open(path, "w")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        raise BenchError(f"another bench run holds {path}: wait for it")
    return f


def main(argv=None, board=None, reg=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["fm1", "state", "switch", "backup", "update"])
    ap.add_argument("firmware", nargs="?", help="the library row to switch to, as the screen labels it")
    ap.add_argument("--board", default="Small-V2", help="the board the run expects, by its registry label")
    ap.add_argument("--fm1", default="FM-B", help="the FM-1 the run expects, by its registry label")
    ap.add_argument("--devices", help="the device registry (default: tools/devices.py's)")
    ap.add_argument("--host", help="the board's address (default: --board's host in the registry)")
    ap.add_argument("--runs", default=str(ROOT / "build" / "bench" / "runs.jsonl"))
    ap.add_argument("--lock", default=str(LOCK), help="one driver at a time; the default is shared by every worktree")
    ap.add_argument("--image", help="update: the app image (default: the bench build for the board's profile)")
    ap.add_argument("--recovery", help="update: the recovery app, sent when the board's differs "
                                       "(default: make recovery's for the board's profile, if built)")
    ap.add_argument("--dry-run", action="store_true", help="a fake board in-process; no network")
    a = ap.parse_args(argv)
    try:
        if reg is None and a.dry_run and not a.devices:
            # The fake board is the example registry's Small-V2 with FM-B attached.
            reg = devices.load(devices.EXAMPLE)
            next(d for d in reg.entries if d["label"] == "FM-B")["unit"] = "0123456789abcdef"
        reg = reg or devices.load(a.devices)
    except devices.DeviceError as e:
        print(e, file=sys.stderr)
        return 2
    if board is None:
        if a.dry_run:
            sys.path.insert(0, str(ROOT / "tests"))
            from bench_fake import FakeBoard
            board = FakeBoard()
            board.refused = BenchError
        else:
            try:
                host = a.host or reg.find(a.board).get("host")
            except devices.DeviceError as e:
                print(e, file=sys.stderr)
                return 2
            if not host:
                print(f"{a.board} has no host in {reg.path}: pass --host", file=sys.stderr)
                return 2
            board = Board(host)
    if a.command == "switch" and not a.firmware:
        ap.error("switch needs a firmware label")
    lock = None
    try:
        if a.command == "state":
            rec = {"bench": check_board(board, a.board, reg), "status": board.get("/status")}
            print(json.dumps(rec, indent=1))
            return 0
        if a.command == "fm1":
            print(json.dumps(fm1_report(board, a.board, reg), indent=1))
            return 0
        if not a.dry_run:
            lock = bench_lock(pathlib.Path(a.lock))
        if a.command == "switch":
            rec = do_switch(board, a.firmware, a.board, a.fm1, reg)
        elif a.command == "update":
            rec = do_update(board, a.board, reg, pathlib.Path(a.image) if a.image else None,
                            pathlib.Path(a.recovery) if a.recovery else None)
        else:
            rec = do_backup(board, a.board, a.fm1, reg)
    except BenchError as e:
        rec = {"kind": a.command, "to": a.firmware, "ok": False, "error": str(e)}
        if a.command in ("state", "fm1"):
            print(json.dumps(rec))
            return 1
    finally:
        if lock:
            lock.close()
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
