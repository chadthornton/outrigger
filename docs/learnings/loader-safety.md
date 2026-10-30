# The loader's safety design

[Index](index.md) · previous: [Porting to other ESP32 boards](porting.md) · next: [How robustness was earned](robustness.md)

The loader writes another device's flash, from a board with no keyboard, through a USB host that crashes now and
then. The design assumes something will go wrong mid-job and asks one question of every step: if the power or the
host dies right here, can the next attempt finish the job?

The rules are in `docs/firmware-loader-plan.md`, "Safety rules". This page says how each one is enforced and what
changed after the board taught the project something.

## The eight rules

| # | Rule | Enforced in | Tested by |
|---|---|---|---|
| 1 | **Address guard.** Erase and write only inside the app area; never the boot head or the Bluetooth and factory-voice area; settings only through an explicit erase. | `guard()`, `layout.cpp` | `test_fm1_package.cpp` (every edge) |
| 2 | **Identity before anything else.** Inquiry reads `WL82` / `UBOOT1.00`, the loader blob matches its pinned SHA-256, the flash ID is `0x856014`, and the chip key matches. Otherwise abort and reset the FM-1 untouched. | `installer.cpp` | golden `wrong_key`, `wrong_flash_id` |
| 3 | **Backup first, verified.** The full 1 MiB read twice and compared, stored under the unit's ID. No write without a verified backup of *this* unit. | `Installer::start_backup`, `library.cpp` | golden `backup`, `backup_mismatch` |
| 4 | **Package checks.** Parse, CRC-16s, `flash.bin` reaches `0x93000`, known identity, SHA-256 as recorded. | `package.cpp` | `test_fm1_package.cpp`, real and damaged packages |
| 5 | **Write only what differs, then read it back.** Per 4 KiB sector: erase, write, read back, compare, retry once. A second failure stops with the FM-1 kept in ROM boot. | `installer.cpp` | golden `retry_once`, `retry_fails` |
| 6 | **Data-layout warning** on a family change, with the settings erase offered. | `library.cpp` (`decide_data`) | `test_fm1_library.cpp` |
| 7 | **Loading is separate from routing.** Routing stops; the Router still drops every SysEx; only the Loader sends the key; a 2-second hold, never a tap. | broker (Load mode), `midi_core`, `host_ui` | `test_broker.cpp`, `test_midi_core.cpp`, `make ui-shots` (hold vs. tap) |
| 8 | **The remaining risk, said on screen.** If the FM-1 powers off mid-write, its app is incomplete and the key no longer works; that unit needs the Transporter plus this loader's backup. | Firmware page | |

The FM-1 runs on its battery, so a hub power blip alone doesn't reset it mid-write.

## Writes are locked until proven

`RomClient` refuses erase and write unless the build defines `FM1_LOADER_WRITES=1` (`make build WRITES=1`).
- **Mac tests:** on, against the mock ROM.
- **Firmware builds:** off by default. Only the checkpoint 2 and 3 builds turned it on.
- **Proof:** `test_fm1_locked` runs a whole install against the mock in a locked build and checks that no erase or
  write command (`FB01`, `FB04`) is ever sent.

## Plan before the key

The first install design took the backup inside ROM boot: key, loader, two full reads, plan, then write. At
checkpoint 3 that ran into the FM-1's [~8.5 s self-restart](fm1-usb-and-rom-boot.md#the-85-s-self-restart) twice.
Since `d860144`, an install does everything it can before the key:

1. **Plan from the stored backup.** The plan is computed from the unit's latest verified backup, the same one the
   preview showed. The user holds for exactly that plan: "Writes 143 of 143 app sectors / Restores its saves (6
   sectors)".
2. **Autosave from that backup,** fsync'd.
3. **Record the pending install** in the store.
4. **Then the key.** With nothing to write there's no ROM boot at all (golden scenario `noop`).
5. **In ROM boot, read once** and compare with that backup sector by sector.
   - **`BackupStale`:** the FM-1 changed since its backup, so stop before any erase.
   - **`LoaderTooSlow`:** the first erase would come more than 6 s after the loader started, so stop.
6. **Write and verify** the planned sectors.
7. **Record the result.** A verified install records the written flash as the unit's latest backup (`c22ba65`).

**A stop before any erase leaves nothing behind.** `ScsiTransport::reached_device()` says whether any command reached
the FM-1. If no erase did, the run clears its pending record and its danger flag.

**The stale-backup check proved itself.** Before `c22ba65`, a finished install didn't update the unit's latest
backup. After Skiff → Felucca, the page still marked Skiff running, and a second Felucca hold planned from the
pre-install backup. The check read refused it ("the FM-1 changed since its backup") before any erase.

## Pending records and resume

The danger case: the board crashes or loses power mid-write. The FM-1 is on its battery, still in ROM boot, with a
half-written app, and the key can no longer reach it.

- **The pending record** is written to the store before the key, so it survives any reset. Since `4985af4` the retry
  comes from the store, not RAM, so a crash, brownout or RST mid-install still offers it.
- **The resume entry:** a board that boots and finds `4C4A:8057` already attached skips the key, reads the unit's
  sector, requires its stored verified backup and the pending record, reads the flash once, and finishes that
  install. It refuses without a stored backup. It never resets the FM-1 on a failure.
- **A running loader:** after a board crash the FM-1's loader may still be running. The resume asks it
  (`online_device`) before deciding whether to upload again (`75fa17d`).
- **The retry is offered only while the FM-1 isn't back as MIDI** (`904f39e`). A pending record left by an install
  that stopped before writing had made the top row a retry while the FM-1 ran Skiff, and backups were unreachable.

**Storage durability.** `PosixStorage` writes each key to a temp file, fsyncs it, then renames it over the old one,
so a torn write is never visible. The `store` partition is mounted with format-on-fail off: an unreadable store says
so on screen and is never wiped.

## One boot per job

Since `74033a9`, every loader job (backup, install, resume) runs in a boot of its own:
1. The hold writes the job to RTC memory (`JobBoot`) and calls `esp_restart()`.
2. The next boot starts the USB host straight in Load mode, with hub tracking off, and clears the job record
   **before** running it. A crash during the job ends in safe mode, not in a second run.
3. The job runs, the result goes to RTC memory, and the board restarts into Play, where the Firmware page shows it.
4. **No ROM device 6 s after the key:** the board restarts once and waits again. If the FM-1 came back in its own
   firmware instead, the key is sent again, up to 3 boots (`fc72748`).

Why: in checkpoint 3, every USB failure came after the host restarted in place, and the first job after a fresh boot
always worked ([failures, section 4](usb-host-failures.md#4-in-place-host-restarts-checkpoint-3-2026-10-27-to-10-29)).

## Holds and what the screen says

- **Hold, never tap.** Anything that sends the key needs a 2-second hold. `make ui-shots` checks that a quick tap
  doesn't trigger it, with finger drift of 0, 8 and 16 px.
- **The row says what the hold does.** In checkpoint 3 the top row's hold had become "retry the install" while the
  row still read "Back up this FM-1", and Chad held it as a backup. It ran an install resume (`b4a4d71` fixed it; see
  [mistakes](mistakes.md#7-a-hold-labelled-back-up-ran-an-install)). The row now names the retry, and the hold runs
  only the package and unit the preview was computed for.
- **The agent reads `/status` before every hold** and quotes the top row and the preview to Chad.
- **Failures name their place.** "The FM-1 stopped answering" became, for example, "write 0x06C00 try 1, sector 3 of
  143", plus what EspUsbHost reported for the failed command (`605c522`).

## What the web may and may not do

The diagnostics pages are read-only (GET only; the one write is the setup hotspot saving a network, answered only on
the hotspot's own interface). In the planned Web UI, the page may *request* an install, but only the on-device hold
can authorize one, and a request expires after 60 s. There's no path from the web to `midiSend()`.
