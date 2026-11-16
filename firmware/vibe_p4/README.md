# vibe_p4: Vibes on the ESP32-P4 (P4 platform spike, night 1, track S1)

Can a Vibe (a firmware built with the VIBE FM SDK, github.com/Vibe-FM-SDK/vibe-fm-sdk, Apache-2.0) run on the
ESP32-P4? This folder has a thin host for the SDK's `.wasm` on WAMR, a Mac runner that checks it against the SDK's
own Node host, one Faust patch wrapped as a Vibe, and the Big-P4 bench images that HW3 times.

Built with the VIBE FM SDK. (The SDK's NOTICE allows that wording; "VIBE FM compatible" needs permission.)

## What's here

| Path | What |
|---|---|
| `host/vh.h`, `vh_wamr.c` | The shim: the three `env` imports (`vibe_host_log`, `vibe_host_request`, `vibe_host_store`, the last always -1) and the per-block calls (`vibe_wasm_init`, `_time`, `_event`, `_io`, `_audio`). Loads a `.wasm` or an `.aot`. Same file on the Mac and the P4. |
| `host/vh_native.c`, `vh_native_host.c` | The same Vibe compiled natively, driven through the same calls. |
| `host/vh_scenario.c` | The fixed scenario and `wasmtest.mjs`'s `out_hash` (FNV-1a over L and R). |
| `mac/` | `vibe_run`: renders the scenario with one backend and prints a JSON line. |
| `faust-synth/` | `voice.dsp` (saw, `fi.resonlp`, `en.adsr`), its `faust -lang c` output, and an 8-voice Vibe around it. |
| `p4/` | The ESP-IDF v5.5.5 project for Big-P4, and `wamr-2.4.4-espidf-icache.patch`. |
| `gen/` | Symbol-prefix headers so two natively compiled Vibes, each with its own SDK copy, link into one image (`tools/gen_prefix.sh`). |
| `scripts/notesN.json` | The scenario as a `wasmtest.mjs` script. |
| `tools/build_p4.sh` | Fetches the pinned sources, builds the Vibes, checks them on the Mac, and builds the three images. |
| `tools/flash_p4.sh`, `tools/vibe_console.py` | For HW3: flash with the MAC check, then drive the bench and summarise. |

**The scenario:** 1, 4 or 8 notes (48 52 55 59 62 65 69 72, velocity 100) as MIDI note on at 0 s, note off at 1.5 s,
2.5 s in all, at 48 kHz in 128-frame blocks. An event goes in before the first block whose start is at or after its time,
as in `wasmtest.mjs`.

## Pinned sources (outside the repo, in `~/Projects/spike-ext/s1`)

- VIBE FM SDK `2ac2340` (SDK 27). Built with its own `tools/build.sh` and zig **0.15.2**. The script asks for 0.13.0.
  - zig 0.15 (LLVM 19 and later) emits the reference-types encoding, so WAMR needs `REF_TYPES` on.
- WAMR tag `WAMR-2.4.4` (Apache-2.0 WITH LLVM-exception). Its ESP-IDF component supports the P4
  (`CONFIG_IDF_TARGET_ESP32P4` builds as `RISCV32_ILP32F`), and it has a RISC-V AOT relocator.
- One patch to WAMR: `os_icache_flush` is empty upstream on ESP-IDF. On the P4 it now writes back the D-cache, invalidates
  the I-cache over the AOT code, and runs `fence.i`. **Untested on hardware.**
- wamrc 2.4.4 on LLVM 18 (WAMR's `build_llvm.py`, AArch64 + RISCV). P4 target:
  `--target=riscv32 --target-abi=ilp32f --cpu=generic-rv32 --cpu-features=+m,+a,+c,+f --bounds-checks=1`.
- Faust 2.88.0 (Homebrew). It's needed only to regenerate `faust_voice.h`.

## What the `.wasm` needs from WAMR

- Shared memory and atomics: the SDK links with `--shared-memory --max-memory=64MB`.
  - The module has no `memory.grow`, so WAMR's loader folds the memory to one 832 KB page. The 64 MB maximum is never
    reserved; the Mac shows "1 pages now, 1 at most".
- Bulk memory and reference types.
- Nothing else: no libc-builtin, no WASI, no pthread, no app heap.

## Mac results (M4 Pro, measured 2026-11-16)

**Correctness.** WAMR's classic interpreter, fast interpreter and AOT (software bounds checks) reproduce `wasmtest.mjs`'s
`out_hash` bit for bit, for both Vibes at 1, 4 and 8 notes.

| Vibe | 1 note | 4 notes | 8 notes |
|---|---|---|---|
| example-synth | 4d38dc17 | 3899413d | 7dda4467 |
| faust-synth | abb1e179 | 157a2b57 | ce13b7b5 |

**Native vs wasm.** The native build uses the same C with `-O2 -ffast-math`. It is not bit-exact, as expected. Difference
below the signal:
- example-synth: -98.2 / -93.4 / -75.1 dB (max abs 5.8e-6 / 1.9e-5 / 2.3e-4).
- faust-synth: -83.0 / -88.7 / -80.0 dB.

**Speed.** us per 128-frame block, best mean of 5 runs. The budget is 2,667 us.

| Vibe | classic | fast | AOT | native |
|---|---|---|---|---|
| example-synth 1 / 4 / 8 notes | 36.7 / 82.6 / 132.6 | 16.0 / 37.8 / 62.6 | 0.97 / 2.18 / 3.67 | 0.70 / 1.54 / 2.58 |
| faust-synth 1 / 4 / 8 notes | 17.5 / 49.0 / 93.5 | 7.8 / 22.5 / 40.6 | 0.78 / 2.67 / 5.16 | 0.52 / 1.86 / 3.83 |

So on the Mac, against native:
- classic: 24 to 52x;
- fast: 11 to 24x;
- AOT: 1.35 to 1.50x, with software bounds checks.

VIBE's own figure for WAMR AOT on its A53 is 1.1 to 1.2x native.

**Mac-only AOT failure.** With WAMR's hardware bound check (the default on 64-bit) and wamrc's defaults, example-synth's
AOT segfaults in 4 of 20 runs ("unhandled SIGSEGV, si_addr: 0x0"). faust-synth's doesn't. With software bounds checks,
which is the P4's mode, it passed 0 of 20 and 0 of 30. Root cause not found.

## The Big-P4 images (for HW3)

`tools/build_p4.sh` builds three images in `~/Projects/spike-ext/s1/p4img/`. Each is one merged file written at 0x0.

| Image | WAMR | Native Vibes' state |
|---|---|---|
| `classic/vibe_bench_p4.merged.bin` | classic interpreter + AOT | PSRAM |
| `fast/vibe_bench_p4.merged.bin` | fast interpreter + AOT | PSRAM |
| `fast-int/vibe_bench_p4.merged.bin` | fast interpreter + AOT | internal SRAM |

Each image holds both Vibes as `.wasm` and `.aot`, plus both compiled natively.
- The `.wasm`'s 832 KB memory has to live in PSRAM. So by default the native Vibes' `.bss` goes there too (a linker
  fragment). `fast-int` shows what that costs.
- AOT code goes to internal RAM (`MALLOC_CAP_EXEC`), which needs `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n`.
- Base config is A2's: rev 3.x, 400 MHz, PSRAM 200 MHz, console on UART0 at 115200, `-O2`, no task watchdog.
- The bench task runs on core 1 at priority 10. Timing uses the CPU cycle counter.

### Build (no hardware)

```sh
firmware/vibe_p4/tools/build_p4.sh
```

About 25 s when warm. The first run also builds LLVM, about 15 min.
- It stops if the WAMR shim's hashes differ from `wasmtest.mjs`.
- It prints the images' SHA-256.

### Flash (HW3)

```sh
firmware/vibe_p4/tools/flash_p4.sh ~/Projects/spike-ext/s1/p4img/fast/vibe_bench_p4.merged.bin
```

The script reads the MAC over UART-C (`/dev/cu.usbmodem5CF70432541`) with `--after hard_reset` and refuses anything but
`e8:f6:0a:e8:a5:55`. Then it runs `write_flash 0x0` at 460,800 baud.

### Run (HW3)

```sh
~/.espressif/python_env/idf5.5_py3.13_env/bin/python -I firmware/vibe_p4/tools/vibe_console.py \
    /dev/cu.usbmodem5CF70432541 iB 900 ~/Projects/spike-ext/s1/hw3-fast.log
```

Opening UART-C resets the board, so the tool waits for `BENCH ready` before it sends anything.
- Commands are single letters, no Enter: `i` info, `B` sweep, `s` / `f` one Vibe, `b` toggles 128 / 64-frame blocks
  (64 has its own expected hashes), `m` memory.
- A healthy boot prints `BENCH P4 vibe bench, MAC e8:f6:0a:e8:a5:55, ... wamr fast+aot, native bss psram ...`, then
  `BENCH ready`, then `BENCH alive` every 10 s.
- `B` prints one `VIBE` line per run: 2 Vibes x interpreter / AOT / native x 1, 4, 8 notes, then `BENCH sweep done`.
  Each line has `hash`, `expect`, `match`, `us_mean`, `us_held` (the mean while the notes are held), `us_max`,
  `held_pct` (of the block budget), `load_us`, and memory used. AOT and native rows add `vs_interp_db`.

**What to check first.** Every interpreter row must say `match=yes`: the P4 then plays the same samples as the SDK's own
host. Do this before trusting any timing.
- AOT rows should also say `match=yes`, as they did on the Mac.
- `FAILED` or `match=NO` on an AOT row is the open risk: riscv32 AOT has never run on hardware, nor has the icache patch.
- Native rows are never bit-exact. Only the first native run of each Vibe after a boot starts from fresh state
  (`fresh=1`). The rest reuse the voices' leftover phase, so their `vs_interp_db` is not meaningful, but their timing is.
  For clean native numbers per note count, reset between runs.

Suggested order: `fast` (iB), `classic` (iB), `fast-int` (iB), then `fast` again with `b` then `B` for 64-frame blocks.

## Licences

- VIBE FM SDK: Apache-2.0. It is used from a clone and not vendored. Its NOTICE restricts the "VIBE FM" name.
- WAMR: Apache-2.0 WITH LLVM-exception. Used from a clone, with our patch file.
- Faust libraries in `voice.dsp`:
  - oscillators.lib and filters.lib: STK-4.3 (MIT-style).
  - envelopes.lib: LGPL-2.1-or-later with the Faust exception, which covers generated code.
  - Faust's LGPL `CInterface.h` is not copied: `faust_glue.h` is our own 20 lines.
- Both are Apache-2.0 and compatible with Outrigger's GPL-3.0.
