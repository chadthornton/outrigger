"""midi.guide CSVs -> Outrigger device profiles (JSON).

midi.guide (https://github.com/pencilresearch/midi, https://midi.guide) is Pencil Research's
CC-BY-SA 4.0 dataset of MIDI implementations: one CSV per device (CCs and NRPNs) and, for drum
machines, an optional `<Device>.triggers.csv` (which note plays which sound). This turns each device
into one profile: its parameters by section, value names parsed from the usage column, the trigger
notes, a USB match block (empty unless tools/profiles/usb_matches.json knows it) and the licence.

    python3 -I tools/profiles/midiguide.py SRC OUT [--commit SHA]

SRC is a checkout of pencilresearch/midi, OUT a folder that gets `<Manufacturer>/<Device>.json` per
device plus `report.json` (every file: imported, imported with warnings, skipped or failed, and why).
Library use: parse_usage(), import_device(), import_tree().

Profiles made from midi.guide stay CC-BY-SA 4.0 (ShareAlike) and credit it: see licence_block().
"""
import argparse
import csv
import io
import json
import re
import subprocess
import sys
import unicodedata
from pathlib import Path

SCHEMA = "outrigger.profile/1"   # /1 adds optional fields only (schema.py); this importer writes none of them
SOURCE_REPO = "https://github.com/pencilresearch/midi"

CC_COLUMNS = (
    "manufacturer", "device", "section", "parameter_name", "parameter_description",
    "cc_msb", "cc_lsb", "cc_min_value", "cc_max_value", "cc_default_value",
    "nrpn_msb", "nrpn_lsb", "nrpn_min_value", "nrpn_max_value", "nrpn_default_value",
    "orientation", "notes", "usage",
)
TRIGGER_COLUMNS = (
    "manufacturer", "device", "section", "sound_name", "sound_description", "midi_channel",
    "note_number_min_value", "note_number_max_value", "note_number_default_value",
    "comments", "velocity_usage",
)
ORIENTATIONS = ("0-based", "centered")

# A usage segment: `5: Saw` (one value), `0-63: Off` (a range whose values all do the same thing),
# `0~127: Cutoff` (a range where every value differs). CONTRIBUTING.md, "Usage".
_SEGMENT = re.compile(r"(\d+)\s*(?:([-~])\s*(\d+))?\s*:\s*(.*)", re.S)
KIND = {None: "value", "-": "switch", "~": "continuous"}


class Problems:
    """Warnings collected while importing one file, each tied to a CSV line when there is one."""

    def __init__(self):
        self.items = []

    def add(self, code, message, line=None):
        self.items.append({"code": code, "line": line, "message": message})


def parse_usage(text, lo=None, hi=None, problems=None, line=None):
    """The usage column -> a list of segments {from, to, kind, label}.

    kind is "value" (a single number), "switch" (a dash range: every value in it does the same
    thing) or "continuous" (a tilde range: every value differs). A "value" or "switch" segment
    is a value name (MicroFreak Type `0-11: Basic Waves`); a "continuous" one labels a sweep.
    lo / hi are the parameter's range, for the range checks. Bad segments are reported to
    `problems` and left out; an empty usage is [].
    """
    segments = []
    if not text or not text.strip():
        return segments
    for raw in text.split(";"):
        seg = raw.strip()
        if not seg:
            continue                      # a trailing or doubled ';'
        m = _SEGMENT.fullmatch(seg)
        if not m:
            if problems is not None:
                problems.add("usage-unparsed", f"usage segment {seg!r} is not `N: x`, `N-M: x` or `N~M: x`", line)
            continue
        a = int(m.group(1))
        b = int(m.group(3)) if m.group(3) is not None else a
        label = " ".join(m.group(4).split())
        if problems is not None:
            if not label:
                problems.add("usage-no-label", f"usage segment {seg!r} has no label", line)
            if b < a:
                problems.add("usage-reversed", f"usage segment {seg!r} runs backwards", line)
            if (lo is not None and min(a, b) < lo) or (hi is not None and max(a, b) > hi):
                problems.add("usage-out-of-range", f"usage segment {seg!r} is outside {lo}..{hi}", line)
        segments.append({"from": a, "to": b, "kind": KIND[m.group(2)], "label": label})
    if problems is not None:
        spans = sorted((s["from"], s["to"]) for s in segments)
        for (a1, b1), (a2, b2) in zip(spans, spans[1:]):
            if a2 <= b1:
                problems.add("usage-overlap", f"usage ranges {a1}..{b1} and {a2}..{b2} overlap", line)
    return segments


def value_names(usage):
    """The segments that name values (not continuous sweeps)."""
    return [s for s in usage if s["kind"] != "continuous"]


def _int(row, key, problems, line, lo=None, hi=None):
    v = (row.get(key) or "").strip()
    if not v:
        return None
    if not re.fullmatch(r"-?\d+", v):
        problems.add("not-a-number", f"{key} is {v!r}", line)
        return None
    n = int(v)
    if (lo is not None and n < lo) or (hi is not None and n > hi):
        problems.add("number-out-of-range", f"{key} {n} is outside {lo}..{hi}", line)
    return n


def _range_checks(kind, block, problems, line):
    lo, hi, d = block["min"], block["max"], block["default"]
    if lo is not None and hi is not None and lo > hi:
        problems.add("min-above-max", f"{kind} min {lo} is above max {hi}", line)
    if d is not None and ((lo is not None and d < lo) or (hi is not None and d > hi)):
        problems.add("default-out-of-range", f"{kind} default {d} is outside {lo}..{hi}", line)


def _read_rows(text, columns, problems):
    """CSV text -> (header, [(line number, row dict)]). Blank rows dropped."""
    reader = csv.reader(io.StringIO(text))
    try:
        header = [h.strip() for h in next(reader)]
    except StopIteration:
        return [], []
    rows = []
    for row in reader:
        line = reader.line_num
        if not any(c.strip() for c in row):
            continue
        if len(row) != len(header):
            problems.add("column-count", f"{len(row)} columns, the header has {len(header)}", line)
        rows.append((line, dict(zip(header, row))))
    return header, rows


def _decode(data):
    return data.decode("utf-8-sig")       # 4 files start with a BOM


def _nfc(s):
    """macOS hands back file names decomposed (NFD); the CSV columns are composed (NFC)."""
    return unicodedata.normalize("NFC", s.strip())


def parse_parameters(text, problems):
    """A device CSV's text -> its parameter rows (dicts with section), in file order."""
    header, rows = _read_rows(text, CC_COLUMNS, problems)
    missing = [c for c in ("manufacturer", "device", "parameter_name") if c not in header]
    if missing:
        raise ValueError(f"header lacks required column(s) {', '.join(missing)}")
    extra = [c for c in header if c not in CC_COLUMNS]
    if extra or [c for c in CC_COLUMNS if c not in header]:
        problems.add("header", f"header differs from the 18 standard columns ({header})")
    params = []
    for line, r in rows:
        name = (r.get("parameter_name") or "").strip()
        if not name:
            problems.add("no-name", "row has no parameter_name", line)
        cc_msb = _int(r, "cc_msb", problems, line, 0, 127)
        cc_lsb = _int(r, "cc_lsb", problems, line, 0, 127)
        cc = {
            "msb": cc_msb, "lsb": cc_lsb,
            "min": _int(r, "cc_min_value", problems, line),
            "max": _int(r, "cc_max_value", problems, line),
            "default": _int(r, "cc_default_value", problems, line),
        }
        nrpn_msb = _int(r, "nrpn_msb", problems, line, 0, 127)
        nrpn_lsb = _int(r, "nrpn_lsb", problems, line, 0, 127)
        nrpn = {
            "msb": nrpn_msb, "lsb": nrpn_lsb,
            "min": _int(r, "nrpn_min_value", problems, line),
            "max": _int(r, "nrpn_max_value", problems, line),
            "default": _int(r, "nrpn_default_value", problems, line),
        }
        has_cc = cc_msb is not None
        has_nrpn = nrpn_msb is not None or nrpn_lsb is not None
        if cc_lsb is not None and cc_msb is None:
            problems.add("lsb-without-msb", "cc_lsb given without cc_msb", line)
        if not has_cc and not has_nrpn:
            problems.add("no-message", f"{name!r} has neither a CC nor an NRPN", line)
        if has_cc:
            _range_checks("cc", cc, problems, line)
        if has_nrpn:
            _range_checks("nrpn", nrpn, problems, line)
        orientation = (r.get("orientation") or "").strip()
        if orientation not in ORIENTATIONS:
            problems.add("orientation", f"orientation is {orientation!r}", line)
        # Usage numbers are CC values when there is a CC (CONTRIBUTING.md), else NRPN values. A row
        # carrying both may give its usage in NRPN values: check against the wider of the two.
        lo, hi = (cc["min"], cc["max"]) if has_cc else (nrpn["min"], nrpn["max"])
        if has_cc and has_nrpn and nrpn["max"] is not None and (hi is None or nrpn["max"] > hi):
            lo, hi = nrpn["min"], nrpn["max"]
        usage = parse_usage(r.get("usage"), lo, hi, problems, line)
        units = None
        if usage:
            top = max(s["to"] for s in usage)
            units = "nrpn" if not has_cc or (has_nrpn and cc["max"] is not None and top > cc["max"]) else "cc"
        params.append({
            "section": (r.get("section") or "").strip(),
            "name": name,
            "description": (r.get("parameter_description") or "").strip(),
            "cc": cc if has_cc else None,
            "nrpn": nrpn if has_nrpn else None,
            "orientation": orientation or None,
            "notes": (r.get("notes") or "").strip(),
            "usage": usage,
            "usage_units": units,         # which message's values the usage numbers are in
            "_manufacturer": _nfc(r.get("manufacturer") or ""),
            "_device": _nfc(r.get("device") or ""),
            "_line": line,
        })
    return params


CHANNEL_WORDS = ("basic", "auto", "omni")   # seen in the data: the device's basic channel, set on the device, any


def _channel(r, problems, line):
    """midi_channel: 1..16, or one of CHANNEL_WORDS (kept as the word), or None."""
    v = (r.get("midi_channel") or "").strip()
    if v.lower() in CHANNEL_WORDS:
        return v.lower()
    return _int(r, "midi_channel", problems, line, 1, 16)


def parse_triggers(text, problems):
    """A `.triggers.csv`'s text -> its trigger notes, in file order."""
    header, rows = _read_rows(text, TRIGGER_COLUMNS, problems)
    missing = [c for c in ("manufacturer", "device", "sound_name") if c not in header]
    if missing:
        raise ValueError(f"triggers header lacks required column(s) {', '.join(missing)}")
    notes = []
    for line, r in rows:
        name = (r.get("sound_name") or "").strip()
        if not name:
            problems.add("no-name", "trigger row has no sound_name", line)
        lo = _int(r, "note_number_min_value", problems, line, 0, 127)
        hi = _int(r, "note_number_max_value", problems, line, 0, 127)
        d = _int(r, "note_number_default_value", problems, line, 0, 127)
        _range_checks("note", {"min": lo, "max": hi, "default": d}, problems, line)
        notes.append({
            "section": (r.get("section") or "").strip(),
            "name": name,
            "description": (r.get("sound_description") or "").strip(),
            "channel": _channel(r, problems, line),
            "note": {"min": lo, "max": hi, "default": d},
            "comments": (r.get("comments") or "").strip(),
            "velocity": parse_usage(r.get("velocity_usage"), 0, 127, problems, line),
            "_manufacturer": _nfc(r.get("manufacturer") or ""),
            "_device": _nfc(r.get("device") or ""),
            "_line": line,
        })
    return notes


def _group(items):
    """[{section, ...}] -> [{name, items}] in first-seen order, `section` dropped."""
    out, index = [], {}
    for it in items:
        sec = it.pop("section")
        if sec not in index:
            index[sec] = len(out)
            out.append({"name": sec, "items": []})
        out[index[sec]]["items"].append(it)
    return out


def _slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")


def licence_block(path, commit):
    return {
        "licence": "CC-BY-SA-4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0/",
        "credit": "MIDI Guide (https://midi.guide), maintained by Pencil Research, and its contributors",
        "source": SOURCE_REPO,
        "source_files": path,
        "source_commit": commit,
        "changes": "Converted from CSV to Outrigger's profile JSON by tools/profiles/midiguide.py; usage parsed into value ranges.",
    }


def import_device(manufacturer, device, cc_text=None, triggers_text=None, rel_paths=(), commit=None,
                  usb=None):
    """One device -> (profile dict, warnings). Either text may be None. Raises ValueError when the
    file can't be a profile (missing required columns, or no rows at all)."""
    problems = Problems()
    params = parse_parameters(cc_text, problems) if cc_text is not None else []
    notes = parse_triggers(triggers_text, problems) if triggers_text is not None else []
    if not params and not notes:
        raise ValueError("no parameter or trigger rows")
    for it in params + notes:
        line = it.pop("_line")
        m, d = it.pop("_manufacturer"), it.pop("_device")
        if m != manufacturer:
            problems.add("manufacturer-mismatch", f"manufacturer column {m!r} is not the folder {manufacturer!r}", line)
        if d != device:
            problems.add("device-mismatch", f"device column {d!r} is not the filename {device!r}", line)
    sections = [{"name": g["name"], "parameters": g["items"]} for g in _group(params)]
    trigger_sections = [{"name": g["name"], "notes": g["items"]} for g in _group(notes)]
    profile = {
        "schema": SCHEMA,
        "id": f"{_slug(manufacturer)}/{_slug(device)}",
        "manufacturer": manufacturer,
        "device": device,
        "usb": usb or [],
        "sections": sections,
        "triggers": trigger_sections,
        "attribution": licence_block(list(rel_paths), commit),
    }
    return profile, problems.items


def profile_stats(profile):
    params = [p for s in profile["sections"] for p in s["parameters"]]
    named = [p for p in params if value_names(p["usage"])]
    return {
        "parameters": len(params),
        "sections": len(profile["sections"]),
        "with_usage": sum(1 for p in params if p["usage"]),
        "with_value_names": len(named),
        "cc": sum(1 for p in params if p["cc"]),
        "nrpn": sum(1 for p in params if p["nrpn"]),
        "nrpn_only": sum(1 for p in params if p["nrpn"] and not p["cc"]),
        "cc14": sum(1 for p in params if p["cc"] and p["cc"]["lsb"] is not None),
        "usage_in_nrpn_units_with_cc": sum(1 for p in params if p["cc"] and p["usage_units"] == "nrpn"),
        "trigger_notes": sum(len(s["notes"]) for s in profile["triggers"]),
    }


def _git_commit(src):
    try:
        return subprocess.run(["git", "-C", str(src), "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip() or None
    except (OSError, subprocess.CalledProcessError):
        return None


def import_tree(src, out=None, commit=None, usb_matches=None):
    """Every CSV under src -> profiles (written under out when given) and a report covering every file."""
    src = Path(src)
    usb_matches = usb_matches or {}
    files = sorted(p for p in src.rglob("*.csv") if ".git" not in p.parts)
    devices, report = {}, {"files": [], "devices": []}
    for p in files:
        rel = p.relative_to(src).as_posix()
        if len(p.relative_to(src).parts) != 2:
            report["files"].append({"file": rel, "status": "skipped", "reason": "not <Manufacturer>/<Device>.csv (template)"})
            continue
        manufacturer = _nfc(p.parent.name)
        if p.name.endswith(".triggers.csv"):
            devices.setdefault((manufacturer, _nfc(p.name[: -len(".triggers.csv")])), {})["triggers"] = p
        else:
            devices.setdefault((manufacturer, _nfc(p.stem)), {})["cc"] = p
    for (manufacturer, device), parts in sorted(devices.items()):
        rels = [parts[k].relative_to(src).as_posix() for k in ("cc", "triggers") if k in parts]
        entry = {"manufacturer": manufacturer, "device": device, "files": rels}
        try:
            texts = {k: _decode(parts[k].read_bytes()) for k in parts}
            profile, warnings = import_device(
                manufacturer, device, texts.get("cc"), texts.get("triggers"), rels, commit,
                usb_matches.get(f"{_slug(manufacturer)}/{_slug(device)}"))
        except (ValueError, UnicodeDecodeError, csv.Error) as e:
            entry.update(status="failed", reason=str(e))
        else:
            entry.update(status="warnings" if warnings else "clean", id=profile["id"],
                         kind="cc+triggers" if len(rels) == 2 else ("triggers-only" if "triggers" in parts else "cc"),
                         stats=profile_stats(profile), warnings=warnings)
            if out is not None:
                dest = Path(out) / manufacturer / f"{device}.json"
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(json.dumps(profile, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        report["devices"].append(entry)
        for r in rels:
            report["files"].append({"file": r, "status": entry["status"], "device": f"{manufacturer}/{device}"})
    report["files"].sort(key=lambda f: f["file"])
    devs = report["devices"]
    ok = [d for d in devs if d["status"] != "failed"]
    codes = {}
    for d in ok:
        for w in d["warnings"]:
            codes[w["code"]] = codes.get(w["code"], 0) + 1
    report["summary"] = {
        "source_commit": commit,
        "csv_files": len(files),
        "files_skipped": sum(1 for f in report["files"] if f["status"] == "skipped"),
        "devices": len(devs),
        "devices_clean": sum(1 for d in devs if d["status"] == "clean"),
        "devices_with_warnings": sum(1 for d in devs if d["status"] == "warnings"),
        "devices_failed": sum(1 for d in devs if d["status"] == "failed"),
        "devices_by_kind": {k: sum(1 for d in ok if d["kind"] == k) for k in ("cc", "cc+triggers", "triggers-only")},
        "devices_with_value_names": sum(1 for d in ok if d["stats"]["with_value_names"]),
        "devices_with_continuous_labels_only": sum(1 for d in ok if d["stats"]["with_usage"] and not d["stats"]["with_value_names"]),
        "devices_without_usage": sum(1 for d in ok if d["stats"]["parameters"] and not d["stats"]["with_usage"]),
        "parameters": sum(d["stats"]["parameters"] for d in ok),
        "parameters_with_value_names": sum(d["stats"]["with_value_names"] for d in ok),
        "parameters_nrpn_only": sum(d["stats"]["nrpn_only"] for d in ok),
        "parameters_cc14": sum(d["stats"]["cc14"] for d in ok),
        "parameters_usage_in_nrpn_units_with_cc": sum(d["stats"]["usage_in_nrpn_units_with_cc"] for d in ok),
        "trigger_notes": sum(d["stats"]["trigger_notes"] for d in ok),
        "warnings_by_code": dict(sorted(codes.items())),
    }
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src", help="a checkout of pencilresearch/midi")
    ap.add_argument("out", help="folder for the profiles and report.json")
    ap.add_argument("--commit", help="the checkout's commit (default: git rev-parse HEAD in src)")
    ap.add_argument("--usb", default=str(Path(__file__).with_name("usb_matches.json")),
                    help="profile id -> USB match list (default: usb_matches.json beside this script)")
    a = ap.parse_args(argv)
    usb = {}
    if a.usb and Path(a.usb).exists():
        usb = {k: v for k, v in json.loads(Path(a.usb).read_text()).items() if not k.startswith("_")}
    report = import_tree(a.src, a.out, a.commit or _git_commit(a.src), usb)
    Path(a.out).mkdir(parents=True, exist_ok=True)
    (Path(a.out) / "report.json").write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n")
    print(json.dumps(report["summary"], indent=1))
    return 1 if report["summary"]["devices_failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
