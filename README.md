# Outrigger

**Outrigger lets a USB MIDI controller play an M-VAVE FM-1 with no computer, and switches the FM-1's firmware for
you, backing it up first.** It runs on a small ESP32-S3 touchscreen board that sits on a powered USB hub next to the
FM-1. Install it from your browser at **[outrigger.music](https://outrigger.music)**.

- **Status: beta.** Tested on a Waveshare ESP32-S3-Touch-LCD-1.69 with a Novation Launchkey Mini MK4 37. Expect
  rough edges; firmware switching writes to your FM-1, so read the beta guide first.
- **Beta guide:** [`docs/beta/`](docs/beta/01-what-you-need.md), from what you need to
  [what to do if your FM-1 won't boot](docs/beta/06-if-your-fm1-wont-boot.md) and the
  [known issues](docs/beta/07-known-issues.md).
- **Feedback:** feedback@outrigger.music.
- **License:** [GPL-3.0](LICENSE). Third-party code and its licenses: [NOTICE](NOTICE). No M-VAVE firmware is in
  this repo; your browser downloads it at install time and checks each file.

The rest of this README is for working on the code.

## For developers

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
| `tests/ui_shots/` | `make ui-shots`: renders every `host_ui` page (docs/ui-redesign.md) with LVGL on the Mac, on 240×280, 320×480 and 480×320 at S/M/L, hub and hub-loader, plus an encoder board's focus ring, using the firmware's own wording. Taps them with a drifting finger, holds the holds, and fails on a clipped or off-glass label. PNGs, `report.txt` and a `sheet_<board>.png` per board in `build/ui-shots/`. Check UI changes here before booking the board. |
| `firmware/config/lv_conf.h` | LVGL configuration (fonts, clib malloc so big buffers can use PSRAM). |
| `site/`, `tools/build_site.py` | Browser installer (`site/installer/`, on esptool-js) and the script that stages firmware into it. |
| `.github/workflows/ci.yml` | CI, the merge gate: the same checks as `make check`, with one job per board × app set, on pushes to master and PRs. Its `ci-ok` check is the one to read. |
| `firmware/waveshare_169/` | The Arduino sketch every board builds (named for the first board): `app.cpp` (USB host, routing, gathers the frame) and `board.cpp` (display, touch and expander drivers, from the build's `board_config.h`). Code stays out of the `.ino` (see below). |
| `firmware/boards.json`, `tools/boards.py` | The board profiles (pins, chips, screen, memory) and the script that checks them and generates each build's `board_config.h` and the screens `make ui-shots` renders. |

To add a board: add its profile to `firmware/boards.json` (`docs/boards.md`, "Adding a board"). `app.cpp`, `host_ui`,
the Makefile, the site and the installer pick it up from there.

## Commands

```
make test      # midi_core, platform and fm1_loader tests on the Mac (fetches fixtures once)
make check     # the full deploy check (critical batches, FULL=1): HEAD checked out clean in the clone all worktrees share, then test, ui-shots and site (~2 min; ~12 min after a core or library update)
make ui-shots  # render and check every page; build/ui-shots/sheet_*.png (needs LVGL 9.6.0, see Makefile)
make fixtures  # FM-1 packages and wl82loader.bin for the tests, pinned by SHA-256 (gitignored)
make golden    # regenerate tests/golden/ from the Unbricker and jl-uboot-tool (Python venv in build/)
make build     # compile one board x app set: BOARD=waveshare_169 (default)|waveshare_35 APPS=hub|hub-loader|everything
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

- **Computer mode (no buttons):** on the board, Setup > **Computer mode**, then hold for 2 s. It restarts with the USB
  host off and hands the port to the ESP32-S3's built-in USB-Serial-JTAG device (`303a:1001`), so `make flash` or the web
  installer can reset it into download mode themselves. The request is kept in NVS (namespace `boot`) with the build's
  ELF SHA, so it survives moving the cable. The first boot of a *different* build clears it and runs the host again;
  "Back to MIDI" clears it by hand. Logic in `lib/midi_core/src/boot_mode.h`, tests in `make test`.
- **Buttons (always works):** connect it to the Mac directly, hold BOOT, tap RST, release BOOT, then `make flash`.
  After a BOOT+RST flash, an RTS reset drops the chip back into download mode; tap RST (or use esptool's
  `--after watchdog-reset`) to start the firmware.

## Web installer

`site/` is a static page that flashes the firmware from Chrome or Edge with its own installer (`site/installer/`) on
[esptool-js](https://github.com/espressif/esptool-js) 0.7.0 (Apache-2.0, vendored in `site/vendor/`, no CDN). Each tab's
install area reads the board, says in plain words what it runs and what stays, then writes, checks and restarts it in
place. There is no erase option.

```
make site     # build every board x app set, then stage each into site/firmware/<board>/<app set>/ with its manifest.json
make serve    # same, then serve on http://localhost:8088 (Web Serial needs https or localhost)
```

- **Four parts, not a merged image.** The page writes bootloader @0x0, partitions @0x8000, boot_app0 @0xe000 and the
  app @0x10000, exactly what `make flash` writes. A merged image from 0x0 would also overwrite NVS (0x9000-0xDFFF), and
  Arduino's `.merged.bin` is padded to 16 MB, so it would wipe every partition. `tools/build_site.py` refuses parts that
  overlap NVS or overflow app0 when staging, and `site/installer/plan.js` checks again before writing: its own list of
  the four offsets (never the manifest's), each part's 4 KiB-rounded end, and the board's current partition table, so an
  install can't move or drop the store. Both run the cases in `tests/installer/tables.json` from `make test`.
- **What the board runs.** Each build carries a 20-byte tag at flash 0x10120 (`lib/platform/src/build_tag.h`): commit,
  dirty flag, app set. The page reads it before installing; `build_site.py` refuses an app without one. Boards running a
  build from before the tag show as "an earlier usb-midi-host build".
- **A store for the loader.** Installing Hub + Loader or Everything onto a board with no store also sets up the store
  `make flash-store` makes, when the board ran other firmware (its partition table has no store, so what's there isn't
  FM-1 backups; factory boards carry Waveshare's demo and its crash dump) or its store region is factory-blank (the
  board's MD5 of the region matches an erased one's). The browser
  downloads `wl82loader.bin` from the pinned jl-uboot-tool commit on GitHub, checks its SHA-256, and copies it into
  `store-template.json` (`tools/store_template.py`, staged beside each loader build's manifest), so this site never hosts
  it. An existing store is never written. Our store partition with data but no readable store stops the install ("Your ESP32 has storage we
  can't read"); a failed download installs everything else and says firmware switching isn't set up.
  `tests/installer/test_store_template.py` mounts the spliced store and reads the loader back.
- **No laptop copy yet.** Saving the store to the laptop from the page waits for the laptop-backups work
  (`docs/firmware-loader-plan.md`, "Laptop backups"): at the 64-byte reads below the 12.1 MB store takes about 8 min in
  the browser (esptool from a Mac: 79 s). `tools/store_dump.py` already turns a raw read of the store partition back
  into 1 MiB FM-1 dumps.
- **Checks.** Each download is checked against the manifest's SHA-256, each written part against the board's MD5 of it.
  Flash mode, frequency and size are kept as built. Afterwards an RTC watchdog reset starts the program.
- **The board's USB port** stalls about 30 s on any single write of 4 KiB or more, so `site/installer/serial.js` sends
  1 KiB at a time. In Chrome and Brave on macOS its longer replies also stop partway (after 616 bytes of a 1 KiB flash
  read, measured 2026-10-31; Node on the same Mac never lost a byte), so reads ask for 64-byte packets, one at a time.
- **Checked on the Waveshare (2026-10-31, installer 2026-10-31.6):** MIDI Hub installed from Chrome from Computer mode and
  from BOOT+RST; both restarted by themselves; the 12.1 MB store read back byte-identical before and after.
- **App sets:** each tab's buttons name an app set (MIDI Hub, Hub + Loader, Everything), each with its own manifest. Hub +
  Loader ships with the Loader's writes on (`WRITES` defaults to 1 for `hub-loader`; `WRITES=0` builds it read-only);
  Everything's loader stays read-only. The settings page shows which set is running next to the commit (`hub`,
  `hub+L`, `all`).
- **Adding a board:** see Layout.
- **Licensing:** our code, LVGL and the vendored EspUsbHost (MIT), Arduino_GFX (BSD) and the Arduino ESP32 core
  (LGPL-2.1) ship as binaries; `make site` copies `NOTICE` to `site/NOTICE.txt` and the footer links it. JieLi's `wl82loader.bin` must
  never be in a published build: `build_site.py` refuses a file with its name or SHA-256, or containing a 1 KiB piece of
  it (when `tests/fixtures/wl82loader.bin` or `$WL82_BLOB` is present), and deletes the staged firmware if so.

### Publishing the site

The site is on Cloudflare Pages (project `outrigger`, a Direct Upload project in Chad's account):

| Channel | URL | Deploys |
|---|---|---|
| production | https://outrigger.music | `origin/master`'s commit only |
| beta | https://beta.outrigger.music | any pushed commit |

```
make deploy-site                     # beta
make deploy-site CHANNEL=production  # outrigger.music
make deploy-site FULL=1              # build it with make check on the Mac instead of taking CI's
```

- **What gets uploaded:** usually CI's build of that exact commit, in about a minute. A push to `master` makes CI build
  every board × app set and keep it as the `site-firmware` artifact (14 days) with a `SHA256SUMS` list.
  `make deploy-site` takes it only from a CI run of exactly HEAD with `ci-ok` green, and checks it three ways: the
  download against GitHub's digest, every file against `SHA256SUMS`, and every manifest against HEAD (clean build, parts
  matching). The rest comes from git at that commit: the pages, `functions/`, `wrangler.toml`, `NOTICE` and `LICENSE`.
  It runs `make check` on the Mac and uploads that build (`build/checked/site`) instead in three cases: `FULL=1`, a
  production batch that touches the critical list's code (`CRITICAL` in `tools/deploy_site.py`), and a commit CI never
  built by itself. That last case is any branch commit, because CI builds a PR merged into `master`. Start CI on the branch by hand
  (`gh workflow run CI --ref <branch>`) to get a build of the commit itself. CI's and the Mac's app binaries
  differ byte for byte (each embeds its own toolchain paths, e.g. `/home/runner/.arduino15/…`); bootloaders, partition
  tables, pages and Functions are identical (compared at d5bc202, 2026-11-03). Nothing uncommitted or unpushed goes
  live. When production may go up without Chad: "Shipping to production".
- **What gets checked first:** `tools/deploy_site.py` refuses a dirty tree, an unpushed HEAD, or production from
  anything but `origin/master` or from a commit beta doesn't serve. Before uploading it scans every file again: under Pages' 25 MiB per-file and
  20,000-file limits, and free of `wl82loader.bin` (by name, SHA-256 and 1 KiB pieces). A local copy of the blob is
  required for that scan.
- **Which commit is live:** each upload carries its commit hash, shown in the Pages dashboard under Workers & Pages,
  then `outrigger`, then Deployments. Every deploy and rollback is also a line in the deploy log,
  `~/Library/Application Support/outrigger/deploys/deploys.jsonl` (commit, Pages deployment ID, time, session), which
  every worktree on the Mac shares. `tail -1` of it, filtered by channel, is what that channel serves.
- **Rollback:** `make rollback-site CHANNEL=production` (or `beta`) puts back the deploy before the live one, in
  seconds and without `make check`; `COMMIT=<sha>` goes back further. Production uses Cloudflare's own rollback (about
  a second, no upload; the old Functions come back with the old site). Cloudflare can't roll back a preview, so for
  beta each deploy keeps what it uploaded (newest 3, beside the log, with SHA-256 sums) and a rollback uploads that copy
  again. A second rollback goes further back, never forward to the build the first one left. It refuses when the log
  has no earlier deploy, when the commit isn't one the log can go back to, and when the channel serves something the
  log didn't deploy (use the dashboard's "Rollback to this deployment" then). Any agent may roll production back the
  moment it's broken, then tell Chad.
- **Board reports:** the installer's "Report this board" pop-up (`site/installer/report_form.js`) posts to a Pages
  Function, `functions/api/report.js`, which writes to D1: `outrigger-reports` for production, `outrigger-reports-beta`
  for beta and previews (`wrangler.toml`, schema in `migrations/`). The function and `wrangler.toml` go up with the
  site, from the same checked commit (`build/checked/`). Read new reports with `python3 -I tools/reports.py new`
  (add `beta` for beta's); `forget <id>` deletes a report's email when someone asks.
- **Credentials:** `~/.config/cloudflare/outrigger-pages.env` holds `CLOUDFLARE_API_TOKEN` (Account, Cloudflare Pages,
  Edit; Account, D1, Edit) and `CLOUDFLARE_ACCOUNT_ID`. wrangler is pinned in `tools/deploy_site.py` and runs through `npx`.
- **Domains:** `outrigger.music` is the project's production custom domain. `beta.outrigger.music` is a custom domain
  whose proxied CNAME points at the beta branch alias, `beta.outrigger.pages.dev`
  ([Cloudflare's branch-domain guide](https://developers.cloudflare.com/pages/how-to/custom-branch-aliases/)). The
  zone's MX, SPF and DKIM records belong to Email Routing (feedback@outrigger.music).
- **One-way choice:** a Direct Upload project can't be switched to Cloudflare building on every push. That would mean a
  new project.

## Web diagnostics

Builds with the Web UI (`everything`, or `hub-loader` with `DIAG_WEB=1`) join Wi-Fi and serve a read-only page at
`http://fm1hub.local` (Setup shows the address). With no network saved they open the `fm1hub-setup`
hotspot (password on Setup); its page saves a network to NVS `wifi`, the only thing any route writes.
Everything else is GET: `/status` (build, heap, each task's unused stack, USB devices with their channels and
refusals, the FM-1 pages), `/log?since=N` (lines from the USB task, the loader task and the loop), `/crash` (the
last abnormal reset), `/coredump` (the raw partition) and `/store`. After a crash, safe mode starts Wi-Fi too, so the
crash can be read without the screen. `tools/diag.py` fetches them from the Mac.

## Merge gate

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

### Shipping to production

Agents ship production themselves (Chad, 2026-11-03) when all of these hold:

1. The commit is `origin/master`'s head, and its CI (`ci-ok`) is green.
2. Beta serves the same commit (`make deploy-site` from it), checked by content: the page and
   `firmware/<board>/<app set>/manifest.json`'s `version` show the commit, and what the batch changed works there.
3. Nothing since the commit production serves is on the critical list. Read the batch with
   `git log --stat <live>..origin/master` (the live commit: the deploy log's last production line).

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
("Publishing the site") with its commit, time and session.

### Boards

There are three: Waveshare-1 (1.69", MAC `a0:85:e3:f0:a2:3c`), Waveshare-2 (1.69", MAC `d8:3b:da:a0:67:d0`)
and Board-3 (Waveshare 3.5", MAC `44:bd:8d:61:2b:28`). The names follow Chad's labels on the boards (checked
2026-11-03; they had been recorded the other way round). A session books a board and the Mac's USB slot through the orchestrator session
(`local_3a47bdc2-23d3-4414-b549-9e1e85c6718a`), checks the board's MAC before flashing, and reflashes the `master`
build when done.

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

## License

GPL-3.0 (`LICENSE`), the license of M-VAVE's `fm1_open`, Felucca, Skiff and SLOOP. `NOTICE` lists the third-party
code inside (MIT, BSD, Apache-2.0, LGPL-2.1), and `make site` serves both beside the firmware as `NOTICE.txt` and `LICENSE.txt`.
