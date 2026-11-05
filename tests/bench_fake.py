"""An in-process bench board for tools/bench.py's tests and --dry-run.

It answers /bench/state, /status and /store as a BENCH=1 build does, and plays
a job out over simulated time: the hold restarts the board (it answers nothing
for a while), the job runs in a Load boot, then a Play boot shows the result.
Knobs on the instance make it fail the ways the real board can.
"""


class FakeBoard:
    # Labels as the real store names them (Small-V2's /status, 2026-11-01). The
    # board and FM-1 are tools/devices.example.json's Small-V2 and FM-B.
    def __init__(self, labels=("M-VAVE stock V15", "Felucca 1.8.0", "Skiff 2.1.1"), running="M-VAVE stock V15"):
        self.host = "fake"
        self.t = 0.0
        self.labels = list(labels)
        self.running = running
        self.mac, self.board = "02:00:00:00:00:02", "small-v2"
        self.units = [{"id": "0123456789abcdef"}]
        self.last_unit = "0123456789abcdef"  # the FM-1 attached
        self.refuse_holds = False   # the Loader refuses the hold: no restart, a result on the same boot
        self.selected = -1
        self.boot_at = 0.0
        self.offline_until = 0.0
        self.job = None            # (kind, label, ends_at)
        self.result = {"headline": "", "seq": 0}
        self.title = ""
        self.unfinished = False
        # Failure knobs.
        self.install_stops = False  # the install stops part-way: the FM-1 stays in ROM boot
        self.boot_loops = False     # the new firmware keeps re-enumerating
        self.plan_blocked = False   # the plan can't be held
        self.posts = []
        # Updates (POST /bench/update): the commit it runs, its recovery app, and how the next update ends.
        self.commit = "abc1234"
        self.recovery = "none"
        self.update = {"state": "idle", "result": "none", "attempts": 0, "label": "", "pending_verify": False}
        self.update_ends = "updated"  # or "rolled-back"
        self.uploads = []
        self.pending_update = None  # (label, ends_at)
        self.refused = RuntimeError  # what Board.upload raises on a 4xx: bench.py's BenchError, set by its user

    # The clock bench.py runs on.
    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s
        self._advance()

    def _advance(self):
        if self.pending_update and self.t >= self.pending_update[1]:
            label, _ = self.pending_update
            self.pending_update = None
            self.boot_at = self.t
            ok = self.update_ends == "updated"
            if ok:
                self.commit = label.rstrip("+")
            self.update = {"state": "idle", "result": self.update_ends, "attempts": 1 if ok else 3, "label": label,
                           "pending_verify": False}
        if self.job and self.t >= self.job[2]:
            kind, label, _ = self.job
            self.job = None
            self.boot_at = self.t
            self.offline_until = self.t + 20  # Wi-Fi's first answer after a boot
            if kind == "switch" and self.install_stops:
                self.unfinished = True
                self.title = "Stopped: write failed"
                self.result = {"headline": "Install unfinished", "ok": False, "seq": 1}
                return
            if kind == "switch":
                self.running = label
                self.title = f"Installed: 60 sectors written"
                self.result = {"headline": "Switched", "ok": True, "seq": 1}
            else:
                self.title = "Backed up. Nothing written."
                self.result = {"headline": "Backed up", "ok": True, "seq": 1}

    def _devices(self):
        if self.job or self.unfinished:
            return [{"vid": 0x4C4A, "pid": 0x8057, "hub": False, "name": "ROM"}] if self.unfinished else []
        if self.boot_loops and int(self.t) % 8 < 4:
            return []
        return [{"vid": 0x4C4A, "pid": 0x4155, "hub": False, "name": "FM-1"}]

    def get(self, path):
        if self.t < self.offline_until:
            return None
        if path == "/bench/state":
            return {"board": self.board, "mac": self.mac, "seq": len(self.posts), "taken": len(self.posts),
                    "pending": False, "last": ""}
        if path == "/crash":
            return {"recovery_log": ["recovery: trial boot 3 of 3"] if self.update_ends != "updated" else []}
        if path == "/store":
            return {"last_unit": self.last_unit, "units": self.units}
        if path != "/status":
            return None
        items = [{"label": l, "note": "running" if l == self.running else ""} for l in self.labels]
        plan = {"item": -1, "lines": [], "hold": "", "can_act": False}
        if 0 <= self.selected < len(self.labels):
            l = self.labels[self.selected]
            plan = {"item": self.selected, "lines": [f"Switches from {self.running}.", "Keeps your data."],
                    "hold": f"switch to {l}", "can_act": not self.plan_blocked and l != self.running}
        return {"uptime_ms": int((self.t - self.boot_at) * 1000), "build": {"commit": self.commit, "board_tag": 0},
                "update": dict(self.update, recovery=self.recovery),
                "usb": {"running": True, "mode": "load" if self.job else "play"},
                "devices": self._devices(),
                "firmware": {"title": self.title, "lines": [""] * 6, "items": items, "plan": plan,
                             "result": self.result, "blocked": {"headline": ""},
                             "unfinished": self.unfinished, "can_back_up": not self.unfinished,
                             "can_retry": self.unfinished, "percent": 0, "busy": bool(self.job),
                             "danger": self.unfinished}}

    def post(self, path):
        self.posts.append(path)
        if path.startswith("/bench/select?index="):
            self.selected = int(path.split("=")[1])
        elif path.startswith("/bench/act") and self.refuse_holds:
            self.result = {"headline": "Nothing started", "ok": False, "seq": self.result.get("seq", 0) + 1}
        elif path.startswith("/bench/act?kind=switch"):
            label = self.labels[int(path.split("index=")[1])]
            self.job = ("switch", label, self.t + 180)
            self.offline_until = self.t + 200
            self.selected = -1
        elif path == "/bench/act?kind=backup":
            self.job = ("backup", "", self.t + 60)
            self.offline_until = self.t + 80
        return {"seq": len(self.posts)}

    def upload(self, path, data):
        import hashlib
        from urllib.parse import parse_qs, urlsplit
        q = {k: v[0] for k, v in parse_qs(urlsplit(path).query).items()}
        self.uploads.append((q.get("slot"), len(data)))
        if hashlib.sha256(data).hexdigest() != q.get("sha256"):
            raise AssertionError("bench.py sent a sha256 that doesn't match the body")
        if q.get("slot") == "recovery":
            self.recovery = q["sha256"][:16]
            return {"accepted": True, "slot": "recovery", "restarting": False}
        if self.recovery == "none":
            raise self.refused("POST /bench/update: 409 no recovery app: send one first (slot=recovery)")
        self.t += len(data) / 200000  # about 200 KB/s over Wi-Fi
        self.pending_update = (q["label"], self.t + 45)  # prev saved, recovery copies, the new build boots and confirms
        self.offline_until = self.t + 45 + 15
        return {"accepted": True, "slot": "app", "restarting": True}
