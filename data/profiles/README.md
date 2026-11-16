# Device profiles (spike, night 0)

A profile says what a MIDI device's knobs are called and which messages reach them, so a screen can label a
controller's knobs and the hub can remap them (charter milestone C1-3).

- `midi-guide/`: four sample profiles imported from [MIDI Guide](https://midi.guide) (MicroFreak; FM1 Community,
  with its USB match; Moog One, NRPN only; Roland T-8, trigger notes only). The full set (461 devices, about
  12.6 MB) is built, not committed: `make profiles MIDIGUIDE=path/to/pencilresearch-midi`.
- `midi-guide-report.json`: every one of midi.guide's 479 CSV files, what became of it, and each warning.
- `standard-parameters.json`: a draft list of standard parameter ids (filter cutoff, amp attack, ...) that a later
  step maps every device's parameters onto.

## Licence

Profiles made from MIDI Guide are CC-BY-SA 4.0 (credit: MIDI Guide, maintained by Pencil Research, and its
contributors). They keep that licence, separate from the GPL code, and each carries an `attribution` block with its
source files and commit. `usb` entries come from `tools/profiles/usb_matches.json` (ours, not midi.guide's).

## Format (`outrigger.profile/0`)

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
