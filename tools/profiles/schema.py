"""Outrigger device profiles: the format's checker and the two lookups a hub needs.

    python3 -I tools/profiles/schema.py PROFILE.json|DIR ...   (exit 1 if any profile is invalid)

`outrigger.profile/0` is what night 0's midi.guide importer wrote. `/1` adds, all optional, so every
/0 profile is a valid /1 profile once its schema string changes:

- `kind`: "instrument" (receives the messages it lists; the default) or "controller" (sends them).
- `parts`: the device's addressable parts (a multitimbral synth's tracks; a controller's zones).
- `channel_modes`: how MIDI channels reach parts, as data: fixed blocks of channels, "every channel to the
  selected part", or "each part on its own channel". A channel a mode doesn't map is ignored entirely.
- `variant_sets`: a dimension (an engine, a pad layer) whose choice changes what some messages mean. Each
  variant lists only the parameters behind the set's `controls`; the base sections never use those.
- per parameter: `scope` ("part" or "device"), `target` (the native range a 0..127 CC is scaled onto),
  `rpn` (like `nrpn`).
- `messages`: the channel messages other than CCs (pitch bend, pressure, program change) and what they do.
- `learn`: MIDI learn slots and the CCs that can never be learned.

validate(profile) -> [error strings]; route(profile, mode, channel, ...) -> part id or None;
resolve(profile, {set id: variant id}) -> the flat parameter list a screen shows for that choice.
"""
import json
import sys
from pathlib import Path

SCHEMAS = ("outrigger.profile/0", "outrigger.profile/1")
CURRENT = SCHEMAS[-1]
KINDS = ("instrument", "controller")
ORIENTATIONS = ("0-based", "centered", None)
USAGE_KINDS = ("value", "switch", "continuous")
SCOPES = ("part", "device")
MODE_KINDS = ("block", "selected", "per_part")
CHANNEL_WORDS = ("basic", "auto", "omni")
MESSAGE_KEYS = ("pitch_bend", "channel_pressure", "poly_pressure", "program_change", "note")


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _msg_block(where, b, errs, lo=0, hi=127):
    """cc / nrpn / rpn block: {msb, lsb, min, max, default}."""
    if not isinstance(b, dict):
        errs.append(f"{where}: not an object")
        return
    for k in ("msb", "lsb", "min", "max", "default"):
        if k not in b:
            errs.append(f"{where}: lacks {k}")
    for k in ("msb", "lsb"):
        v = b.get(k)
        if v is not None and (not _is_int(v) or not lo <= v <= hi):
            errs.append(f"{where}.{k}: {v!r} is not an integer {lo}..{hi}")
    for k in ("min", "max", "default"):
        v = b.get(k)
        if v is not None and not _is_int(v):
            errs.append(f"{where}.{k}: {v!r} is not an integer")


def _parameter(where, p, errs):
    if not isinstance(p, dict):
        errs.append(f"{where}: not an object")
        return
    if not isinstance(p.get("name"), str):
        errs.append(f"{where}: name is not a string")
    for k in ("cc", "nrpn", "rpn"):
        if p.get(k) is not None:
            _msg_block(f"{where}.{k}", p[k], errs)
    if p.get("cc") is not None and p["cc"].get("msb") is None:
        errs.append(f"{where}.cc: msb is null")
    if p.get("orientation") not in ORIENTATIONS:
        errs.append(f"{where}: orientation {p.get('orientation')!r}")
    for i, s in enumerate(p.get("usage") or []):
        w = f"{where}.usage[{i}]"
        if not isinstance(s, dict) or not _is_int(s.get("from")) or not _is_int(s.get("to")):
            errs.append(f"{w}: needs integer from/to")
        elif s.get("kind") not in USAGE_KINDS or not isinstance(s.get("label"), str):
            errs.append(f"{w}: kind {s.get('kind')!r} or label is wrong")
    if p.get("usage_units") not in ("cc", "nrpn", "rpn", None):
        errs.append(f"{where}: usage_units {p.get('usage_units')!r}")
    if "scope" in p and p["scope"] not in SCOPES:
        errs.append(f"{where}: scope {p['scope']!r} is not one of {SCOPES}")
    if "target" in p:
        t = p["target"]
        if not isinstance(t, dict) or not _is_int(t.get("min")) or not _is_int(t.get("max")) or t["min"] > t["max"]:
            errs.append(f"{where}.target: needs integer min <= max")
        elif t.get("default") is not None and not (_is_int(t["default"]) and t["min"] <= t["default"] <= t["max"]):
            errs.append(f"{where}.target.default {t.get('default')!r} is outside {t['min']}..{t['max']}")


def _sections(where, sections, errs):
    if not isinstance(sections, list):
        errs.append(f"{where}: not a list")
        return
    for i, s in enumerate(sections):
        if not isinstance(s, dict) or not isinstance(s.get("name"), str) or not isinstance(s.get("parameters"), list):
            errs.append(f"{where}[{i}]: needs name and a parameters list")
            continue
        for j, p in enumerate(s["parameters"]):
            _parameter(f"{where}[{i}].parameters[{j}] ({p.get('name') if isinstance(p, dict) else '?'})", p, errs)


def params_of(sections):
    return [p for s in sections for p in s["parameters"]]


def _cc_of(p):
    return p["cc"]["msb"] if p.get("cc") else None


def _channels(where, chans, errs):
    if not isinstance(chans, dict):
        errs.append(f"{where}: not an object")
        return {}
    for ch in chans:
        if not (ch.isdigit() and 1 <= int(ch) <= 16):
            errs.append(f"{where}: channel {ch!r} is not 1..16")
    return chans


def _channel_modes(cm, part_ids, errs):
    if not isinstance(cm, dict) or not isinstance(cm.get("modes"), list) or not cm["modes"]:
        errs.append("channel_modes: needs a non-empty modes list")
        return
    ids = [m.get("id") for m in cm["modes"] if isinstance(m, dict)]
    if len(set(ids)) != len(ids):
        errs.append("channel_modes: duplicate mode ids")
    if cm.get("default") not in ids:
        errs.append(f"channel_modes.default {cm.get('default')!r} is not a mode")
    for m in cm["modes"]:
        w = f"channel_modes[{m.get('id')}]"
        kind = m.get("kind")
        if kind not in MODE_KINDS:
            errs.append(f"{w}: kind {kind!r} is not one of {MODE_KINDS}")
        elif kind == "block":
            chans = _channels(f"{w}.channels", m.get("channels"), errs)
            for ch, part in chans.items():
                if part not in part_ids:
                    errs.append(f"{w}: channel {ch} -> unknown part {part!r}")
            if len(set(chans.values())) != len(chans):
                errs.append(f"{w}: two channels reach one part")
        elif kind == "per_part":
            d = m.get("defaults")
            if not isinstance(d, dict) or set(d) != set(part_ids):
                errs.append(f"{w}: defaults must give every part a channel")
            else:
                for part, ch in d.items():
                    if not ((_is_int(ch) and 1 <= ch <= 16) or ch in ("omni", "off")):
                        errs.append(f"{w}: part {part} default {ch!r} is not 1..16, omni or off")


def _variant_sets(vs, base_params, part_ids, errs):
    if not isinstance(vs, list):
        errs.append("variant_sets: not a list")
        return
    base_ccs = {_cc_of(p) for p in base_params} - {None}
    for s in vs:
        w = f"variant_sets[{s.get('id') if isinstance(s, dict) else '?'}]"
        if not isinstance(s, dict) or not isinstance(s.get("id"), str) or not isinstance(s.get("variants"), list):
            errs.append(f"{w}: needs id and a variants list")
            continue
        if s.get("scope") not in SCOPES:
            errs.append(f"{w}: scope {s.get('scope')!r}")
        if s.get("scope") == "part" and not part_ids:
            errs.append(f"{w}: scope part, but the profile has no parts")
        controls = s.get("controls") or {}
        ccs = controls.get("cc") or []
        if not ccs:
            errs.append(f"{w}: controls.cc lists no CCs")
        clash = sorted(base_ccs & set(ccs))
        if clash:
            errs.append(f"{w}: CC {clash} is in the base sections and in the variants: which meaning wins?")
        if "observable" not in (s.get("selected_by") or {}):
            errs.append(f"{w}: selected_by.observable must say whether the hub can see the choice")
        vids = [v.get("id") for v in s["variants"]]
        if len(set(vids)) != len(vids):
            errs.append(f"{w}: duplicate variant ids")
        if s.get("default") is not None and s["default"] not in vids:
            errs.append(f"{w}: default {s['default']!r} is not a variant")
        for v in s["variants"]:
            vw = f"{w}.{v.get('id')}"
            _sections(f"{vw}.sections", v.get("sections", []), errs)
            seen = set()
            for p in params_of(v.get("sections", [])):
                cc = _cc_of(p)
                if cc is None or cc not in ccs:
                    errs.append(f"{vw}: parameter {p.get('name')!r} (CC {cc}) is not one of the set's controls {ccs}")
                elif cc in seen:
                    errs.append(f"{vw}: CC {cc} given twice")
                seen.add(cc)
            for ig in v.get("ignores", []):
                if ig not in MESSAGE_KEYS and not (isinstance(ig, str) and ig.startswith("cc:") and ig[3:].isdigit()):
                    errs.append(f"{vw}: ignores {ig!r} is not a message key or cc:N")


def validate(profile):
    """The profile's problems as strings; [] when it is valid."""
    errs = []
    if not isinstance(profile, dict):
        return ["not a JSON object"]
    if profile.get("schema") not in SCHEMAS:
        errs.append(f"schema {profile.get('schema')!r} is not one of {SCHEMAS}")
    for k in ("id", "manufacturer", "device"):
        if not isinstance(profile.get(k), str) or not profile.get(k):
            errs.append(f"{k} is missing or not a string")
    if profile.get("kind", "instrument") not in KINDS:
        errs.append(f"kind {profile.get('kind')!r} is not one of {KINDS}")
    usb = profile.get("usb")
    if not isinstance(usb, list):
        errs.append("usb is not a list")
    else:
        for i, u in enumerate(usb):
            if not isinstance(u, dict) or not all(isinstance(u.get(k), str) and len(u.get(k)) == 4 for k in ("vid", "pid")):
                errs.append(f"usb[{i}]: needs 4-digit hex vid and pid")
    _sections("sections", profile.get("sections"), errs)
    triggers = profile.get("triggers")
    if not isinstance(triggers, list):
        errs.append("triggers is not a list")
    else:
        for i, t in enumerate(triggers):
            for j, n in enumerate(t.get("notes", []) if isinstance(t, dict) else []):
                ch = n.get("channel")
                if ch is not None and ch not in CHANNEL_WORDS and not (_is_int(ch) and 1 <= ch <= 16):
                    errs.append(f"triggers[{i}].notes[{j}]: channel {ch!r}")
    a = profile.get("attribution")
    if not isinstance(a, dict) or not a.get("licence") or not a.get("source"):
        errs.append("attribution needs a licence and a source")
    if profile.get("schema") == "outrigger.profile/0":
        extra = [k for k in ("kind", "parts", "channel_modes", "variant_sets", "messages", "learn") if k in profile]
        if extra:
            errs.append(f"{extra} need schema outrigger.profile/1")
    parts = profile.get("parts", [])
    part_ids = [p.get("id") for p in parts if isinstance(p, dict)]
    if len(set(part_ids)) != len(part_ids) or None in part_ids:
        errs.append("parts: every part needs a unique id")
    if "channel_modes" in profile:
        if not part_ids:
            errs.append("channel_modes needs parts")
        _channel_modes(profile["channel_modes"], part_ids, errs)
    if "variant_sets" in profile and isinstance(profile.get("sections"), list):
        _variant_sets(profile["variant_sets"], params_of(profile["sections"]), part_ids, errs)
    for k in profile.get("messages", {}) or {}:
        if k not in MESSAGE_KEYS:
            errs.append(f"messages: {k!r} is not one of {MESSAGE_KEYS}")
    if "learn" in profile:
        L = profile["learn"]
        if not _is_int(L.get("slots")) or not all(_is_int(c) and 0 <= c <= 127 for c in L.get("never", [])):
            errs.append("learn needs integer slots and a list of never-learnable CCs")
    return errs


def route(profile, mode_id, channel, part_channels=None, selected=None):
    """The part a message on `channel` (1..16) reaches under channel mode `mode_id`, or None (ignored).

    block: the mode's channel map. selected: every channel -> `selected` (the part chosen on the device).
    per_part: `part_channels` {part: 1..16 | "omni" | "off"} (the mode's defaults when None); the first part
    set to the channel, else the first set to omni, in the profile's part order."""
    mode = next(m for m in profile["channel_modes"]["modes"] if m["id"] == mode_id)
    if mode["kind"] == "block":
        return mode["channels"].get(str(channel))
    if mode["kind"] == "selected":
        return selected
    pc = part_channels or mode["defaults"]
    order = [p["id"] for p in profile["parts"]]
    for part in order:
        if pc.get(part) == channel:
            return part
    for part in order:
        if pc.get(part) == "omni":
            return part
    return None


def resolve(profile, choice=None):
    """The parameters in effect for `choice` {variant set id: variant id} (each set's default if absent):
    the base sections' parameters, then the chosen variant's. A set's control CC that its variant doesn't
    map is in `ignored` as cc:N. Returns (parameters, ignored)."""
    params = list(params_of(profile["sections"]))
    ignored = set()
    for s in profile.get("variant_sets", []):
        vid = (choice or {}).get(s["id"], s.get("default"))
        v = next((v for v in s["variants"] if v["id"] == vid), None)
        if v is None:
            raise KeyError(f"variant set {s['id']!r} has no variant {vid!r}")
        mine = params_of(v.get("sections", []))
        params += mine
        mapped = {_cc_of(p) for p in mine}
        ignored |= {f"cc:{cc}" for cc in s["controls"]["cc"] if cc not in mapped}
        ignored |= set(v.get("ignores", []))
    return params, sorted(ignored)


def _files(args):
    for a in args:
        p = Path(a)
        if p.is_dir():
            yield from sorted(q for q in p.rglob("*.json") if q.name != "report.json")
        else:
            yield p


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__.split("\n\n")[1].strip())
        return 2
    n = bad = 0
    for f in _files(argv):
        n += 1
        errs = validate(json.loads(f.read_text(encoding="utf-8")))
        if errs:
            bad += 1
            print(f"{f}: {len(errs)} error(s)")
            for e in errs[:10]:
                print(f"  {e}")
    print(f"{n - bad} of {n} profiles valid")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
