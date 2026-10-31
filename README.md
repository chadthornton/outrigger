# usb-midi-host

A computer-free USB MIDI host. The first target is the Waveshare ESP32-S3-Touch-LCD-1.69: its USB-C port runs as
a USB host through a powered hub, and incoming MIDI is shown on its screen. The goal is to play an M-VAVE FM-1
from a Novation Launchkey Mini MK4 37 with both on the hub, power and MIDI on one cable each.

It is one shared core with pluggable apps (Router, firmware Loader, and, in progress, Recorder and Web UI), chosen
per build as an app set. The core owns the 8 USB host channels, the FM-1, the radio, storage, settings and the screen;
apps ask it for them. See `docs/architecture.md`, and the merge gate below before landing anything.

## Layout

| Path | What |
|---|---|
| `docs/architecture.md` | The shared core, the apps, the mode rules and the app sets. |
| `lib/platform/` | Shared core, plain C++: the resource broker (`broker.h`: USB claims within 8 channels, modes, FM-1 ownership, screen reasons) and the app-set flags (`apps.h`). |
| `lib/midi_core/` | Board-independent MIDI logic (formatting, line log). No Arduino headers. |
| `tests/` | Mac-side tests for `midi_core`, `platform` and `fm1_loader`: `make test`. `tests/golden/` holds the loader's golden vectors. |
| `lib/fm1_loader/` | FM-1 firmware loader core (no Arduino headers): packages, address guard, sector store, units and save states, ROM-boot install state machine. Plan: `docs/firmware-loader-plan.md`. |
| `lib/web_core/` | Web UI pieces with no Arduino headers: the diagnostics log (lock-free rings per task, numbered history), the read-only route table, the Host guard, the setup hotspot's channel pick. Tested by `make test`. |
| `tools/diag.py` | Reads the board's web diagnostics from the Mac: `/status`, `/log`, `/crash`, `/store`, and saves `/coredump` for `esp-coredump`. |
| `lib/host_ui/` | The LVGL screens (signal chain, settings), laid out from a `BoardProfile`, not fixed pixels. |
| `tests/ui_shots/` | `make ui-shots`: renders every `host_ui` screen with LVGL on the Mac (S/M/L, hub and hub-loader, 0-6 controllers, Off, a problem, settings, Firmware, Computer mode), taps them with a drifting finger, and fails on a clipped or out-of-safe-area label. PNGs and `sheet.png` in `build/ui-shots/`. Check UI changes here before booking the board. |
| `firmware/config/lv_conf.h` | LVGL configuration (fonts, clib malloc so big buffers can use PSRAM). |
| `site/`, `tools/build_site.py` | Browser installer (ESP Web Tools) and the script that stages firmware into it. |
| `.github/workflows/ci.yml` | CI, the same checks as `make check` on GitHub's machines. Off: disabled on GitHub (2026-10-31) and manual-only; `gh workflow enable CI`, then "Run workflow". Not part of the merge gate. |
| `firmware/waveshare_169/` | Arduino sketch for the Waveshare board: `app.cpp` (USB host, routing, gathers the frame) and `board_waveshare_169.cpp` (board profile, display and touch drivers). Code stays out of the `.ino` (see below). |

To add a board: copy the sketch folder, write a new `board_*.cpp` with its `BoardProfile` (size, shape, corner
radius, font boost) and the three driver hooks in `lib/host_ui/src/board_profile.h`. `app.cpp` and `host_ui` don't
change. Check the board's schematic for a USB-C port wired to the ESP32-S3's native USB (GPIO19/20). Then add it to
`BOARDS` in the `Makefile` and in `tools/build_site.py`, and a card in `site/index.html`.

## Commands

```
make test      # midi_core, platform and fm1_loader tests on the Mac (fetches fixtures once)
make check     # merge gate step 2: a clean clone of HEAD, then test, ui-shots and site (several minutes)
make ui-shots  # render and check every screen; build/ui-shots/sheet.png (needs LVGL 9.6.0, see Makefile)
make fixtures  # FM-1 packages and wl82loader.bin for the tests, pinned by SHA-256 (gitignored)
make golden    # regenerate tests/golden/ from the Unbricker and jl-uboot-tool (Python venv in build/)
make build     # compile one board x app set: BOARD=waveshare_169 (default) APPS=hub|hub-loader|everything
make build-all # every board x app set, into build/<board>/<app set>/
make build APPS=hub-loader DIAG_WEB=1  # hub-loader plus the read-only web diagnostics (build/<board>/hub-loader-web/)
python3 tools/diag.py all              # status, log, crash and store into build/diag/<time>/, plus the core dump
make flash     # flash BOARD/APPS; PORT=/dev/cu.usbmodemXXXX to pick the port
make serve     # web installer on http://localhost:8088
make store-image                    # the store partition's first setup (LittleFS image with the loader blob, in build/)
make flash-store CONFIRM=erase-store  # flash it; ERASES every backup in the store
```

Toolchain: `arduino-cli` (Homebrew), ESP32 core 3.3.12, GFX Library for Arduino 1.6.9 (display driver), LVGL 9.6.0
(UI), and a vendored copy of
EspUsbHost 2.10.1 (MIT) in `lib/EspUsbHost`. The one patch makes `ESP_USB_HOST_MAX_SERIAL_PORTS` overridable; the
Makefile sets it to 0 so the library never claims a device's CDC serial port. Skiff's serial port otherwise costs
2 of the ESP32-S3's 8 host channels, and with it the hub + FM-1 + Launchkey need 10. Arduino's bundled ctags is x86-only on Apple Silicon, so the Makefile points at Homebrew's universal-ctags (`CTAGS_DIR`, `/usr/bin` in CI) and
the sketch keeps its code in `app.cpp`, which skips prototype generation.

## Flashing

Once the firmware runs, the board's only USB port is a host, so the Mac can't reset it. Two ways in:

- **Computer mode (no buttons):** on the board, Settings > **Computer**, tap again within 3 s. It restarts with the USB
  host off and hands the port to the ESP32-S3's built-in USB-Serial-JTAG device (`303a:1001`), so `make flash` or the web
  installer can reset it into download mode themselves. The request is kept in NVS (namespace `boot`) with the build's
  ELF SHA, so it survives moving the cable. The first boot of a *different* build clears it and runs the host again;
  "Back to MIDI" clears it by hand. Logic in `lib/midi_core/src/boot_mode.h`, tests in `make test`.
- **Buttons (always works):** connect it to the Mac directly, hold BOOT, tap RST, release BOOT, then `make flash`.
  After a BOOT+RST flash, an RTS reset drops the chip back into download mode; tap RST (or use esptool's
  `--after watchdog-reset`) to start the firmware.

## Web installer

`site/` is a static page that flashes the firmware from Chrome or Edge with
[ESP Web Tools](https://github.com/esphome/esp-web-tools) 10.4.0 (Apache-2.0, vendored in `site/vendor/`, no CDN).

```
make site     # build every board x app set, then stage each into site/firmware/<board>/<app set>/ with its manifest.json
make serve    # same, then serve on http://localhost:8088 (Web Serial needs https or localhost)
```

- **Four parts, not a merged image.** The manifest writes bootloader @0x0, partitions @0x8000, boot_app0 @0xe000 and the
  app @0x10000, exactly what `make flash` writes. A merged image from 0x0 would also overwrite NVS (0x9000-0xDFFF), and
  Arduino's `.merged.bin` is padded to 16 MB, so it would wipe every partition. `tools/build_site.py` refuses parts that
  overlap NVS or overflow app0.
- **No erase by default.** ESP Web Tools erases the whole chip for firmware without Improv unless the manifest sets
  `new_install_prompt_erase`; ours does, so the user sees an "Erase device" box that starts unticked.
- **App sets:** one Install button per app set (MIDI Hub, Hub + Loader, Everything), each with its own manifest. Hub +
  Loader ships with the Loader's writes on (`WRITES` defaults to 1 for `hub-loader`; `WRITES=0` builds it read-only);
  Everything's loader stays read-only. The settings page shows which set is running next to the commit (`hub`,
  `hub+L`, `all`).
- **Adding a board:** see Layout.
- **Licensing:** our code, LVGL (MIT) and the vendored EspUsbHost (MIT) ship as binaries. JieLi's `wl82loader.bin` must
  never be in a published build: `build_site.py` refuses a file with its name or SHA-256, or containing a 1 KiB piece of
  it (when `tests/fixtures/wl82loader.bin` or `$WL82_BLOB` is present), and deletes the staged firmware if so.
- Not published anywhere yet. GitHub Pages (or any host) needs Chad's go-ahead.

## Web diagnostics

Builds with the Web UI (`everything`, or `hub-loader` with `DIAG_WEB=1`) join Wi-Fi and serve a read-only page at
`http://fm1hub.local` (the settings page shows the address). With no network saved they open the `fm1hub-setup`
hotspot (password on the settings page); its page saves a network to NVS `wifi`, the only thing any route writes.
Everything else is GET: `/status` (build, heap, each task's unused stack, USB devices with their channels and
refusals, the Firmware page), `/log?since=N` (lines from the USB task, the loader task and the loop), `/crash` (the
last abnormal reset), `/coredump` (the raw partition) and `/store`. After a crash, safe mode starts Wi-Fi too, so the
crash can be read without the screen. `tools/diag.py` fetches them from the Mac.

## Merge gate

Nothing lands on `master` until all three pass, on the branch's last commit:

1. **Mac tests.** `make test` passes every suite on the Mac.
2. **Clean check.** `make check` passes on the Mac: it clones the branch's last commit fresh into `build/check` (so
   nothing uncommitted can help), then runs `make test`, `make ui-shots` and `make site` (every board × app set builds,
   the site stages, and the `wl82loader.bin` scan runs). The PR quotes its last lines. GitHub Actions CI is off the gate
   (Chad, 2026-10-31: Actions minutes on this private repo cost money); `ci.yml` stays for a manual run.
3. **Hardware smoke check** on the Waveshare, with the branch's own build:
   1. Install it the way a user would: the web installer (`make serve`), from Computer mode, with the app set the
      branch changes (MIDI Hub if none).
   2. Put the board back in the rig: hub power brick → hub, hub upstream cable → Waveshare, FM-1 and Launchkey on the
      hub's USB-C ports.
   3. Pass when the settings page shows the branch's commit, the chain screen is all green at `8/8`, and playing the
      Launchkey sounds the FM-1.

There is one Waveshare. A session books it in the shared notes queue before asking Chad to move it, and reflashes the
`master` build when done.

An app also lands with its row in the mode table, its claims and tests in the broker, its NVS namespace with a `schema`
key, and its build flag (`docs/architecture.md`, "How an app lands on `master`"). Guarded work (anything that writes the
FM-1's flash, the partition table or stored data) also gets one high-effort code review before merging.

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
