# Updates over Wi-Fi

Outrigger updates its own firmware without USB. The flash layout doesn't change
(`firmware/waveshare_169/partitions.csv`, decided in `firmware-loader-plan.md`,
decision 5), so the store and every backup in it stay where they are.

| Partition | Role in an update |
|---|---|
| `app0` (3 MB, 0x10000) | Outrigger |
| `recovery` (768 KB, 0x310000) | the recovery app (`firmware/recovery`): copies an image over `app0`, checks it, rolls back |
| `store` (12 MB) | `u/next.bin` (the new image) and `u/prev.bin` (the running one) during an update |
| `nvs` | the update record (`ota`/`rec`) and the recovery app's log (`ota`/`log`) |

## How an update runs

The bootloader's own app rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, on in
Arduino's ESP32 core 3.3.12) does the watching. The steps:

1. Outrigger saves the new image as `u/next.bin` and its own running image as
   `u/prev.bin`, and checks both by SHA-256. It then saves a **Staged** record and
   points otadata at `recovery`.
2. The recovery app checks `u/next.bin` against the record before it erases
   anything. It copies the file over `app0`, checks the copy, and counts the boot in
   the record (**Trial**, attempt 1). Then it points otadata at `app0`, which
   leaves that image `NEW`.
3. The bootloader starts `app0` in `PENDING_VERIFY`. Outrigger confirms the image
   (`esp_ota_mark_app_valid_cancel_rollback`) 10 s into a healthy boot, meaning
   Play or Computer mode, not safe mode. The record then becomes **Idle**/updated
   and both files are deleted.
4. A boot that ends without that confirm makes the bootloader mark `app0`
   `ABORTED` and start `recovery` again. Causes: a crash, a watchdog reset, a power
   cut, or Outrigger's own restart at 120 s. The recovery app counts the attempt
   and tries again.
5. After 3 attempts it copies `u/prev.bin` back (**RolledBack**). Outrigger's next
   boot records the result and deletes the files.

The logic lives in `lib/ota` (`run_recovery`, `plan_boot`), plain C++.
`tests/test_ota.cpp` runs it against a model of the ESP-IDF 5.5.5 bootloader's
otadata code (`tests/support/ota_sim.h`). A power cut lands at every write and at
every pair of writes: flash writes and erases torn in half, otadata entries left
with a bad CRC. In every case the board ends up on a confirmed build, and the
record says which one.

## What the tests found

- **One window loses the update:** a power cut tearing one of the two otadata
  writes as the recovery app first starts sends the board back to the old build.
  The update is lost but never left half-done, and the record says `abandoned`.
  ESP-IDF's otadata handling can't close this window.
- **Writing only `app0` over USB:** the installer and `make flash` write
  `boot_app0` at otadata, which ends any update. A USB write of `app0` alone during
  an update leaves otadata pointing at `recovery`, which copies `u/next.bin` over
  it. The cure is to reflash with `make flash`.
- **Old builds:** a build from before this change doesn't read the record. After a
  rollback to one, the record stays `rolled-back` and the files stay until the
  next update. That's harmless, and they cost store space only.

## The recovery app

- **What it has:** LittleFS, NVS, mbedtls and the partition API, and nothing else
  (343 KB). It has no Wi-Fi, because Outrigger does every download.
- **It can't draw:** the record carries the board profile's backlight pin and its
  hold-low pins, such as a speaker amplifier's, so the recovery app keeps them
  where Outrigger had them.
- **Installing it:** `make flash-recovery` writes only that slot over USB.
  Outrigger can also write the slot itself, because it's the app slot not running.
- **Is it there:** `/status` → `update.recovery` gives the slot's image digest, or
  `none`. That's the SHA-256 ESP-IDF appends to every image (its last 32 bytes), 16 hex
  digits, not the file's own SHA-256.
- **What it did:** `/crash` → `recovery_log`.

## Bench push (BENCH=1 builds)

`POST /bench/update?sha256=<64 hex>&label=<commit>[&slot=recovery]`, the image as the body. It's answered only on
the boards in the device registry (the same gate as `/bench/act`), never on the setup hotspot. It's refused while
the Loader is busy, while the board's own image is unconfirmed, while another update is under way, and when the store
lacks room for both images.

- **App images** stream into `u/next.tmp` on the web task, 4 KB at a time, so MIDI keeps running. A wrong SHA-256
  discards the file. The loop then saves `u/prev.bin`, writes the record, points otadata at the recovery app, shows
  "Updating Outrigger. Don't unplug." and restarts.
- **`slot=recovery`** writes the recovery slot in place, then validates it as an app image. Outrigger never runs from
  that slot, so a failed write only means sending it again.
- **The client:** `tools/bench.py update` (`docs/test-bench.md`).

There's no signature on this path: it's bench-only, on the LAN, and driven from the Mac.

## Phases

1. The recovery app and the confirm.
2. Bench push: `POST /bench/update` and `tools/bench.py update`.
3. End-user updates from the site's signed manifest, behind Setup.
