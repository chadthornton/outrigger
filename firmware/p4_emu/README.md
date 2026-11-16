# fm1-p4-emu: fm1-emu's core on the ESP32-P4 (spike, night 0, milestone C1-6)

Runs one FM-1 firmware package headless on Big-P4 and prints its emulated speed every 5 s.
The emulator is [fm1-emu](https://github.com/joakimeriksson/fm1-emu) by Joakim Eriksson, MIT
(credited in the repo's NOTICE), rev `60645a58`. MIT code may be used in this GPL-3.0 project as
long as its copyright and permission notice stay with it (`third_party/fm1-core/LICENSE`).

| Part | What |
|---|---|
| `core/` | `fm1-bench`: boots a package the way `fm1-live --bench` does (same machine set-up, keys 21/25/28 held from 0.8–1.0 s). `fm1-bench-host PKG [EMU_MS] [--jit]` is the Mac runner. |
| `third_party/fm1-core/` | fm1-emu's CPU core, vendored with one patch: on riscv32 (or `--features small_icache` on a Mac) a smaller instruction cache (40,960 lines, not 327,680) and a pre-sized block store capped at 2^17 ops, because the full ones take ~56 MB per emulated core and Big-P4 has 32 MB of PSRAM. On a Mac this costs under 3% of speed, and the instruction counts are identical. fm1-isa and fm1-soc come from git at the same rev. |
| `esp/` | `fm1-p4-emu`, the P4 app: esp-idf-sys 0.38.1, ESP-IDF v5.5.5, `riscv32imafc-esp-espidf`, rev ≥ 3.0, 400 MHz, PSRAM on, console on USB Serial/JTAG. One emulator thread on core 1 runs both emulated FM-1 cores. No JIT: fm1-core JITs only to AArch64, x86-64 and wasm, so the P4 runs the block interpreter (the Mac's `FM1_NO_JIT=1` path). |
| `tools/mkpkg.py` | Turns a `.fwsc` into a `fwpkg` partition image (64-byte header + the package). |
| `tools/read_console.py` | Reads the console without resetting the board and averages the reports. |

Package images are written outside the repo (`~/Projects/spike-ext/pkgimg/`); none is ever committed. SHRUB is closed: keep it at `~/Projects/shrub-test/SHRUB.fwsc`.

## Build (Mac, no hardware)

```sh
firmware/p4_emu/build.sh ~/Projects/spike-ext/packages/FM-1_015.fwsc \
    ~/Projects/spike-ext/packages/felucca-1.8.2.fwsc ~/Projects/shrub-test/SHRUB.fwsc
```

Needs rustup's toolchain (`~/.cargo/bin`; `esp/rust-toolchain.toml` pins nightly-2026-09-15,
because the 2026-11-13 nightly's std doesn't build for espidf against libc 0.2.191), `ldproxy`
(`cargo install ldproxy`), and pyenv's Python 3.13.3 for ESP-IDF's tools. The first build puts
ESP-IDF v5.5.5 and its tools in `~/.espressif`. Output in `out/`:

| File | Flash offset |
|---|---|
| `bootloader.bin` | 0x2000 |
| `partition-table.bin` (from `esp/partitions.csv`) | 0x8000 |
| `fm1-p4-emu.bin` (2.7 MB; factory slot is 4 MB) | 0x10000 |
| `~/Projects/spike-ext/pkgimg/<name>.pkg.bin`, one per package (outside the repo; `PKG_OUT` moves it) | 0x410000 (`fwpkg`, 2 MB) |

## Flash and read (track A2, Big-P4 only)

1. **Check the board first**, without opening any port: the USB device on the port must be
   `303a:0012` with serial number `e8:f6:0a:e8:a5:55`.
   ```sh
   ioreg -r -c IOUSBHostDevice -l | grep -E '"USB Serial Number" = "e8:f6:0a:e8:a5:55"'
   ls /dev/cu.usbmodem101
   ```
2. **Flash everything once**, with Felucca as the first package:
   ```sh
   cd ~/Projects/usb-midi-host-b/firmware/p4_emu
   PY=~/.espressif/python_env/idf5.5_py3.13_env/bin/python
   $PY -m esptool --chip esp32p4 --port /dev/cu.usbmodem101 -b 460800 \
       --before default_reset --after hard_reset write_flash \
       --flash_mode dio --flash_size 8MB --flash_freq 80m \
       0x2000 out/bootloader.bin 0x8000 out/partition-table.bin \
       0x10000 out/fm1-p4-emu.bin 0x410000 ~/Projects/spike-ext/pkgimg/felucca-1.8.2.pkg.bin
   ```
   If it can't connect: hold BOOT, tap RST, release BOOT (a person has to do this; leave a note).
3. **Read it**, for at least 60 s (each report covers 5 s of wall time):
   ```sh
   $PY -I tools/read_console.py /dev/cu.usbmodem101 90 /tmp/p4emu-felucca.log
   ```
   Lines to expect:
   - `FM1EMU start: ESP32-P4 at 400 MHz ...`
   - `FM1EMU package felucca-1.8.2: 610286 bytes`
   - `FM1EMU boot ... free internal N PSRAM N`
   - every 5 s: `FM1EMU fw=... emu_ms= wall_s= ratio= ratio_total= emu_mcyc_s= cpu0_minsn_s= cpu1_minsn_s= cpu1= audio_frames= skipped_pct= free_int= free_psram=`
   - in the end, `SUMMARY fw=... mean_ratio=... mean_emu_mcyc_s=...`

   `ratio` is emulated time over wall time (1.0 = a real FM-1's speed). `emu_mcyc_s` is emulated
   cycles a second, against a real FM-1's 240 M a core. `FM1EMU error`, `FM1EMU halted` or a
   panic or reboot loop is a failure: copy the log.
4. **The next package**: only the package partition changes.
   ```sh
   $PY -m esptool --chip esp32p4 --port /dev/cu.usbmodem101 --after hard_reset \
       write_flash 0x410000 ~/Projects/spike-ext/pkgimg/FM-1_015.pkg.bin   # then SHRUB.pkg.bin
   ```
   Then read again (step 3). Repeat for each package: stock V15 (`FM-1_015`), Felucca 1.8.2, SHRUB.
5. **End with a hard reset**, e.g. `$PY -m esptool --chip esp32p4 --port /dev/cu.usbmodem101 --after hard_reset chip_id`.

The Mac numbers to compare against (same input, M4 Pro, one thread) are in `night0/b.md` in the
spike notes folder. Mac interpreter: stock 1.38x, Felucca 0.99x, SHRUB 0.50x real time.
