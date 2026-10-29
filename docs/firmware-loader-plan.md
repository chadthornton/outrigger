# Plan: on-device FM-1 firmware loader (guarded)

Status: **approved by Chad, 2026-10-26** ("let's proceed"). Session 1 (the Mac-tested core) is built; see "Session 1:
done" below. Decisions:
- **Save states:** the defaults (decision 0).
- **v1 packages:** from the Mac.
- **First firmwares:** stock V15, Skiff, SLOOP.
- **Loader blob:** `wl82loader.bin` is fetched at setup and kept on device storage, never committed.
- **Storage layout:** the measured one (decision 5): one shared sector store plus a recovery app for self-updates.
  The storage back end stays abstract, so a microSD card (the 1.47" Waveshare has a TF slot) can replace internal
  flash in session 2 without touching the core.

**Session 1: done (2026-10-26), Mac only, no hardware.** `lib/fm1_loader/`, tested by `make test`:
- `.fwsc` and UFW parsing (`package.*`), checked against fm1_unbrick.py's output for stock V15, Skiff 1.4 and
  SLOOP 2.8.1, and against damaged copies.
- The address guard (`layout.*`): one gate with three rights (app, save restore, settings erase).
- The shared sector store, units, save states and the default restore rules (`store.*`, `library.*`).
- The ROM-boot client (`rom.*`) and the install state machine with its resume entry (`installer.*`).
- **Golden vectors** (`make golden`, `tools/gen_golden.py`): the Unbricker's own code and jl-uboot-tool's classes run
  against the Unbricker's CDB-level `MockScsi`. The C++ state machine, against a C++ port of that mock, reproduces
  every SCSI command (CDB, direction, length, payload hash) and the final flash of eight scenarios: install, no-op,
  a retry that clears, one that doesn't, wrong chip key, wrong flash ID, backup reads that disagree, and resume.
- **Fixtures** (`make fixtures`): the three packages and `wl82loader.bin`, pinned by SHA-256, gitignored.

What session 1 settled or changed:
- **Reset is `0xFC0C` with argument 1** (jl-uboot-tool's `reset` = `run_app`), not `0xFC02`, which is the V1
  loader's reset. The wl82 runs the V2 loader. The Unbricker itself never resets; it has the user power-cycle.
  Whether `0xFC0C` really restarts the FM-1 into its firmware is a checkpoint 1 question.
- **Per-sector writes:** the Unbricker erases and rewrites the whole app area. The loader uses the Unbricker's own
  erase, write and read functions per differing 4 KiB sector, with one retry per sector.
- **No retained-RAM read:** the Unbricker reads 512 bytes of RAM (`FD07`) before uploading the loader, for crash
  diagnosis. The loader leaves it out.
- **The loader is uploaded as is:** the wl82 ROM unscrambles MengLi itself, so jl-uboot-tool's scramble condition is
  false for this chip (ported and tested both ways).
- **Stock V15's data map stays unknown.** Its store is the whole VM `0x93000–0xE8FFF`, with factory data in
  `0x93000` and an update record at `0xE8F00`, but which sectors hold what isn't mapped. So stock saves stay
  disabled, and a switch to stock keeps the data in place with the layout warning. It never erases `0x93000`.
  Mapping it needs a stock unit's dumps before and after changing settings (checkpoint 4).
- **Resume** reads the unit's sector, requires its stored verified backup and an unfinished install recorded before
  the first erase, then reads the flash once and finishes that install. It never resets the FM-1 on a failure.

**Session 2 (approved by Chad 2026-10-27: "start loader session 2 … ROM-boot transport, on-device storage,
checkpoint 1 read-only").** Branch `loader-session2`. Nothing is written to the FM-1 in this session.

1. **Partition table (decision 5, exact numbers).** One table for every app set, in `firmware/waveshare_169/partitions.csv`:

   | Name | Type | Offset | Size | Why |
   |---|---|---|---|---|
   | `nvs` | data/nvs | `0x9000` | `0x5000` | unchanged, so settings survive |
   | `otadata` | data/ota | `0xE000` | `0x2000` | unchanged; the installer's `boot_app0` lands here |
   | `app0` | app/ota_0 | `0x10000` | `0x300000` (3 MB) | the main app, at the address every installer writes |
   | `recovery` | app/ota_1 | `0x310000` | `0xC0000` (768 KB) | reserved for the recovery app; empty for now |
   | `store` | data/littlefs | `0x3D0000` | `0xC20000` (12.1 MB) | the shared sector store |
   | `coredump` | data/coredump | `0xFF0000` | `0x10000` | as before |

   - **What changes on the board:** today's table (Arduino `default.csv`) has the same `nvs` and `otadata`, so the
     settings survive. The old 1.25 MB `app1`, `spiffs` and `coredump` bytes now lie inside `store`.
   - **Never auto-formatted:** `store` is mounted with format-on-fail off. An unreadable store says so on screen.
   - **Size limit:** the build's `upload.maximum_size` becomes the 3 MB slot. `build_site.py`'s guard still holds.
   - **A test:** the built table must keep these invariants.
2. **Storage back end.** `PosixStorage` (in `lib/fm1_loader`):
   - **How it works:** keys are paths under a root directory. Each write goes to a temp file, is fsync'd, then
     renamed over the old one, so a torn write is never visible. On the device the root is LittleFS mounted at
     `/store` through the VFS. On the Mac it's a temp directory, so the same code is tested.
   - **Tests:** the library suite runs against both `MemoryStorage` and `PosixStorage`.
   - **File names:** LittleFS names are at most 64 characters per path component, so a sector lives at
     `s/<2 hex>/<62 hex>`.
3. **Transport.**
   - **A new raw command:** the vendored EspUsbHost gets a public `mscRawCommand(address, cdb, …)`. It makes no
     LUN, inquiry or capacity calls, always reads the status (CSW), and returns the bytes actually received.
   - **Recovery:** on failure it clears halted pipes only. It never sends a mass-storage reset or REQUEST SENSE,
     which the JieLi ROM has never been shown to handle.
   - **On attach:** EspUsbHost sends nothing beyond enumeration; every mass-storage command is on demand.
   - **Where it runs:** a loader task, because the library refuses mass-storage calls from its own USB task, and
     LVGL may only run in `loop()`.
4. **Key entry, ported from the Unbricker's `softkey`:**
   - **Identity first:** the identity query `F0 00 32 45 00 00 00 40 7F F7`, up to 3 tries with 1 s each.
   - **Refused:** no answer, an `ota-…` identity, or anything that isn't `FM-1_NNN`.
   - **The key:** one 8-byte USB transfer `04 F0 22 24 07 35 7D F7`, sent 3 times 20 ms apart.
   - **Then:** wait up to 15 s for `4C4A:8057`.
   - **Who may send it:** only the Loader, in Load mode, when the broker says it owns the FM-1's MIDI.
5. **Read-only lock.** `RomClient` refuses erase and write unless the build defines `FM1_LOADER_WRITES=1`.
   - **Where it's on:** only in the Mac tests. Every firmware build is locked until checkpoint 3.
   - **Proof:** a locked test binary runs a whole install against the mock and checks that no `FB01`/`FB04` is
     ever sent.
   - **ROM boot at power-on:** a locked build that finds an FM-1 already in ROM boot shows it and offers a reset,
     never a resume.
6. **"Back up this FM-1"** (`Installer::start_backup`). The steps: confirm, key, identify, loader, IDs, the double
   read, then record the backup and the autosave and reset the FM-1. It has its own golden scenario. Checkpoint 1
   runs this, and it's also the plan's "Save now".
7. **Firmware page (hub-loader build).**
   - **Reached from:** Settings.
   - **Shows:** the store's state, whether the loader blob checks out, the units, a "Back up FM-1" hold, progress,
     and the result.
   - **Hashes:** the result shows the hashes of the boot head and of `0xE9000–0xFBFFF` against the 2026-10-25 dump.
   - **Mode switching:** entering Load mode restarts the USB host (`usb.end()`/`begin()`, or `esp_restart()` if
     devices don't re-enumerate). Leaving it does the same.
8. **Getting the blob onto the board.**
   - **`make store-image`:** builds a LittleFS image of the `store` partition on the Mac with the loader blob in it.
     It's gitignored and never staged by `make site` or CI.
   - **`make flash-store CONFIRM=erase-store`:** flashes that image. It's for first setup only, because it erases
     every backup in the store.
9. **Checkpoint 1.**
   - **Dry run first, no FM-1:** the store mounts, the blob checks out, Load mode times out cleanly, and Play comes
     back at 8/8.
   - **Then with Chad, read-only:** back up the FM-1 on the hub. Pass when the boot head and `0xE9000–0xFBFFF` match
     the 2026-10-25 dump, if it's the same unit.
   - **It answers the open questions** listed under "On hardware".
10. **Merge:** the README gate, plus `code-review` at high effort (partition table and stored data).

The firmware browser and the web install page with laptop backups follow in their own session, after this merges.

**Session 3 plan (draft, needs Chad's go-ahead).** Session 2 merged as `6a2fcd4`. Three things stand between it and a
web install page:
- **Writes are locked**, and the plan puts checkpoints 2 and 3 before any unlocked build.
- **The Web UI** is only on `spike/web-ui`, with no radio manager on `master`.
- **`flash-store` wipes backups**, so packages need another way onto the board.

So it splits in two.

- **3a. On-device firmware browser, then writes (this session, with Chad at checkpoints 2 and 3):**
  1. **`make store-add PKG=…`:** adds packages without losing backups. It reads the `store` partition back
     (`esptool read-flash`), mounts it with littlefs-python, and adds each `.fwsc` through the same `Library` format
     (a Python port of `add_package`, checked against the C++ on the fixtures). It writes the image back only if the
     store's manifests are unchanged since the read. The board must be in Computer mode.
  2. **A Firmware browser** on the Firmware page:
     - **What it lists:** library packages with their identity, name and version.
     - **Marks:** which one each unit runs, and a favorite pin.
     - **Install:** an "Install" hold, showing the plan before the hold: N sectors, and keep, restore or fresh data,
       with the layout warning.
     - **Locked builds:** the hold stays off ("read-only build").
  3. **Checkpoint 2, no-op:** a build with `FM1_LOADER_WRITES=1` installs Skiff over Skiff. The plan must show
     0 sectors, and nothing is erased.
     - **Passed 2026-10-27** (build `b81773c`, writes on). After a fresh backup, the browser marked Skiff 1.4 "running"
       (all 143 backed-up app sectors equal the package's) and previewed 0 sectors with data kept. The install wrote
       nothing, and the FM-1 came back on its own.
     - **Found on the way:**
       - **A reboot loop with packages in the store:** the loop task's 8 KB stack overflowed in LVGL's redraw. Found
         from the core dump on the `coredump` partition (`esp-coredump`). The loop task now has 16 KB, and the page's
         data is off its stack.
       - **Allocation inside a critical section:** now fixed.
       - **Crash breadcrumbs and a safe mode:** after a crash the board starts without the USB host, shows where it
         died, and hands the port to the Mac.
       - **store-add mounts by version:** the board had upgraded its littlefs to disk version 2.1, so store-add finds
         the version by mounting instead of parsing.
  4. **Checkpoint 3, real install:** Skiff → Felucca 1.6.1 → Skiff, with the Transporter wired and ready.
     - **Why not SLOOP (Chad, 2026-10-27):** SLOOP is Felucca 0.9-based, and Skiff (1.6.1) over 0.9 data is the
       known brick pattern. SLOOP waits until store dumps (`tools/store_dump.py`) show where it keeps data outside
       the Felucca table. This includes the
     save states (a Skiff project survives the round trip).
  5. **Then:** `hub-loader` ships with writes on, and the README and site say so. Code-review at high effort.
- **Checkpoint 3, first attempt (2026-10-27): stopped safely.**
  - **What happened:** Skiff → Felucca 1.6.1 stopped with "the FM-1 stopped answering" during the write phase. The
    FM-1 restarted on its own, and wasn't reset by the board.
  - **Then:** a key on the next attempt didn't bring up a ROM device, and the FM-1 went dark. After a power cycle, a
    backup showed the app area byte-for-byte Skiff, so nothing had been written.
  - **Suspects:** the FM-1's battery (erase current), or a loader or USB fault that the screen can't name.
  - **Before retrying:**
    - Charge the FM-1 fully.
    - Get logs from `web-diag`, the session building read-only `/status`, `/log`, `/crash` and `/coredump` pages on
      branch `web-diag` off `loader-session3`.
    - Make the failure name its command, address and sector count.
  - **Also fix:** `status.cpp`'s fallback line "It may not be class-compliant", which showed for a Launchkey whose
    MIDI simply didn't reopen after many host restarts.
- **Checkpoint 3, second session (2026-10-27/29, builds `c42d5f0` to `b4a4d71`):**
  - **Leg 1 done, Skiff → Felucca 1.6.1, but through the retry.** The board booted with the FM-1 in ROM boot and
    found the first attempt's pending record. The top row's hold became "retry the install" while the row still
    read "Back up this FM-1", and Chad held it as a backup. It ran the first attempt's confirmed plan (fresh start,
    143 + 6 sectors) as a resume. The Skiff saves stayed in the store. Fixed: the row now reads
    "Retry the … install", and `/status` shows the top row, quoted to Chad before every hold.
  - **Leg 2 stopped twice before any erase:** the FM-1 left ROM boot on its own about 8.5 s after the loader
    started (two backup reads, then the plan). A resume reaches its first erase 3.3 s in and wrote for 9 s without
    a stop. Leading hypothesis: a loader watchdog that erases and writes feed and reads don't. Fixed in `d860144`:
    an install plans from the stored backup before the key, then reads once (compared with that backup) and
    erases, with a 6 s budget. A stop before any erase leaves no danger flag and no pending record.
  - **The board's USB host failed after in-place restarts:** an ESP-IDF `hcd_urb_dequeue` assert (twice, with app
    hub requests in flight), ROM devices never seen, devices not re-enumerating, and once Wi-Fi gone too. Every
    first job after a fresh boot worked. Fixed in `74033a9`: a job is its own boot (RTC `JobBoot`, then
    `esp_restart`), and loader builds send no hub requests. The log tail now survives any reset (`/crash`).
- **Left in session 3a, in order:**
  1. ~~The failure detail and the status line.~~ Done in `605c522`.
  2. ~~Merge `web-diag`.~~ Done (`c42d5f0`).
  3. Checkpoint 3, leg 2: Felucca → Skiff on `74033a9` or later (backup, preview "Restores its saves", hold).
  4. `code-review` at high effort. The first round (9 findings) covered up to `d2284ba`; everything since is
     guarded and unreviewed.
  5. Merge `master` (ui-polish rewrote `host_ui`; the Firmware page section conflicts), then the merge gate.
  6. Merge with `hub-loader` writes on.
  - **Open causes:** the loader watchdog (confirm by timing a backup that stops reading for 9 s), and the USB host
    after `usb.end()/begin()` (PR #2's session suspects app hub requests colliding with ESP-IDF's hub driver on
    EP0).
- **Long term, in order:**
  1. **3b:** the Web UI with install requests and laptop `.fm1bak`/`.fm1save` export and import, building on
     `web-diag`.
  2. **SLOOP and other Felucca 0.9 firmwares:** diff store dumps for data outside the Felucca table first.
  3. **Store packing:** per-sector files cost about 2.6× space; 4 packages took 4.3 MB.
  4. **The stock V15 data map.**
  5. **Catalog downloads over Wi-Fi.**
  6. **The recovery app and self-update.**

- **3b. Web UI app and web install (its own session after 3a):**
  - **The Web UI first:** land the Web UI as an app from `spike/web-ui`, with the radio manager, pairing, the `web`
    and `wifi` NVS namespaces, and the 3 MB slot that's now there.
  - **Then the pages:** firmware list and install requests, which the device still confirms with its own hold;
    `.fm1bak`/`.fm1save` export and import; uploads of non-redistributable packages.
  - **Catalog downloads:** need the Wi-Fi lease.
- **Follow-up:** the store's per-sector files cost about 2× space, so pack them before the library grows past a
  handful of packages.

**Decision for Chad:** start 3a now (browser, `store-add`, then checkpoints 2 and 3 at the board), or land the Web UI
first.

**Checkpoint 1: passed, 2026-10-27** (Chad at the board; build `e848423`, read-only). On the recovered unit:
- **Result:** "Backed up. Nothing written."
- **Same unit:** unit `69bb070aaa87bd10`, the dump's.
- **Untouched areas:** the boot head (`d9f43191…`) and the Bluetooth data plus factory voices (`70a8f5be…`) equal
  the 2026-10-25 dump.

What it answered:
- **Skiff accepts the ROM-boot key:** it answered the identity query as `FM-1_714`, took the key and entered ROM
  boot.
- **EspUsbHost's attach is harmless:** the ROM device took the loader, the IDs and two full 1 MiB reads through
  `mscRawCommand()`.
- **`0xFC0C` with argument 1 restarts the FM-1 into its firmware:** the job only reaches "done" once the FM-1 is back
  as a MIDI device.
- **Store:** it mounted with the image's geometry, passed the rename-over self-test, and found the blob.
- **Still unknown:** what inquiry reports once the loader runs, and the loader's USB buffer size. Neither is shown on
  screen yet; the resume path needs the first.

Found on the way:
- **A timing bug, fixed:** EspUsbHost reports a device as connected before its interfaces are claimed, so the first
  attempt sent nothing ("couldn't send to the FM-1"). The loader now waits for MIDI-ready and broker ownership.
- **An intermittent ROM-boot miss:** on one repeat run the FM-1 took the key (screen dark), but no usable ROM device
  appeared within 15 s. The suspect is the broker still counting the FM-1 that had just left. `536ee16` restarts the
  host once in that case and shows the broker's refusal.
  - **Next run:** passed with no re-enumeration needed ("Backed up", both hashes "= dump").
  - **Still open:** the cause is unconfirmed until the miss happens again with that build.
  - **The store:** didn't grow, because the second backup's sectors were already stored.
- **Store overhead, a follow-up:** the first backup used about 1.7 MB of the store. A file per 4 KiB sector costs
  LittleFS a block of metadata as well as its data. Packing sectors into larger files would roughly halve that, and
  matters for the "about 24 packages" budget.

**Measurement for later:** packages are encrypted with a scramble that depends on flash address, so identical code
at a shifted address shows up as different bytes. Decrypting before sharing sectors and compressing may save far
more than the 1.41× measured on the encrypted bytes. Measure it with `tools/catalog_dedupe.py` after decryption is
ported.

## Goal

The Waveshare holds a library of FM-1 firmwares and installs one onto a **working** FM-1 plugged into the hub, with
no computer involved. It also does two more things:
- **Save states:** it keeps each firmware's songs and presets per FM-1, so switching back to a firmware brings your
  work back.
- **More than one FM-1:** it tells your FM-1s apart from the start, so one Waveshare serves both of Chad's units.

**Not in scope for v1:**
- **Reviving a dead FM-1:** that needs the USB_KEY power-on pulses, which can't pass through a hub. The XIAO
  Transporter stays the recovery tool.
- **Writing the boot head**, ever.
- **Wi-Fi downloads:** that's v2.

## Why ROM boot, not the web-installer path

FM-1 firmware gets installed one of two ways (details in the recovery project's `FM1-LOADERS-NOTES.md`):

1. **USB-MIDI SysEx to the running firmware**, which stages the update and applies it after a restart. An
   interruption between those two steps is what bricked Chad's unit.
2. **The JieLi ROM bootloader** (`4C4A:8057`, a USB mass-storage device that takes vendor SCSI commands):
   - **How it writes:** after a small loader is uploaded to RAM, flash is written directly.
   - **Why it's safer:** an interrupted write is fixed by running it again, as long as the ROM bootloader can still
     be reached.
   - **Track record:** Quixotic7's Unbricker and the Transporter use it, and it recovered Chad's unit on 2026-10-25.

The loader uses path 2. It enters ROM boot with the SysEx key `F0 22 24 35 7D F7`, which only a working FM-1 accepts
and which passes through the hub.

**Alternative considered: the standard update protocol (path 1).** Every normal installer uses it: stock M-UPGRADE,
the Felucca-family installers, and The Firmware Hub. It's documented in
[AL-255/FM-1-RE `11-ota-protocol.md`](https://github.com/AL-255/FM-1-RE/blob/main/docs/io/11-ota-protocol.md) and
[Felucca `tools/fm1_install.py`](https://github.com/hugelton/Felucca/blob/main/tools/fm1_install.py).
- **How it works:**
  - The host sends the handshake `F0 00 32 45 00 00 00 40 7F F7`, then the upgrade key `F0 22 24 35 7F F7` (ending
    `7F`, not the ROM-boot `7D`).
  - From then on the device drives: it asks for ranges of the package, and the host answers 7-bit-packed frames.
  - The FM-1 re-enumerates twice, as `4D4A:4155` "ota-FM-1" or `1209:0002` "Felucca Update". It stays a USB-MIDI
    device throughout.
- **Its appeal:** the host side is about 150 lines, needs no loader blob, and works with any package that carries its
  own loader.
- **Why it isn't v1:**
  - Its applying step is the one that bricked Chad's unit when interrupted.
  - It writes nothing the host can verify.
  - It can't take a backup.
- **Where it might return:** as a later "compatible" mode for packages the ROM-boot path rejects, once v1 is
  proven.

## Safety rules (each one enforced in code and covered by a test)

1. **Address guard.**
   - **Allowed:** erase and write only inside the app area, `0x4000–0x92FFF`.
   - **Refused:** any request touching `0x0000–0x3FFF` (boot head, unrepairable), or the Bluetooth data and factory
     voices at `0xE9000–0xFBFFF`.
   - **Optional repair:** the settings areas `0x93000–0xE8FFF` and `0xFC000–0xFEFFF` may be erased only through an
     explicit "erase settings" choice.
2. **Identity before anything else.** All of these must check out before any erase; otherwise abort and reset the
   FM-1 untouched:
   - SCSI inquiry reads `WL82` / `UBOOT1.00`.
   - The flash loader blob matches its SHA-256 (`d41da612…ca5376`).
   - The flash ID is `0x856014`.
   - The chip key is `0x980F`.
3. **Backup first, verified.**
   - **Taking it:** read the full 1 MiB twice and compare the two reads.
   - **Storing it:** save it to the Waveshare's storage under a unit ID derived from the per-unit Bluetooth data at
     `0xE9000`.
   - **Hard rule:** no write without a verified backup of *this* unit.
4. **Package checks.** The `.fwsc` package must pass all of these:
   - It parses.
   - Its UFW header and entry-list CRC-16s are correct.
   - Its `flash.bin` reaches `0x93000`.
   - Its identity string is a known `FM-1_NNN`.
   - It matches the SHA-256 recorded when it was added to the library.
5. **Write only what differs, then read it back.**
   - **Per 4 KiB sector:** erase, write, read back, compare. Retry once.
   - **On a second failure:** stop and keep the FM-1 in ROM boot.
   - **Screen:** "Don't unplug: retry or restore backup".
6. **Data-layout warning.** If the firmware family changes (stock V15 ↔ Felucca family, or Felucca 0.9 ↔ 1.0
   lines), warn that the old settings can crash the new firmware, and offer the settings erase from rule 1. This
   follows the Unbricker's `REPAIR_WINDOWS`.
7. **Loader mode is separate from routing.**
   - **Routing off:** routing stops while the loader runs.
   - **Router unchanged:** it still drops every SysEx.
   - **The one sender:** the SysEx key lives only in the loader module.
   - **Confirmation:** sending it needs an explicit confirm on screen, a 2-second hold, never a single tap.
8. **The remaining risk, stated plainly.**
   - **When it's dangerous:** if the FM-1 powers off mid-write, its app area is incomplete and the SysEx key no
     longer works.
   - **Recovery:** that unit then needs the Transporter, plus the backup this loader took.
   - **Mitigation:** the screen says so before the write starts. The FM-1 runs on its battery, so a hub power blip
     alone doesn't reset it.

## Units and save states

### Telling FM-1s apart

In normal MIDI mode the two FM-1s look identical: same VID:PID, same name, no USB serial number. During ROM boot the
Waveshare reads the whole flash, including the per-unit Bluetooth data at `0xE9000`, which no installer ever writes.
- **Unit ID:** a hash of that sector.
- **Naming:** the first time a unit is seen, the Waveshare asks for a name (A / B, or a short label picked on screen).
  A sticker on each FM-1 that matches helps.
- **Per unit:** everything is keyed by the unit ID: its baseline backup, its save states, its installed-firmware
  history.
- **One at a time:** the loader works on one FM-1 at a time. With two attached, the screen asks you to unplug one. Two
  FM-1s alone on the hub would fit the 8 USB channels, but the loader keeps it simple.
- **During routing:** the units still can't be told apart in normal MIDI mode, so the chain screen shows the name
  of the unit last identified on that hub port. It marks the name as a guess until the next ROM-boot visit confirms
  it.

### What a save state is

A save state is one firmware's user data, taken from one unit. It's saved per firmware family, because each family
stores data in its own places. Felucca's own source gives the Felucca-family map (verified:
`firmware/src/storage.c`, `firmware/loader/ldr_core.c`):

| Range | Holds | In a save state? |
|---|---|---|
| `0x97000–0x9EFFF` | Projects / songs | Yes |
| `0xA0000–0xDBFFF` | User sample slots | Yes |
| `0xDC000–0xDFFFF` | User preset banks | Yes |
| `0xE0000–0xE4FFF` | Update staging: update loader and record | **Never.** Restoring a stale update record could start an update. |
| `0xE5000–0xE6FFF` | Autosave A/B | Yes |
| `0xE7000–0xE7FFF` | Voice bank | Yes |
| `0xFC000–0xFEFFF` | Settings / globals | Yes |

- **Stock V15:** keeps its settings store in `0x93000–0xE8FFF`, with its own update record at `0xE8F00`. Its exact
  map gets worked out in session 1, from the stock firmware and the Unbricker's notes, before stock save states are
  enabled. Until then, stock installs keep or erase data but never save or restore it.
- **Size:** most of these sectors are blank (`0xFF`), so a save stores only the non-blank 4 KiB sectors and their
  addresses. A typical save is tens of KB. The worst case, with full sample slots, is about 360 KB.

### Switching firmware, with save states

1. **Identify:** enter ROM boot and identify the unit, as before.
2. **Autosave:** save the current firmware's data as a state for (unit, firmware identity, app hash), and fsync it.
3. **Install:** write the new app area, as before.
4. **Pick the data the new firmware starts with:**
   - **Same identity and same app hash** as a saved state: restore it. That's your songs and presets back.
   - **Same firmware, newer version (an update):** keep the data in place, as the official installers do, which
     write only the app area. The autosave from step 2 is the rollback point.
   - **Same identity, an older save being brought back** (rollback): restore the save made just before the update.
   - **No saved state:** erase that family's data ranges for a clean start, after confirming. This also prevents the
     data-layout crashes seen between firmware families.
5. **Restart the FM-1.**

**Manual saves.** "Save now" works without changing firmware: ROM boot, read, restart. It takes about 30 s, and the
FM-1 restarts.

**Later option.** Felucca-family firmware has its own backup protocol over USB-MIDI (`fm1backup.js`, commands 65/66/67,
used by Melodee). It could make quick saves without a restart. It's unverified, so it isn't planned yet.

### Rule changes this needs

The address guard (safety rule 1) gains one more allowed writer:
- **Who:** the save-state restore may write a family's data ranges (from the table above).
- **What it writes:** only bytes that came from a save of the same unit and the same firmware identity.
- **Never:** the update staging range, the boot head, Bluetooth data and factory voices.

## Keeping firmwares current

The ecosystem moves fast: the catalog was refreshed today and lists 102 firmwares. The Waveshare treats
[The Firmware Hub](https://thefirmwarehub.com/)'s `catalog.json` as its source of truth.

**What the catalog gives** (checked 2026-10-26). Each entry has:
- `id`, `name`, `author`, `version`, `date`, and a `beta` flag;
- `pkg` (the download path), `size` and `sha256`;
- `ident`, a pattern for the identity string the firmware reports, e.g. `FM-1_97[0-689]`;
- `lic`, `repo`, `release`, and provenance fields (`provCheck`, `relCheck`).

The top level has `updated` and `stock`. The stock V15 package comes with its Apache-2.0 notice.

**One catch:** only the latest version of each firmware is listed. Older versions aren't guaranteed to stay
downloadable, so rollback relies on the Waveshare keeping the previous package locally.

### Checking for updates (v2, Wi-Fi)

1. **Fetch the catalog:**
   - **When:** on demand from the Firmware page, and at most once a day in the background.
   - **Only when changed:** the request uses `If-Modified-Since`, so an unchanged catalog costs one request.
2. **Compare against what the Waveshare knows:**
   - **Installed:** each known FM-1's installed identity and app hash are matched against the `ident` patterns, which
     finds that unit's catalog entry.
   - **Updates:** a newer `version` or `date` for a firmware in the library or on a unit shows as "Update".
   - **New:** catalog entries added since the last check show as "New", using `added`.
3. **Badges:** the chain screen shows a small "2 updates" badge. Nothing ever downloads or installs on its own.

### Downloading a package

1. **Fetch** `https://thefirmwarehub.com/<pkg>` over HTTPS into a temporary file.
2. **Verify the size and SHA-256** against the catalog. A mismatch discards the file.
3. **Run the package checks** (safety rule 4): `.fwsc` parse, UFW CRCs, `flash.bin` reaches `0x93000`, and the
   identity matches the entry's `ident`.
4. **Add it to the library,** and record its source, version, license and hash.

### Updating a firmware on an FM-1

An update is a firmware switch where the identity stays the same:
1. **Autosave** the unit's data, tagged with the old version: the rollback point.
2. **Write the new app area.**
3. **Keep the data in place** and restart. Updates within a firmware are meant to keep songs and presets, and the
   Felucca loader itself never touches user data.
4. **Rollback** is one tap: reinstall the previous package, kept locally, and restore the pre-update autosave.

### Library housekeeping

- **Space:** the shared store holds about 24 typical packages alongside backups and saves (decision 5).
- **Pinning:** keep favorites, and whatever is installed on a unit, permanently.
- **Previous versions:** keep one previous version per installed firmware for rollback.
- **Eviction:** when space runs out, evict the least recently used unpinned package.
- **Betas:** hidden unless "Show betas" is on.

### Trust, stated plainly

- **What the SHA-256 proves:** the download matches the catalog. It doesn't prove the catalog is trustworthy. The
  Firmware Hub is a third party.
- **Mitigations:**
  - HTTPS only.
  - Each firmware's author, repo, license and provenance fields are shown before installing.
  - The ROM-boot path never touches the boot head, so a bad app can always be overwritten. The exception: a bad
    app that also ignores the ROM-boot key needs the Transporter.
- **Not redistributable:** stock V16, Baud Girl and Groove OS. These can only arrive from the Mac, from a copy Chad
  downloaded himself.

### Getting onto Wi-Fi

- **Setup:** from the phone, not the 1.69" screen. The Waveshare opens a setup hotspot with a small captive page,
  where Chad picks the network and enters the password.
- **Storage:** credentials go in NVS.
- **Use:** Wi-Fi is on only while checking or downloading. Routing never depends on it.

### Without Wi-Fi (v1)

- **Building the library:** `make library` builds the library partition on the Mac from a list of catalog IDs. It uses
  the same download and verify steps, run on the Mac.
- **Flashing it:** in download mode, the library partition is flashed alone, so backups and saves are untouched.
- **Own copies:** packages Chad downloaded himself go in the same way.

### The Waveshare's own updates

The Waveshare's firmware can also update itself over Wi-Fi from this repo's GitHub releases. That needs two app
slots (OTA) in the partition table. Changing the partition table later wipes the library and the backups, so it's
decided up front (decision 5 below).

## Storage, measured (2026-10-26)

All 103 catalog packages (60 MB) were downloaded to the Mac, and each matched its catalog SHA-256. The app areas
were then compared in 4 KiB sectors using `tools/catalog_dedupe.py`:
- **Before sharing:** each app area is 143 sectors (0.56 MB). All 103 together are 57.3 MB of non-blank sectors.
- **Shared code:** about 67 sectors are shared widely, the most common by 93 packages. Stored once, that brings the
  total to **40.7 MB** (1.41×).
- **Per package:** the median package needs 472 KB of its own.
- **Compression:** packages are encrypted, so zlib only shrinks them to 0.96×.

Packages that fit, picking firmwares at random (300 trials, median with 10th–90th percentile):

| Library budget | Stored whole | With shared sectors |
|---|---|---|
| 6.4 MB | 11 | 15 (13–17) |
| 8.4 MB | 15 | 20 (18–23) |
| 10 MB | 17 | 24 (22–27) |

**Design consequence: one content-addressed sector store** for packages, backups and saves. Each 4 KiB sector is
stored once, under its hash, and each package, backup or save is a list of sector hashes.
- **Backups get cheap:** a unit's app area is normally identical to a library package, so it costs nothing extra.
  The factory voices may well be identical across units (to be checked on the two FM-1s). What's left per unit is
  its settings, data and Bluetooth sectors.
- **Self-update needs no second app slot:**
  - A ~0.75 MB recovery app (Wi-Fi plus a flash copier, no screen library or USB host) stays fixed.
  - The new main app downloads into the store as an ordinary object.
  - On reboot, the recovery app copies it over the main app. If the copy is interrupted, the recovery app still
    boots and retries.
  - While downloading, it temporarily evicts a few unpinned, re-downloadable packages to make about 3 MB of room.
- **Recovery app's own updates:** they need the Mac (rare, by design).

## Laptop backups through the web page (added 2026-10-27)

Chad's request is that work on a firmware should never be lost to a swap, and that device space shouldn't limit
history. The browser the web UI already uses is the archive.

**Export: "Back up this FM-1"**
- **Starting it:** the web page asks. The device shows a confirm, because the FM-1 restarts into ROM boot.
- **What happens:** the device reads the full 1 MiB and streams it as a browser download, one file:
  `FM1-<unit name>_<firmware>-<version>_<date>.fm1bak`.
- **File format:** a manifest (unit ID, firmware identity, app hash, per-range SHA-256s, tool version) plus the
  sectors.
- **Save states:** they export the same way as small `.fm1save` files.

**Import and restore**
- **Starting it:** drop a `.fm1bak` or `.fm1save` onto the page.
- **Checks:** the device checks the unit ID, the hashes and the address guard before anything is written.
- **Writing:** it still needs the on-device confirm-and-hold. The web UI may request a write but never authorize one.

**What stays on the device**
- **Always:** the latest verified backup of each unit, taken before every write, as the plan already requires. An
  install must be able to finish or roll back with the laptop gone.
- **Freed up:** older history, archives and rarely used packages live on the laptop. The on-device library becomes
  "favorites, ready to go".

**Firmware sources**
- **Catalog firmwares:** the device downloads them itself over Wi-Fi. Browsers block the page from fetching
  thefirmwarehub.com across origins.
- **Chad's own downloads:** non-redistributable ones like V16 and Baud Girl get uploaded through the page.
- **Never:** `wl82loader.bin` is never offered for download.

## Design

| Part | Where | What |
|---|---|---|
| `fm1_loader` core | `lib/fm1_loader/` (plain C++, Mac-tested) | `.fwsc` and UFW parsing, flash layout constants, address guard, sector-diff planner, and the install state machine. The state machine runs against an abstract "ROM device" interface. |
| ROM-boot transport | firmware | The jl-uboot-tool command set (MIT, `jltech/uboot.py`) sent as raw SCSI commands through EspUsbHost's `mscCommand()`. That function is private today; the vendored copy gets a second one-line patch to make it public. |
| Loader blob | device storage, **not in git** | `wl82loader.bin` is JieLi's, and its redistribution status is unclear. `make setup-loader` fetches it from jl-uboot-tool at the pinned commit, checks the SHA-256, and puts it in the storage image. |
| Units and saves | backups partition | Per unit: name, baseline full backup (first sight, kept forever), latest full backup, and save states (non-blank sectors plus a manifest of unit ID, firmware identity, app hash, time and label). |
| Storage (superseded by "Storage, measured") | new partition table on the 16 MB flash | Three areas. A **library** partition holds packages and the loader blob, and is rebuilt from the Mac. A separate **backups** partition is never overwritten by library updates. `make pull-backups` reads it back to the Mac with `esptool read_flash` in download mode, so every backup also lives off-device. The partition change resets the NVS settings once. Sizes: about a 3 MB app slot, about 12 MB library, That holds the loader blob, about 15–18 packages (609–718 KB each, median 610 KB), and 1 MiB per backed-up unit. All 102 catalog packages would need about 62 MB, so v2 caches and fetches on demand. |
| UI | new "Firmware" page, reached from Settings | The FM-1's current identity and version, the stored packages, and the backups. Then a confirm screen, staged progress (Enter boot, Identify, Backup, Write N of M, Verify, Restart), and a result. |

**Install state machine:**
1. **Confirm**
2. **Send key**
3. **Wait for `4C4A:8057`**, with a 10 s timeout
4. **Identify**
5. **Upload loader**
6. **Read IDs**
7. **Backup**, or reuse a verified one
8. **Plan diff**
9. **Write and verify**
10. **Reset** (`0xFC0C`, argument 1: jl-uboot-tool's `reset`)
11. **Wait for the FM-1 to come back as MIDI**
12. **Done**

Every step has a failure exit.

**Resume entry.** If the Waveshare starts up, or its USB stack restarts, and finds `4C4A:8057` already attached, it
skips steps 1–3 and goes straight to Identify:
- **Why:** this is the case after a host crash mid-write. The FM-1 is on its battery, still in ROM boot, with a
  half-written app, and the SysEx key can no longer work.
- **Backup:** resuming reuses the stored, verified backup for that unit and refuses if there isn't one.
- **Durability:** the backup is fully written and fsync'd before the first erase.

That is what makes "run it again" true.

**Byte-exact port.** The ROM-boot sequence is ported from the proven Python and checked against it, not
re-derived:
- **Source:** the Unbricker's `upload_loader()` and `write_and_verify()`, and jl-uboot-tool's `jltech/uboot.py`.
- **What must match:**
  - The loader goes up in 512-byte blocks to `0x1C02000`.
  - The MengLi cipher is applied under the same condition.
  - The jump uses argument 1, then flash-ID and chip-key reads.
  - Erase, write and read use the same command layouts.
- **How it's checked:** golden vectors, the exact command bytes and payloads, are generated from the Python running
  against its own mock device (`mock_device`, `synthetic_flash`). The C++ state machine must emit the identical
  sequence against the same mock.

**Licenses (from `catalog.json`, 2026-10-26):**
- **Fine to cache:** 93 packages are GPL-3.0, 8 are MPL-2.0, and stock V15 is Apache-2.0 (mirrored from
  m-vave/fm1_open).
- **Not to be redistributed:** stock V16, Baud Girl and Groove OS. The Waveshare can still install a copy Chad
  downloads himself, but the repo and any bundled library never carry them.

**How packages get onto the Waveshare in v1:** a folder on the Mac is built into a LittleFS image, which I flash into the library partition only, so the backups partition is untouched. In
v2, the Waveshare fetches from `thefirmwarehub.com/catalog.json` over Wi-Fi, checks each SHA-256, and caches the
package; Wi-Fi is set up from a phone.

## Testing (test-first)

**On the Mac, before any hardware:**
- **Address guard:** every forbidden range is refused, including requests that only straddle the edges.
- **Parser:**
  - Real packages: stock V15 and SLOOP 2.8.1, hash-verified.
  - Damaged copies, with a CRC flipped or the file truncated.
- **Diff planner:** identical image gives 0 sectors. A one-byte change gives 1 sector. Nothing is planned outside
  the app area.
- **State machine against a mock ROM device with NOR semantics:**
  - **Golden-vector match:** the exact command sequence matches the Python reference.
  - **Save states:**
    - A save/restore round trip is byte-exact.
    - The update staging range is never saved or restored.
    - Restore is refused for a different unit or a different identity.
    - A version mismatch defaults to a fresh start.
    - Two units are keyed separately.
  - **Resume:** the host restarts mid-write, then resumes from "`4C4A:8057` already attached" using the stored
    backup. With no stored backup it refuses.
  - **Normal runs:** a clean install, and a no-op when the image is already installed.
  - **Identity failures:** wrong chip key, wrong flash ID.
  - **Write failures:** a read-back mismatch that clears on retry, and one that persists.
  - **Mid-write events:** the device disconnecting mid-write, and backup reads that disagree.

**On hardware, one checkpoint at a time, each with Chad present:**
1. **Read-only.**
   - Steps: enter ROM boot, identify, upload the loader, read the IDs, take a full backup, then reset the FM-1
     without writing anything.
   - **Pass:** compared with the recovery project's 2026-10-25 dump, the boot head (`0x0–0x3FFF`), Bluetooth data and
     factory voices (`0xE9000–0xFBFFF`) are byte-identical. The app and settings areas are expected to differ,
     because the unit now runs Skiff, not the V15 restored that day.
   - **Also answers two questions:**
     - Does Skiff accept the ROM-boot key `…7D F7`? That's reported for Felucca-family firmware but unverified for
       Skiff. The fallback is the OCT− + OCT+ 5 s hold, which is also only reported.
     - Does EspUsbHost's automatic mass-storage probing (inquiry, capacity) leave the ROM device usable? The Linux and
       Windows tools coexist with OS probing, which suggests yes. Before this checkpoint, read what the library sends
       on attach.
     - What does inquiry report once the loader runs? If it still says `UBOOT1.00`, a resume after a Waveshare
       crash would upload the loader again into a running loader (the mock then refuses, and the loader stops,
       leaving the FM-1 in ROM boot). The Unbricker's own branch suggests the name changes.
     - What does the loader report as its USB buffer size (`0xFC14`)? The mock says 512.
     - Does `0xFC0C` with argument 1 restart the FM-1 into its firmware?
2. **No-op install.** Install the firmware that's already on the unit. The plan must show 0 sectors to write.
3. **Real install.**
   - Steps: switch Skiff to SLOOP (same family), then back.
   - Precaution: the Transporter is wired and ready beforehand.
   - Honesty note: writing a third-party package through ROM boot has only been mock-tested so far (SLOOP 2.8.1 with
     the Unbricker). This is the first real run.
4. **Family change.** Stock V15 ↔ Skiff, including the settings warning.
5. **Save states.**
   - Make a Skiff project.
   - Switch to SLOOP, which starts fresh.
   - Switch back to Skiff, which restores the save.
   - Check the project is there.
6. **Second unit.** Plug in the other FM-1. It gets its own ID and name, and its own baseline backup and saves.

**Review:** `code-review` at high effort before merge, one round.

## Decisions for Chad

0. **Save-state behavior** (defaults; the alternative is to ask every time):
   - **Switching back to a firmware you've saved:** restore the save automatically.
   - **Updating a firmware:** keep the data in place.
   - **Switching families:** start fresh, keeping the saves.

1. **Go-ahead** for this plan under the guarded rules.
2. **v1 package source:** packages flashed from the Mac (recommended for v1), or straight to Wi-Fi.
3. **First firmwares to stock:** for example stock V15, Skiff, SLOOP.
5. **Partition layout, decided once.**
   - **Recommended:** map all 16 MB (today only 4 MB is mapped), as:
     - a ~0.75 MB recovery app;
     - one ~3 MB main app;
     - one ~12 MB shared sector store holding the library, backups and saves.
   - **Room:** about 24 packages, plus backups and saves for two FM-1s, plus self-updates. That replaces the earlier
     (a) "about 10" and (b) "about 13". See "Storage, measured" below.
4. **Loader blob:** OK to keep `wl82loader.bin` on the Waveshare's storage, fetched by `make setup-loader` and never
   committed. It sits in jl-uboot-tool, whose repo is MIT-licensed, but the blob is probably JieLi's own code, so
   treat its license as unclear.

## Rough size

Save states and units add about one session.

- **Core and Mac tests:** about one session.
- **Transport and storage:** one session.
- **UI and hardware checkpoints 1–2:** one session.
- **Checkpoints 3–4 and review:** one session.
