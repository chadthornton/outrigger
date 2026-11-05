# Contributing to Outrigger

Thanks for taking a look. This page covers building and testing the firmware, and how a change gets from a branch to
outrigger.music. Players want the [beta guide](docs/beta/01-what-you-need.md) instead; everything else is in the
[docs index](docs/README.md).

**Found a bug or tried another board?** Open an issue, or email feedback@outrigger.music. Board reports from the
installer's "Report this board" button help too.

**How the work happens.** Most changes here are written by AI coding agents (Claude Code) working for the maintainer,
several at once, each on its own branch. The rules below are written for them and for human contributors alike: the
same gate, the same ship policy. Decisions credited "(Chad, date)" are the maintainer's.

## How it's built

A computer-free USB MIDI host. It runs on the Waveshare ESP32-S3-Touch-LCD-1.69 and 3.5 (`firmware/boards.json`
lists every profile, tested or not). Its USB-C port runs as a USB host through a powered hub, and incoming MIDI is
shown on its screen. The goal is to play an M-VAVE FM-1 from a Novation Launchkey Mini MK4 37 with both on the hub, power and MIDI on one cable each.

It is one shared core with pluggable apps (Router, firmware Loader, and, in progress, Recorder and Web UI), chosen
per build as an app set. The core owns the 8 USB host channels, the FM-1, the radio, storage, settings and the screen;
apps ask it for them. See [docs/architecture.md](docs/architecture.md), and the merge gate below before landing anything.

### Layout

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
| `tests/ui_shots/` | `make ui-shots`: renders every `host_ui` page (docs/ui-redesign.md) with LVGL on the Mac, on 240×280, 320×480 and 480×320 at S/M/L, hub and hub-loader, plus an encoder board's focus ring, using the firmware's own wording. Taps them with a drifting finger, holds the holds, and fails on a clipped or off-glass label. PNGs, `report.txt` and a `sheet_<board>.png` per board in `build/ui-shots/`. Check UI changes here before booking the board. |
| `firmware/config/lv_conf.h` | LVGL configuration (fonts, clib malloc so big buffers can use PSRAM). |
| `site/`, `tools/build_site.py` | Browser installer (`site/installer/`, on esptool-js) and the script that stages firmware into it. |
| `.github/workflows/ci.yml` | CI, the merge gate: the same checks as `make check`, with one job per board × app set, on pushes to master and PRs. Its `ci-ok` check is the one to read. |
| `firmware/waveshare_169/` | The Arduino sketch every board builds (named for the first board): `app.cpp` (USB host, routing, gathers the frame) and `board.cpp` (display, touch and expander drivers, from the build's `board_config.h`). Code stays out of the `.ino` (see below). |
| `firmware/boards.json`, `tools/boards.py` | The board profiles (pins, chips, screen, memory) and the script that checks them and generates each build's `board_config.h` and the screens `make ui-shots` renders. |

To add a board: add its profile to `firmware/boards.json` (`docs/boards.md`, "Adding a board"). `app.cpp`, `host_ui`,
the Makefile, the site and the installer pick it up from there.

### Commands

```
make test      # midi_core, platform and fm1_loader tests on the Mac (fetches fixtures once)
make check     # the full deploy check (critical batches, FULL=1): HEAD checked out clean in the clone all worktrees share, then test, ui-shots and site (~2 min; ~12 min after a core or library update)
make ui-shots  # render and check every page; build/ui-shots/sheet_*.png (needs LVGL 9.6.0, see Makefile)
make fixtures  # FM-1 packages and wl82loader.bin for the tests, pinned by SHA-256 (gitignored)
make golden    # regenerate tests/golden/ from the Unbricker and jl-uboot-tool (Python venv in build/)
make build     # compile one board x app set: BOARD=waveshare_169 (default)|waveshare_35 APPS=hub|hub-loader|everything
make build-all # every board x app set, into build/<board>/<app set>/
make build APPS=hub-loader DIAG_WEB=1  # hub-loader with Remote diagnostics on from the start (build/<board>/hub-loader-web/)
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

### Flashing

Once the firmware runs, the board's only USB port is a host, so the Mac can't reset it. Two ways in:

- **Computer mode (no buttons):** on the board, Setup > **Computer mode**, then hold for 2 s. It restarts with the USB
  host off and hands the port to the ESP32-S3's built-in USB-Serial-JTAG device (`303a:1001`), so `make flash` or the web
  installer can reset it into download mode themselves. The request is kept in NVS (namespace `boot`) with the build's
  ELF SHA, so it survives moving the cable. The first boot of a *different* build clears it and runs the host again;
  "Back to MIDI" clears it by hand. Logic in `lib/midi_core/src/boot_mode.h`, tests in `make test`.
- **Buttons (always works):** connect it to the Mac directly, hold BOOT, tap RST, release BOOT, then `make flash`.
  After a BOOT+RST flash, an RTS reset drops the chip back into download mode; tap RST (or use esptool's
  `--after watchdog-reset`) to start the firmware.

### Web diagnostics

Every build carries the Web UI, off until **Setup > Remote diagnostics** turns it on (`everything` and `DIAG_WEB=1`
builds start with it on; turning it on or off restarts the board). Off, nothing starts, Wi-Fi included. On, the board
joins Wi-Fi and serves a read-only page at `http://fm1hub.local` (Setup shows the address). With no network saved they open the `fm1hub-setup`
hotspot (password on Setup); its page saves a network to NVS `wifi`, the only thing any route writes.
Everything else is GET: `/status` (build, heap, each task's unused stack, USB devices with their channels and
refusals, the FM-1 pages), `/log?since=N` (lines from the USB task, the loader task and the loop), `/crash` (the
last abnormal reset), `/coredump` (the raw partition) and `/store`. After a crash, safe mode starts Wi-Fi too, so the
crash can be read without the screen, when Remote diagnostics is on. `tools/diag.py` fetches them from the Mac.

Compiled in, the Web UI costs about 27 KB of internal RAM even when off (20 KB of it the Wi-Fi driver's own data),
so a board with it off has the fixed cost of `everything` without Wi-Fi running.

The browser installer and how it stages firmware: [docs/web-installer.md](docs/web-installer.md).

## How changes land

### Merge gate

Nothing lands on `master` until **GitHub CI is green** on the PR's head commit: the `ci-ok` check (`gh pr checks`).
It runs `make test`, `make ui-shots`, one build per board × app set in parallel, then stages the installer site with
the `wl82loader.bin` scan. A PR that only touches docs, site copy (`site/`), `tests/installer/` or `.md` files skips
the firmware builds and gets a licensing scan of `site/` instead. Run `make test` locally while working; it is not a
gate step, and neither is `make check` (Chad, 2026-11-01). A briefly broken `master` is tolerated: fix forward.

Agents merge their own PRs on green CI, unless the PR is on the critical list below; a critical PR also waits for
Chad's "ship it" before it reaches production (Chad, 2026-11-03).

**Exception:** a PR that changes the FM-1 loader, the store or flash writes, or the USB host stack (`lib/fm1_loader/`,
the loader and store code in `firmware/`, `partitions.csv`, `lib/EspUsbHost/`) also passes the hardware smoke check
below with its own build before merging.

### Review

How much review a change gets follows what a mistake would cost:

- **Light:** UI, layout, copy, read paths, test tooling. No review round before merging; check it on the preview site or
  the board. A light change that needed a rollback moves that kind of change to guarded.
- **Guarded:** anything a rollback can't undo. That means writing the FM-1's flash, the partition table or stored data, deploy
  tooling, server code and secrets. Plan first, tests first (a bug fix comes with a test that fails without it), then one
  high-effort code review round before merging.

An app also lands with its row in the mode table, its claims and tests in the broker, its NVS namespace with a `schema`
key, and its build flag ([docs/architecture.md](docs/architecture.md), "How an app lands on `master`").

### Shipping to production

Agents ship production themselves (Chad, 2026-11-03) when all of these hold:

1. The commit is `origin/master`'s head, and its CI (`ci-ok`) is green.
2. Preview (preview.outrigger.music) serves the same commit (`make deploy-site` from it), checked by content: `/version.json`'s `site` shows the
   commit, `firmware/<board>/<app set>/manifest.json`'s `version` shows `/version.json`'s `firmware` (the same commit
   unless the deploy was pinned, [docs/publishing.md](docs/publishing.md)), and what the batch changed works there.
3. Nothing in the batch is on the critical list. The batch is firmware changes since production's firmware commit and
   everything else since its pages' commit (the same unless production's last deploy was pinned): read
   `git log --stat <firmware>..origin/master` for `firmware/`, `lib/` and the build files, and
   `git log --stat <site>..origin/master` for the rest (production's `/version.json`, or the deploy log's last
   production line: `commit`, and `firmware` or `commit` if it has none).

Then `make deploy-site CHANNEL=production`, check outrigger.music the same way, and say what shipped in the
session's chat. A broken production gets `make rollback-site CHANNEL=production` first and questions after.

**The critical list.** A batch with any of these waits for Chad's "ship it" in the deploying session's chat:

- the FM-1 loader, the store, flash-erase ranges, or the USB host stack (`lib/fm1_loader/`, the loader and store code
  in `firmware/`, `partitions.csv`, `lib/EspUsbHost/`);
- new data collection, privacy copy, emails, accounts or secrets;
- deploy tooling or server code (`tools/deploy_site.py`, `tools/check.py`, the Makefile's `check`, `deploy-site` and
  `rollback-site`, `.github/workflows/`, `functions/`, `wrangler.toml`, `migrations/`);
- releases, tags and announcements.

If a critical batch changes what the firmware does, it also passes the **hardware smoke check** with `master`'s build
before the "ship it":

1. Install it the way a user would: the web installer (`make serve`), from Computer mode, with the app set the
   ship changes (MIDI Hub if none).
2. Put the board back in the rig: hub power brick → hub, hub upstream cable → Waveshare, FM-1 and Launchkey on the
   hub's USB-C ports.
3. Pass when Setup shows the commit, Play's four lines are green with the hub at `USB 8/8`, and playing the
   Launchkey sounds the FM-1.

Non-critical batches skip the smoke check. Every production deploy and rollback lands in the deploy log
([docs/publishing.md](docs/publishing.md)) with its commit, time and session.

### Test hardware

Test hardware is referred to by name: **Small-V1** (Waveshare 1.69", the older revision), **Small-V2** (1.69" with
the V2 sticker), **Big-S3** (Waveshare 3.5" S3), **Big-P4** (the Synth track's 3.5" P4), and the FM-1s **FM-A** and
**FM-B**. The names map to MACs and unit IDs in a local device registry outside this repo; its shape is
`tools/devices.example.json`. Builds with the Wi-Fi diagnostics (`DIAG_WEB=1`) read the ESP32s' names from it at build
time (`tools/devices.py header`); release builds carry no MACs, and `tools/build_site.py` refuses one that does.

- **Tools ask for a device by name** and refuse any other: `tools/port_for.py Small-V2` finds its port,
  `tools/wait_for_board.sh Small-V2` waits for it, and `tools/bench.py --board Small-V2 --fm1 FM-B` drives the bench.
- **Wiping any of them is fine,** FM-1s included (2026-11-03). The check only makes sure a session has the device it asked for.
- **Every esptool MAC read ends with a hard reset,** so no board, another session's included, is left in download mode.
- A session books a board and the Mac's USB slot through the orchestrator session, and reflashes the `master` build
  when done.
- **On Wi-Fi:** Small-V1 (`small-v1.local`, hub A with FM-A), Small-V2 (`small-v2.local`, hub B with FM-B) and
  Big-S3 (`big-s3.local`) run the Wi-Fi diagnostics build. Reading their status, logs and crash reports needs no
  booking; switches do (`docs/test-bench.md`, "Using the lab boards").
