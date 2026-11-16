# Device profiles (spike, nights 0 and 1)

A profile says what a MIDI device's knobs are called and which messages reach them, so a screen can label a
controller's knobs and the hub can remap them (charter milestone C1-3).

- `midi-guide/`: four sample profiles imported from [MIDI Guide](https://midi.guide) (MicroFreak; FM1 Community,
  with its USB match; Moog One, NRPN only; Roland T-8, trigger notes only). The full set (461 devices, about
  12.6 MB) is built, not committed: `make profiles MIDIGUIDE=path/to/pencilresearch-midi`.
- `felucca/Felucca.json`: Felucca 1.7.0 on the FM-1, generated from the vendored sources by
  `tools/profiles/felucca.py` (`--check` fails when it is stale; `make profiles` runs it). GPL-3.0, not midi.guide's.
- `midi-guide-report.json`: every one of midi.guide's 479 CSV files, what became of it, and each warning.
- `standard-parameters.json`: a draft list of standard parameter ids (filter cutoff, amp attack, ...) that a later
  step maps every device's parameters onto.

## Licence

Profiles made from MIDI Guide are CC-BY-SA 4.0 (credit: MIDI Guide, maintained by Pencil Research, and its
contributors). They keep that licence, separate from the GPL code, and each carries an `attribution` block with its
source files and commit. `usb` entries come from `tools/profiles/usb_matches.json` (ours, not midi.guide's).

## Format (`outrigger.profile/1`)

`tools/profiles/schema.py` checks it (`python3 -I tools/profiles/schema.py DIR`); `/1` only adds optional fields
to night 0's `/0`, so every `/0` profile stays valid. The midi.guide importer writes `/1` with none of them.

```
{ schema, id: "arturia/microfreak", manufacturer, device,
  usb: [ {name, vid, pid, source} ],                 // [] when unknown
  sections: [ {name, parameters: [
      {name, description, orientation: "0-based"|"centered", notes,
       cc:   {msb, lsb, min, max, default} | null,   // lsb set: a 14-bit CC pair
       nrpn: {msb, lsb, min, max, default} | null,
       usage: [ {from, to, kind, label} ],           // parsed from midi.guide's usage column
       usage_units: "cc"|"nrpn"|null } ] } ],
  triggers: [ {name, notes: [ {name, description, channel, note: {min, max, default}, comments, velocity: [...]} ]} ],
  attribution: {licence, licence_url, credit, source, source_files, source_commit, changes} }
```

`usage` kinds: `value` is one number (`5: Saw`), `switch` is a range whose values all do the same thing (`0-11: Basic
Waves`), and `continuous` is a range where every value differs (`0~127: Cutoff`). A `value` or `switch` segment is a
value name; a `continuous` one labels a sweep.

### Added in `/1` (all optional)

```
  kind: "instrument" | "controller",                 // receives (default) or sends the messages it lists
  firmware: {name, version, commit, runs_on},        // when a firmware, not the hardware, defines the map
  notes: "...",                                      // what the profile can't say in fields
  parts: [ {id, name} ],                             // a multitimbral synth's tracks; a controller's zones
  channel_modes: { setting, observable, default, unmapped,
      modes: [ {id, name, kind: "block", channels: {"1": part, ...}}         // a fixed block; others ignored
             | {id, name, kind: "selected"}                                 // every channel -> the selected part
             | {id, name, kind: "per_part", defaults: {part: 1..16|"omni"|"off"}} ] },
  variant_sets: [ {id, name, scope: "part"|"device", default,
      controls: {cc: [..]},                          // the CCs whose meaning the choice changes
      selected_by: {observable: bool, how},          // can the hub see the choice? Felucca: no
      variants: [ {id, name, sections: [...], ignores: ["pitch_bend", "cc:64", ...]} ] } ],
  messages: {note, pitch_bend, channel_pressure, poly_pressure, program_change: {scope, notes}},
  learn: {slots, never: [cc, ...], notes},
  per parameter: scope: "part"|"device", target: {min, max, default, format}, rpn: {msb, lsb, min, max, default}
```

Rules the checker enforces: a variant may only use its set's `controls` CCs, once each; the base sections never use
them (so one CC never has two meanings in one view); a block maps each part from at most one channel; `per_part`
gives every part a default. `schema.resolve(profile, {"engine": "drum"})` gives the parameters a screen shows for
one choice (and the messages it ignores), `schema.route(profile, mode, channel)` the part a channel reaches.

### Controllers use the same format

A controller profile is the same file with `kind: "controller"`: each parameter is a physical control and its `cc`
is what it sends; `parts` are its zones (keys, pads) and `channel_modes` the channel each zone sends on; a layer or
page button that changes what the pads send is a `variant_set` (with `observable: true` when the layer change is
itself sent). A hub then maps a controller's resolved view onto an instrument's resolved view by `standard`
parameter ids, rather than by CC number. `tests/test_profiles_format.py` has a synthetic example; no real
controller profile is written yet (midi.guide has no Launchkey file).
