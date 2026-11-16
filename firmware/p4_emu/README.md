# fm1-p4-emu: fm1-emu's core on the ESP32-P4 (spike, night 0, milestone C1-6)

Runs one FM-1 firmware package headless on Big-P4 and prints its emulated speed every 5 s.
The emulator is [fm1-emu](https://github.com/joakimeriksson/fm1-emu) by Joakim Eriksson, MIT
(credited in the repo's NOTICE), rev `60645a58`. MIT code may be used in this GPL-3.0 project as
long as its copyright and permission notice stay with it (`third_party/fm1-core/LICENSE`).

| Part | What |
|---|---|
| `core/` | `fm1-bench`: boots a package the way `fm1-live --bench` does (same machine set-up, keys 21/25/28 held from 0.8–1.0 s). `fm1-bench-host PKG [EMU_MS] [--jit]` is the Mac runner. |
| `third_party/fm1-core/` | fm1-emu's CPU core, vendored with one patch: on riscv32 (or `--features small_icache` on a Mac) a smaller instruction cache (40,960 lines, not 327,680) and a pre-sized block store capped at 2^17 ops, plus (track A2) a RAM index for the ISA class scan (`isa_index`: the table is ~300 KB of rodata in SPI flash on the P4, ~13 ms per uncached decode without it) and decode/block counters, because the full ones take ~56 MB per emulated core and Big-P4 has 32 MB of PSRAM. On a Mac this costs under 3% of speed, and the instruction counts are identical. fm1-isa and fm1-soc come from git at the same rev. |
| `esp/` | `fm1-p4-emu`, the P4 app: esp-idf-sys 0.38.1, ESP-IDF v5.5.5, `riscv32imafc-esp-espidf`, rev ≥ 3.0, 400 MHz, PSRAM on, console on UART0 (Big-P4's UART-C port). One emulator thread on core 1 runs both emulated FM-1 cores. No JIT: fm1-core JITs only to AArch64, x86-64 and wasm, so the P4 runs the block interpreter (the Mac's `FM1_NO_JIT=1` path). |
| `tools/mkpkg.py` | Turns a `.fwsc` into a `fwpkg` partition image (64-byte header + the package). |
| `tools/read_console.py` | Reads the console (optionally resetting the board first by RTS) and summarises a window of reports past the boot. |

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

Big-P4's Mac cable is on its **UART-C** port (CH343P, `/dev/cu.usbmodem5CF70432541`), whose
DTR/RTS drive the reset and download pins, so the console is on UART0. The USB-C port is the
P4's USB OTG port, for the hub. (B's first runbook used USB-C at `/dev/cu.usbmodem101`; that was
wrong for this bench.)

```sh
cd firmware/p4_emu
PY=~/.espressif/python_env/idf5.5_py3.13_env/bin/python
P=/dev/cu.usbmodem5CF70432541
# 1. identify the board before every flash: must print MAC e8:f6:0a:e8:a5:55
$PY -m esptool --chip esp32p4 --port $P --after no_reset read_mac
# 2. flash everything once (package: any of ~/Projects/spike-ext/pkgimg/*.pkg.bin)
$PY -m esptool --chip esp32p4 --port $P -b 460800 --after no_reset write_flash \
    --flash_mode dio --flash_size 8MB --flash_freq 80m \
    0x2000 out/bootloader.bin 0x8000 out/partition-table.bin \
    0x10000 out/fm1-p4-emu.bin 0x410000 ~/Projects/spike-ext/pkgimg/felucca-1.8.2.pkg.bin
# 3. reset and read for 10 minutes; the summary's window starts past 1 emulated second
$PY -I tools/read_console.py --from-emu-ms 1000 --reset $P 600 /tmp/p4emu-felucca.log
# 4. the next package: only the fwpkg partition changes, then read again
$PY -m esptool --chip esp32p4 --port $P -b 460800 --after no_reset write_flash \
    0x410000 ~/Projects/spike-ext/pkgimg/FM-1_015.pkg.bin
```

Opening the UART-C port resets Big-P4 on macOS even with DTR/RTS asked low (measured night 0),
so each read starts at a boot. If esptool can't connect: hold BOOT, tap RST, release BOOT (a person). `--after no_reset` leaves
the reset to the reader's `--reset` (an RTS pulse), so the log starts at the ROM banner.

Every 5 s the app prints `FM1EMU fw= emu_ms= wall_s= ratio= ... free_psram= decodes= built= resets=
min_free_psram=`. `ratio` is emulated time over wall time for the last report (1.0 = a real FM-1).
The SUMMARY's `window_ratio` is (last emu_ms - first emu_ms) / (last wall_s - first wall_s) over
the reports past `--from-emu-ms`; use it, not the per-report mean. A second `ESP-ROM` banner
(`boots=` > 0), `FM1EMU error` or `FM1EMU halted` is a failure.

Results from night 0 (track A2) are in `night0/a2.md` in the spike notes folder.
