"""Felucca's device profile, read from the vendored Felucca sources (no hand-copied CC numbers).

    python3 -I tools/profiles/felucca.py [OUT.json]     (default: data/profiles/felucca/Felucca.json)
    python3 -I tools/profiles/felucca.py --check        (exit 1 if the committed profile is stale)

Felucca (hugelton/Felucca, GPL-3.0-only) is the FM-1 firmware Outrigger's synth vendors under
lib/synth/third_party/fm1-cartridge/third_party/felucca. What this reads, all from firmware/src:

- midi_control.c: MIDI_CC_MAP (the standard CC map, #103), MIDI_CC_ENG (what CC74 / CC71 reach per engine),
  ml_free_cc (the CCs MIDI learn never takes) and midi_control's own cases (CC1 6 38 64 98..101 120 121 123);
- engines.c ENGINES[] (engine numbers) and ENGINE_ORDER (the order the panel shows them);
- each engine's `.edit` table (the eight engine parameters E1..E8: label, format, range, default);
- params.c (the common parameters' labels and ranges); core.h / seq.c ROUT (G_ROUTE) for the channel modes.

The prose (what a mode or a message does) is written here, next to the source lines it describes. Every
number in the output comes from the sources; parse failures raise instead of guessing.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FELUCCA = ROOT / "lib/synth/third_party/fm1-cartridge/third_party/felucca"
SRC = FELUCCA / "firmware/src"
ENG_SRC = FELUCCA / "firmware/engines/src"
OUT = ROOT / "data/profiles/felucca/Felucca.json"
VERSION = "1.7.0"
COMMIT = "d7b1b7c4099bba28854869ddb256b3d9f634cee8"   # FELUCCA/README.md: the v1.7.0 tag
FLAGS = {"FELUCCA_FM4": 0, "FELUCCA_SLICE": 1}          # core.h defaults, as Outrigger builds it


def _strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def _preprocess(text):
    """Keep the lines an `#if FLAG` / `#else` / `#endif` with FLAGS selects (one level, as these files use)."""
    out, keep = [], [True]
    for line in text.splitlines():
        m = re.match(r"\s*#if\s+(\w+)\s*$", line)
        if m:
            keep.append(bool(FLAGS[m.group(1)]))
            continue
        if re.match(r"\s*#else\b", line):
            keep[-1] = not keep[-1]
            continue
        if re.match(r"\s*#endif\b", line):
            keep.pop()
            continue
        if all(keep):
            out.append(line)
    return "\n".join(out)


def _array(text, name):
    m = re.search(rf"{name}\s*\[[^\]]*\](?:\s*\[[^\]]*\])?\s*=\s*\{{(.*?)\n\}};", text, re.S)
    if not m:
        raise ValueError(f"{name} not found")
    return _strip_comments(_preprocess(m.group(1)))


def _read(path):
    return path.read_text(encoding="utf-8")


def cc_map(mc):
    """MIDI_CC_MAP -> [(cc, P_* name or MCC_CUT / MCC_RES)]."""
    pairs = re.findall(r"\{\s*(\d+)\s*,\s*(\w+)\s*\}", _array(mc, "MIDI_CC_MAP"))
    if not pairs:
        raise ValueError("MIDI_CC_MAP is empty")
    return [(int(cc), p) for cc, p in pairs]


def cc_eng(mc):
    """MIDI_CC_ENG -> per engine number (CC74's E index, CC71's E index), each 1..8 or 0 (none)."""
    return [(int(b, 16) >> 4, int(b, 16) & 15) for b in re.findall(r"0x([0-9A-Fa-f]{2})", _array(mc, "MIDI_CC_ENG"))]


def engines():
    """ENGINES[] -> [engine_t symbol] by engine number; ENGINE_ORDER -> [engine number] as shown."""
    text = _read(SRC / "engines.c")
    syms = re.findall(r"&(ENG_\w+)", _array(text, "ENGINES"))
    order = [int(n) for n in re.findall(r"\b(\d+)\b", _array(text, "ENGINE_ORDER"))]
    return syms, order


def _engine_def(sym):
    for d in (SRC, ENG_SRC):
        for f in sorted(d.glob("*.c")):
            text = _read(f)
            m = re.search(rf"engine_t\s+{sym}\s*=\s*\{{(.*?)\n\}};", text, re.S)
            if m:
                return f, m.group(1)
    raise ValueError(f"{sym} not defined")


_EDIT_ROW = re.compile(r'\{\s*"([^"]*)"\s*,\s*(F_\w+)\s*,\s*([^,]+),\s*([^,]+),\s*([^,]+),')


def _num(s):
    s = s.strip()
    return int(s) if re.fullmatch(r"-?\d+", s) else None   # an expression (SMP_NALL - 1): not evaluated


def engine(sym):
    """An engine_t -> {name, file, edit: [{label, format, min, max, default}] x 8}."""
    f, body = _engine_def(sym)
    name = re.search(r'\.name\s*=\s*"([^"]+)"', body)
    edit = re.search(r"\.edit\s*=\s*\{(.*?)\n    \},", body, re.S)
    if not name or not edit:
        raise ValueError(f"{sym}: no .name or .edit table")
    rows = [{"label": l, "format": fmt, "min": _num(a), "max": _num(b), "default": _num(d)}
            for l, fmt, a, b, d in _EDIT_ROW.findall(_strip_comments(edit.group(1)))]
    if len(rows) != 8:
        raise ValueError(f"{sym}: {len(rows)} edit rows, not 8")
    return {"symbol": sym, "name": name.group(1), "file": f.relative_to(ROOT).as_posix(), "edit": rows}


def common_params():
    """params.c's PD(...) rows -> {P_*: {label, format, min, max, default}}."""
    text = _read(SRC / "params.c")
    rows = re.findall(r"\[(P_\w+)\]\s*=\s*PD\(\s*\"([^\"]+)\"\s*,\s*(F_\w+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\)", text)
    return {p: {"label": l, "format": f, "min": int(a), "max": int(b), "default": int(d)} for p, l, f, a, b, d in rows}


def learn_never(mc):
    """ml_free_cc's body -> the CCs MIDI learn never takes. Fails if the expression is not the known shape."""
    m = re.search(r"static int ml_free_cc\(uint32_t cc\)[^{]*\{\s*return (.*?);\s*\}", mc, re.S)
    if not m:
        raise ValueError("ml_free_cc not found")
    expr = " ".join(m.group(1).split())
    shape = re.fullmatch(r"cc && cc < (\d+)u((?: && cc != \d+u)+) && \(cc < (\d+)u \|\| cc > (\d+)u\)", expr)
    if not shape:
        raise ValueError(f"ml_free_cc changed shape: {expr}")
    top, singles, lo, hi = int(shape.group(1)), [int(x) for x in re.findall(r"\d+", shape.group(2))], int(shape.group(3)), int(shape.group(4))
    return sorted({0, *singles, *range(lo, hi + 1), *range(top, 128)})


def control_cases(mc):
    """midi_control's switch: the CCs with a meaning of their own (not the standard map)."""
    m = re.search(r"static void midi_control\(.*?\n\{(.*?)\n\}", mc, re.S)
    return sorted({int(c) for c in re.findall(r"case (\d+):", m.group(1))})


def _cc(n, lo=0, hi=127, default=None):
    return {"msb": n, "lsb": None, "min": lo, "max": hi, "default": default}


def _target(d):
    return {"min": d["min"], "max": d["max"], "default": d["default"], "format": d["format"]}


def _param(name, cc, description, notes="", usage=None, target=None, orientation="0-based", rpn=None):
    p = {"name": name, "description": description, "orientation": orientation, "notes": notes,
         "cc": cc, "nrpn": None, "usage": usage or [], "usage_units": "cc" if usage else None, "scope": "part"}
    if rpn:
        p["rpn"] = rpn
    if target:
        p["target"] = target
    return p


# What each standard-map target is, in words (params.c labels are 3-letter panel names).
STANDARD_NAMES = {"P_GLIDE": "Glide", "P_LEVEL": "Level", "P_PAN": "Pan", "P_REL": "Amp release",
                  "P_ATK": "Amp attack", "P_DEC": "Amp decay", "P_REV": "Reverb send", "P_CHOR": "Chorus send",
                  "P_DLY": "Delay send"}
SCALING = ("0..127 is scaled onto the target's own range as a knob would set it: "
           "min + (value * (max - min) + 63) / 127 (midi_control.c midi_cc_set); 64 is the middle of a bipolar one.")


def build():
    mc = _read(SRC / "midi_control.c")
    cmap, ceng = cc_map(mc), cc_eng(mc)
    syms, order = engines()
    if len(ceng) != len(syms):
        raise ValueError(f"MIDI_CC_ENG has {len(ceng)} entries, ENGINES[] {len(syms)}")
    common = common_params()
    cases = control_cases(mc)
    expected_cases = [1, 6, 38, 64, 98, 99, 100, 101, 120, 121, 123]
    if cases != expected_cases:
        raise ValueError(f"midi_control's cases are {cases}, this generator describes {expected_cases}")

    channel = [
        _param("Mod wheel", _cc(1), "The MODW source of the part's modulation matrix (mod.c mod_midi).",
               "No fixed destination: the sound's matrix routes it."),
        _param("Expression", _cc(11, default=127), "The EXPR source of the part's modulation matrix (mod.c: 127 - value is kept).",
               "No fixed destination: the sound's matrix routes it."),
        _param("Sustain pedal", _cc(64), "Holds released notes until the pedal lifts.",
               "DRUM parts ignore it.", usage=[{"from": 0, "to": 63, "kind": "switch", "label": "Off"},
                                               {"from": 64, "to": 127, "kind": "switch", "label": "On"}]),
        _param("Pitch bend range", None, "RPN 0 (CC101 = 0, CC100 = 0), then CC6 = semitones and CC38 = cents.",
               "CC6 clamps to 0..24 semitones, CC38 to 0..99 cents; 2 semitones until set. CC98 / CC99 (NRPN select) "
               "cancel RPN data entry; Felucca has no NRPN parameters.",
               rpn={"msb": 0, "lsb": 0, "min": 0, "max": 24, "default": 2}),
        _param("All sound off", _cc(120), "Silences every part the channel plays at once, pedal or not.", "Any value."),
        _param("Reset all controllers", _cc(121), "Bend, mod wheel and pressure to 0, expression to full (127), pedal up, RPN deselected; keeps the bend range.",
               "Does not reset the standard-map parameters (RP-015: they are the sound's)."),
        _param("All notes off", _cc(123), "Releases the channel's notes normally; the pedal still holds.", "Any value."),
    ]
    standard = []
    for cc, p in cmap:
        if p.startswith("MCC_"):
            continue
        d = common[p]
        standard.append(_param(STANDARD_NAMES[p], _cc(cc), f"Panel {d['label']} ({p}).", SCALING, target=_target(d),
                               orientation="centered" if d["min"] < 0 else "0-based"))
    eng_ccs = sorted(cc for cc, p in cmap if p in ("MCC_CUT", "MCC_RES"))
    which = {cc: p for cc, p in cmap}

    variants = []
    for num, sym in enumerate(syms):
        if num == 1 and not FLAGS["FELUCCA_FM4"]:
            continue                                   # DIGITAL, retired: never selectable (core.h ENGI_DIGITAL)
        e = engine(sym)
        cut, res = ceng[num]
        params = []
        for cc in eng_ccs:
            k = cut if which[cc] == "MCC_CUT" else res
            if not k:
                continue
            d = e["edit"][k - 1]
            if d["min"] is None or d["max"] is None:
                raise ValueError(f"{e['name']} E{k} {d['label']} has a range this generator can't read")
            params.append(_param(d["label"], _cc(cc), f"{e['name']}'s E{k} {d['label']} (CC{cc}: the engine's "
                                 f"{'brightness' if which[cc] == 'MCC_CUT' else 'resonance'} slot).", SCALING,
                                 target=_target(d), orientation="centered" if d["min"] < 0 else "0-based"))
        v = {"id": e["name"].lower(), "name": e["name"], "index": num, "order": order.index(num),
             "source": e["file"], "sections": [{"name": "Engine", "parameters": params}] if params else []}
        if sym == "ENG_DRUM":
            v["ignores"] = ["pitch_bend", "cc:64"]      # midi_control.c: midi_expression, midi_note_event
        variants.append(v)
    variants.sort(key=lambda v: v["order"])

    parts = [{"id": f"t{i}", "name": f"Track {i}"} for i in range(1, 5)]
    block = lambda first: {str(first + i): f"t{i + 1}" for i in range(4)}
    rel = lambda p: p.relative_to(ROOT).as_posix()
    return {
        "schema": "outrigger.profile/1",
        "id": "felucca/felucca",
        "manufacturer": "Hügelton Instruments",
        "device": "Felucca",
        "kind": "instrument",
        "firmware": {"name": "Felucca", "version": VERSION, "commit": COMMIT,
                     "runs_on": "the M-VAVE FM-1 (Skiff enumerates the same)"},
        "usb": [{"name": "Felucca", "vid": "1209", "pid": "0001",
                 "source": "lib/midi_core/src/midi_core.cpp (Role::Target), docs/hardware-notes.md"}],
        "notes": "Four tracks, each its own engine. Every CC acts on the track its channel plays (channel_modes). "
                 "A CC learned with MIDI LEARN no longer reaches this map on any channel, so these labels are "
                 "the defaults. Every other parameter is reachable only through Felucca's SysEx editor protocol, "
                 "which this format does not model. Outrigger's own P4 Felucca node renders one track: the same "
                 "profile with one part.",
        "parts": parts,
        "channel_modes": {
            "setting": "MENU > MIDI > MIDI IN (ROUT; core.h G_ROUTE, seq.c midi_base / midi_slot)",
            "observable": False,
            "default": "ch1-4",
            "unmapped": "A channel the mode doesn't map is ignored entirely: notes, bend, CCs, pressure, Program "
                        "Change and the CC120 / 121 / 123 panics, so it stays free for other gear.",
            "modes": [
                {"id": "ch1-4", "name": "CH1-4", "value": 0, "kind": "block", "channels": block(1)},
                {"id": "sel", "name": "SEL", "value": 1, "kind": "selected",
                 "notes": "Every channel plays the track selected on the panel."},
                {"id": "ch5-8", "name": "CH5-8", "value": 2, "kind": "block", "channels": block(5)},
                {"id": "ch9-12", "name": "CH9-12", "value": 3, "kind": "block", "channels": block(9)},
                {"id": "ch13-16", "name": "CH13-16", "value": 4, "kind": "block", "channels": block(13)},
                {"id": "track", "name": "TRACK", "value": 5, "kind": "per_part",
                 "defaults": {"t1": 1, "t2": 2, "t3": 3, "t4": 4},
                 "notes": "MENU > MIDI > T1..T4 CHANNEL: 1..16, OMNI or OFF each. A channel plays the first track "
                          "set to it, else the first set to OMNI, else none."},
            ],
        },
        "sections": [{"name": "Channel", "parameters": channel},
                     {"name": "Standard map", "parameters": standard}],
        "variant_sets": [{
            "id": "engine", "name": "Engine", "scope": "part", "default": "analog",
            "controls": {"cc": eng_ccs},
            "selected_by": {"observable": False,
                            "how": "The track's engine: set on the panel, by the editor (SysEx) or by loading a sound "
                                   "(a preset, Program Change). No MIDI message reports it, so a hub labelling "
                                   "CC74 / CC71 must be told the engine; an engine that maps neither ignores them."},
            "variants": variants,
        }],
        "messages": {
            "note": {"scope": "part", "notes": "A repeated note replaces the previous press. Chord keys (P_CHRD) may "
                                                "turn one note into a chord."},
            "pitch_bend": {"scope": "part", "notes": "Range from RPN 0 (Channel section); DRUM ignores it. Tracks on "
                                                      "one channel share it (last controller wins)."},
            "channel_pressure": {"scope": "part", "notes": "The AT source of the modulation matrix."},
            "program_change": {"scope": "part", "notes": "The track loads program d1 in the main loop (ui.c "
                                                          "midi_pc_poll, not vendored: what d1 indexes is unchecked)."},
        },
        "learn": {"slots": 16, "never": learn_never(mc),
                  "notes": "A learned CC sets one parameter of one track whatever channel it comes on (a channel ROUT "
                           "ignores stays ignored)."},
        "triggers": [],
        "attribution": {
            "licence": "GPL-3.0-only",
            "credit": "Felucca by Leo Kuroshita (Hügelton Instruments) and contributors; MIDI control by ChanceTheMaker",
            "source": "https://github.com/hugelton/Felucca",
            "source_commit": COMMIT,
            "source_files": [rel(SRC / f) for f in ("midi_control.c", "engines.c", "params.c", "core.h", "seq.c", "mod.c")]
                            + sorted({v["source"] for v in variants}),
            "changes": "Read from the vendored sources by tools/profiles/felucca.py; not from midi.guide.",
        },
    }


def render(profile):
    return json.dumps(profile, indent=1, ensure_ascii=False) + "\n"


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    text = render(build())
    if argv and argv[0] == "--check":
        if OUT.read_text(encoding="utf-8") != text:
            print(f"{OUT.relative_to(ROOT)} is stale: run python3 -I tools/profiles/felucca.py")
            return 1
        print(f"{OUT.relative_to(ROOT)} matches the sources")
        return 0
    out = Path(argv[0]) if argv else OUT
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
