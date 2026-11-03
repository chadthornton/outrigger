"""The test hardware registry: which board or FM-1 a session means by a name
(Small-V2, Big-S3, FM-B ...), and the check that the one plugged in is it.

The real file lives outside the public repo, at
~/Library/Application Support/outrigger/devices.json, or $OUTRIGGER_DEVICES;
tools/devices.example.json shows the shape with made-up identities. A tool asks
for a device by label and refuses anything else: a check that the session has
the device it asked for, not a list of precious ones.

    python3 -I tools/devices.py list
    python3 -I tools/devices.py mac <label>     # an ESP32's MAC, for scripts
Tests: tests/test_devices.py.
"""
import json
import os
import re
import sys
from pathlib import Path

DEFAULT = Path.home() / "Library/Application Support/outrigger/devices.json"
EXAMPLE = Path(__file__).resolve().parent / "devices.example.json"
KINDS = ("esp32", "fm1")


class DeviceError(Exception):
    pass


def norm_mac(mac):
    return str(mac or "").strip().lower().replace("-", ":")


class Registry:
    def __init__(self, entries, path):
        self.entries, self.path = entries, path

    def labels(self):
        return [d["label"] for d in self.entries]

    def find(self, label):
        for d in self.entries:
            if d["label"].lower() == str(label).lower():
                return d
        raise DeviceError(f"no device called {label!r} in {self.path}; known: {', '.join(self.labels())}")

    def by_mac(self, mac):
        mac = norm_mac(mac)
        return next((d for d in self.entries if d["kind"] == "esp32" and d["mac"] == mac), None)

    def by_unit(self, unit):
        unit = str(unit or "").lower()
        return next((d for d in self.entries if d["kind"] == "fm1" and d.get("unit") and unit.startswith(d["unit"])),
                    None)

    def _of_kind(self, label, kind):
        d = self.find(label)
        if d["kind"] != kind:
            raise DeviceError(f"{d['label']} is an {d['kind']}, not an {kind}")
        return d

    def expect_esp32(self, label, mac):
        """Refuses unless the board answering with this MAC is the one called label."""
        d = self._of_kind(label, "esp32")
        if norm_mac(mac) != d["mac"]:
            other = self.by_mac(mac)
            raise DeviceError(f"asked for {d['label']} ({d['mac']}), but this board is "
                              f"{other['label'] if other else 'not in the registry'} ({norm_mac(mac) or 'no MAC read'}): refusing")
        return d

    def expect_fm1(self, label, unit):
        """Refuses unless the attached FM-1's unit ID is the one called label's."""
        d = self._of_kind(label, "fm1")
        unit = str(unit or "").lower()
        if not d.get("unit"):
            raise DeviceError(f"{d['label']}'s unit isn't recorded yet; the attached FM-1 is unit {unit or '(unknown)'}. "
                              f"If that is {d['label']}, add it to {self.path} and run again")
        if not unit.startswith(d["unit"]):
            other = self.by_unit(unit)
            raise DeviceError(f"asked for {d['label']} (unit {d['unit']}), but the attached FM-1 is "
                              f"{other['label'] if other else 'not in the registry'} (unit {unit or 'unknown'}): refusing")
        return d


def load(path=None):
    path = Path(path or os.environ.get("OUTRIGGER_DEVICES") or DEFAULT)
    if not path.is_file():
        raise DeviceError(f"no device registry at {path}: copy {EXAMPLE} there and fill in the real MACs and units")
    try:
        data = json.loads(path.read_text())
    except ValueError as e:
        raise DeviceError(f"{path} isn't valid JSON: {e}") from None
    entries, seen = [], set()
    for d in data.get("devices", []):
        label, kind = d.get("label"), d.get("kind")
        if not label or kind not in KINDS:
            raise DeviceError(f"{path}: each device needs a label and a kind ({', '.join(KINDS)}): {d}")
        if label.lower() in seen:
            raise DeviceError(f"{path}: {label} is listed twice")
        seen.add(label.lower())
        d = dict(d)
        if kind == "esp32":
            d["mac"] = norm_mac(d.get("mac"))
            if not re.fullmatch(r"([0-9a-f]{2}:){5}[0-9a-f]{2}", d["mac"]):
                raise DeviceError(f"{path}: {label}'s mac {d.get('mac')!r} isn't a MAC")
        elif d.get("unit") is not None:
            d["unit"] = str(d["unit"]).lower()
            if not re.fullmatch(r"[0-9a-f]{16}", d["unit"]):
                raise DeviceError(f"{path}: {label}'s unit {d['unit']!r} isn't 16 hex digits (null if unread)")
        entries.append(d)
    return Registry(entries, path)


def main(argv):
    try:
        reg = load()
        if argv[:1] == ["list"] or not argv:
            for d in reg.entries:
                ident = d["mac"] if d["kind"] == "esp32" else (d.get("unit") or "unit not read")
                print(f"{d['label']:10} {d['kind']:6} {ident:18} {d.get('role', '')}")
        elif argv[0] == "mac" and len(argv) == 2:
            print(reg._of_kind(argv[1], "esp32")["mac"])
        else:
            sys.exit(__doc__)
    except DeviceError as e:
        sys.exit(str(e))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
