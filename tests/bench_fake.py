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
        self.controller = None      # None, "always" or "late": a Launchkey on the same hub
        self.fm1_gone = False       # the FM-1 never comes back after a job
        self.posts = []

    # The clock bench.py runs on.
    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s
        self._advance()

    def _advance(self):
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
        # A controller on the same hub, as the real board lists it (vid/pid as hex strings).
        ctl = [{"vid": "1235", "pid": "0142", "hub": False, "name": "Launchkey Mini MK4 37"}]
        if self.controller == "late" and self.t - self.boot_at < 30:
            ctl = []  # enumerates some seconds after the FM-1 (Big-S3, 2026-11-05)
        if not self.controller:
            ctl = []
        if self.job or self.unfinished:
            return [{"vid": 0x4C4A, "pid": 0x8057, "hub": False, "name": "ROM"}] if self.unfinished else ctl
        if self.boot_loops and int(self.t) % 8 < 4:
            return ctl
        if self.fm1_gone:
            return ctl
        return [{"vid": 0x4C4A, "pid": 0x4155, "hub": False, "name": "FM-1"}] + ctl

    def get(self, path):
        if self.t < self.offline_until:
            return None
        if path == "/bench/state":
            return {"board": self.board, "mac": self.mac, "seq": len(self.posts), "taken": len(self.posts),
                    "pending": False, "last": ""}
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
        return {"uptime_ms": int((self.t - self.boot_at) * 1000), "build": {"commit": "abc1234"},
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
