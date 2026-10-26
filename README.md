# usb-midi-host

A computer-free USB MIDI host. The first target is the Waveshare ESP32-S3-Touch-LCD-1.69: its USB-C port runs as
a USB host through a powered hub, and incoming MIDI is shown on its screen. The goal is to play an M-VAVE FM-1
from a Novation Launchkey Mini MK4 37 with both on the hub, power and MIDI on one cable each.

## Layout

| Path | What |
|---|---|
| `lib/midi_core/` | Board-independent MIDI logic (formatting, line log). No Arduino headers. |
| `tests/` | Mac-side tests for `midi_core`: `make test` |
| `firmware/waveshare_169/` | Arduino sketch for the Waveshare board. Code is in `app.cpp`, not the `.ino` (see below). |

## Commands

```
make test     # midi_core unit tests on the Mac
make build    # compile the Waveshare firmware
make flash    # flash; PORT=/dev/cu.usbmodemXXXX to pick the port
```

Toolchain: `arduino-cli` (Homebrew), ESP32 core 3.3.12, GFX Library for Arduino 1.6.9, and a vendored copy of
EspUsbHost 2.10.1 (MIT) in `lib/EspUsbHost`. The one patch makes `ESP_USB_HOST_MAX_SERIAL_PORTS` overridable; the
Makefile sets it to 0 so the library never claims a device's CDC serial port. Skiff's serial port otherwise costs
2 of the ESP32-S3's 8 host channels, and with it the hub + FM-1 + Launchkey need 10. Arduino's bundled ctags is x86-only on Apple Silicon, so the Makefile points at Homebrew's universal-ctags and
the sketch keeps its code in `app.cpp`, which skips prototype generation.

## Flashing

Once the firmware runs, the board's only USB port is a host, so the Mac can't reset it. To reflash: connect it to the
Mac directly, hold BOOT, tap RST, release BOOT, then `make flash`.

## Hardware findings (2026-10-26)

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
