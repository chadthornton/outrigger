# Plan: on-device FM-1 firmware loader (guarded)

Status: **draft for Chad's go-ahead**, 2026-10-26. Nothing below is built yet.

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

- **Space:** the library partition holds about 10 packages under layout (a), or about 13 under (b) (decision 5).
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

## Design

| Part | Where | What |
|---|---|---|
| `fm1_loader` core | `lib/fm1_loader/` (plain C++, Mac-tested) | `.fwsc` and UFW parsing, flash layout constants, address guard, sector-diff planner, and the install state machine. The state machine runs against an abstract "ROM device" interface. |
| ROM-boot transport | firmware | The jl-uboot-tool command set (MIT, `jltech/uboot.py`) sent as raw SCSI commands through EspUsbHost's `mscCommand()`. That function is private today; the vendored copy gets a second one-line patch to make it public. |
| Loader blob | device storage, **not in git** | `wl82loader.bin` is JieLi's, and its redistribution status is unclear. `make setup-loader` fetches it from jl-uboot-tool at the pinned commit, checks the SHA-256, and puts it in the storage image. |
| Units and saves | backups partition | Per unit: name, baseline full backup (first sight, kept forever), latest full backup, and save states (non-blank sectors plus a manifest of unit ID, firmware identity, app hash, time and label). |
| Storage | new partition table on the 16 MB flash | Three areas. A **library** partition holds packages and the loader blob, and is rebuilt from the Mac. A separate **backups** partition is never overwritten by library updates. `make pull-backups` reads it back to the Mac with `esptool read_flash` in download mode, so every backup also lives off-device. The partition change resets the NVS settings once. Sizes: about a 3 MB app slot, about 12 MB library, That holds the loader blob, about 15–18 packages (609–718 KB each, median 610 KB), and 1 MiB per backed-up unit. All 102 catalog packages would need about 62 MB, so v2 caches and fetches on demand. |
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
10. **Reset** (`0xFC02`)
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
5. **Partition layout, decided once.** Today only 4 MB of the 16 MB flash is mapped (the Arduino default): two
   1.25 MB app slots, holding the 0.96 MB firmware plus an empty spare, and an unused 1.4 MB file area. Backups
   and saves need about 4.5 MB for two units, each with a 1 MB first-sight backup, a 1 MB latest backup, and saves.
   - **(a) Recommended:** two 2.5 MB app slots for the Waveshare's own OTA, and a library of about 6.4 MB (about
     10 packages).
   - **(b)** One 3 MB app slot and a library of about 8.4 MB (about 13 packages). With (b), the Waveshare's own
     updates need the Mac.
   - **For scale:** the whole catalog is about 62 MB, so the library is a cache either way.
4. **Loader blob:** OK to keep `wl82loader.bin` on the Waveshare's storage, fetched by `make setup-loader` and never
   committed. It sits in jl-uboot-tool, whose repo is MIT-licensed, but the blob is probably JieLi's own code, so
   treat its license as unclear.

## Rough size

Save states and units add about one session.

- **Core and Mac tests:** about one session.
- **Transport and storage:** one session.
- **UI and hardware checkpoints 1–2:** one session.
- **Checkpoints 3–4 and review:** one session.
