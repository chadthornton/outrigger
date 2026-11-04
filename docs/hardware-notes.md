# Hardware notes

What the first bring-up found, and the milestones it reached.

## Findings (2026-10-26)

- **Rig that works:** hub power brick → hub; hub upstream cable → Waveshare USB-C; FM-1 → hub downstream port.
  The hub's PD pass-through powers the Waveshare up its upstream cable, so the rig runs from one supply.
- **Hub:** Genesys Logic `05e3:0610`, USB 2.0, internally two cascaded 4-port hub chips. On the Waveshare only the
  outer chip comes up; the inner chip's port shows connected but is never enabled. The **USB-C** downstream ports
  are on the outer chip and work. The **USB-A** ports are on the inner chip and don't.
- **ESP32-S3 host channels:** 8 in total. Each hub costs 2, each MIDI device about 3 (EP0 + bulk IN + bulk OUT).
  Hub + FM-1 + Launchkey is exactly 8 once CDC claiming is off, so a third device won't fit. The top screen line
  shows the estimate as `N/8`. A device over budget enumerates (its name appears) but delivers no MIDI.
- **FM-1 on Skiff firmware** enumerates as `1209:0001` "Felucca" (MIDI plus a CDC serial port) and sends its keys
  and knobs as USB MIDI.
- **Safety:** the firmware sends the FM-1 only channel messages forwarded from a controller.
  `midi_core::Router` drops every system message, SysEx included, so a controller can never deliver an install or
  ROM-boot command (stock UBOOT key is `F0 22 24 35 7D F7`). The tests cover this.

## Milestones

1. Done 2026-10-26: FM-1 MIDI shown on the Waveshare screen through the hub.
2. Done 2026-10-26: Launchkey and FM-1 on the hub together (8/8 host channels once CDC claiming is off).
3. Done 2026-10-26: Launchkey → FM-1 routing, all system messages blocked toward the FM-1, played by hand.

Running on the board: `cb4ee7b` (channel fix). `13b99a5` adds drop-reason letters on screen and is not flashed yet.
