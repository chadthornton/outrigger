# Porting to other ESP32 boards

[Index](index.md) · previous: [The FM-1's flash, firmwares and save states](fm1-flash-and-firmwares.md) · next: [The loader's safety design](loader-safety.md)

One board runs this today, the Waveshare ESP32-S3-Touch-LCD-1.69. `docs/boards.md` lists boards checked for USB
host support. This page is about what moves to a new board and what doesn't.

## You already have an ESP32-S3R8

The ESP32-S3R8 is a chip, not a board: an ESP32-S3 with 8 MB of octal PSRAM stacked in the package and no flash
inside. The board adds its own flash chip. The Waveshare 1.69" is an S3R8 board with a 16 MB W25Q128 flash chip
([Waveshare's documentation](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.69)), which is why the Makefile builds
with `FlashSize=16M,PSRAM=opi`. The Waveshare 3.5" in `docs/boards.md` is an S3R8 board too.

So "try an ESP32-S3R8" is a question about the board around it, not the chip:
1. **Is its USB-C wired to the S3's native USB** (GPIO19/20), with no USB-to-serial chip in between? This is the one
   that decides whether it can host at all. See `docs/boards.md`, "The one test".
2. **How much flash?** The partition table assumes 16 MB. With 8 MB, the table and the store have to shrink (below).
3. **Does it power devices on that port?** Not needed with this project's powered hub, which also powered the
   Waveshare.
4. **Which screen and touch chips,** and are GPIO19/20 free of them?

The ESP32-S3 datasheet reserves several GPIOs (33–37) for octal PSRAM on R8 parts; a board design has already
accounted for that, but it matters if you wire anything extra. (From Espressif's datasheet, not checked in this
repo.)

## Which ESP32 chips can host USB

| Chip | USB host? | Host channels | Source |
|---|---|---|---|
| ESP32-S3 | Yes, full speed (USB OTG) | 8 | EspUsbHost README and header; this project |
| ESP32-S2 | Yes, full speed | 8 | EspUsbHost README and header |
| ESP32-P4 | Yes: a full-speed port and a high-speed port | 8 (FS), 16 (HS) | EspUsbHost header; `docs/boards.md` |
| ESP32 (original), C3, C6, H2 | No USB OTG controller | | Espressif datasheets (not checked in this repo) |

**The P4's catch:** its 16-channel high-speed port would fit hub, FM-1 MIDI and audio, a controller and a USB stick
together. But EspUsbHost says hubs are "effectively not usable" on that port, and P4 validation is ongoing. This
whole design uses a hub. `docs/boards.md` recommends one short P4 spike with one question: does a hub work on its
16-channel port?

## What's portable

These libraries have no board code and move as they are:

| Library | What it is | Board dependence |
|---|---|---|
| `lib/midi_core` | Routing, the drop rules, chain health, Computer mode logic | None. No Arduino headers. |
| `lib/platform` | The resource broker, app-set flags, descriptor parsing | None, except `kHostChannels = 8` in `broker.h`, the S3's count. A P4 port would make it a parameter. |
| `lib/fm1_loader` | Packages, the guard, the store, the ROM client, the installer | None. Storage and SCSI transport are interfaces. |
| `lib/web_core` | Log rings, routes, the Host guard | None. |
| `lib/host_ui` | The LVGL screens | Lays out from a `BoardProfile` (size, shape, corner radius, font boost). `make ui-shots` renders every screen at a profile's size, so a new screen can be checked on the Mac first. |
| `lib/EspUsbHost` | The vendored, patched USB host library | Supports the S2, S3 and P4. |

## What's per-board, honestly

README says to add a board you copy the sketch folder and write a new `board_*.cpp`. That's true, but more is per-board
than that file:

| Piece | Where | Why it's per-board |
|---|---|---|
| Board profile and drivers | `firmware/<board>/board_*.cpp` (68 lines for the Waveshare) | `BoardProfile` plus three hooks: `board_display_begin()`, `board_display_flush()`, `board_touch_read()`. |
| Chip options | `FQBN` in `Makefile` | Hard-coded `FlashSize=16M,PSRAM=opi` for every board. A board with quad PSRAM or other flash needs its own. |
| Partition table | `firmware/<board>/partitions.csv` | Assumes 16 MB: a 3 MB app slot, `store` at `0x3D0000` for 12.1 MB, `coredump` at `0xFF0000`. `tools/check_partitions.py` and `tools/build_site.py` check the install layout against it. |
| The app itself | `app.cpp` (660 lines), `loader_app.cpp` (877), `web.cpp` (916), and helpers | Not board-specific, but they live in the sketch folder, so copying the folder duplicates about 2,450 lines. Moving them to a shared place before the second board lands would avoid two copies drifting. |
| Build lists | `BOARDS` in `Makefile` and `tools/build_site.py`, a card in `site/index.html` | So `make build-all`, CI and the web installer include it. |
| Inputs | | A board with buttons and no touch (the 1.47") needs LVGL's keypad/encoder input instead of taps. |

**With 8 MB of flash:** the store shrinks from 12.1 MB to about 4 MB. At the measured overhead (4 packages took 4.3
MB) that's a handful of packages plus backups. A TF card would carry the library; each unit's safety backup stays in
internal flash either way (`docs/architecture.md`, "Storage tiers").

## Checklist for a new board

1. Confirm native USB on the connector from the schematic.
2. Write `board_<name>.cpp`: the profile, display, touch.
3. Set the FQBN and partition table for its flash and PSRAM.
4. Render the screens at its size with `make ui-shots`, before flashing anything.
5. Flash the `hub` app set and check the rig at 8/8 with the Launchkey playing the FM-1.
6. Only then try `hub-loader`, read-only first: a backup, compared with a known dump of your FM-1.
