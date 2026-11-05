# Outrigger Synth: first sound and Felucca's fit (Big-S3)

Session: "synth first sound" (branch `claude/synth-first-sound-50d8a7`, PR #54). Status, 2026-11-03: **done on
Big-S3. Chad has moved the synth track to the P4 only** ("keep strictly focused with the P4 on the synth stuff").

**The answer:** Felucca fits on one ESP32-S3 core with room to spare.
- **Heaviest case:** PHYS with 4–8 notes held, at 67% of the time budget (worst block 1,028 of 1,451 µs).
- **Most engines:** 14–50%.
- **Dropouts:** 0 underruns anywhere.

The P4 (faster cores, its USB DAC already proven) is now where the synth gets built and heard.

## What's built

- **Tone bench** (`make synth-bench`): a serial bench on Big-S3.
  - Plays a quiet 440 Hz tone out of the speaker, at about −37 dB in total; I can raise it from the Mac.
  - Logs every 10 s: dropouts ("underruns"), and how long each 64-sample block took against its budget.
- **Felucca bench** (same build):
  - Felucca 1.7.0's engine, compiled for the S3 from the fm1-cartridge port.
  - One command (`B`) steps through all 13 engines with 1, 4 and 8 notes held, with the speaker off.
  - Logs render time per block against the 1,451 µs budget.
- **Synth firmware** (`make build BOARD=waveshare_35 APPS=synth`):
  - Outrigger itself, with Felucca as a second destination: Launchkey notes go to the built-in synth and to an FM-1
    if one is connected.
  - Setup's network line shows the synth's render time and dropouts.
  - Development-only: CI builds it, the installer site never gets it.

## Results (Big-S3, 44:bd:8d:61:2b:28, 2026-11-03)

**The tone, slice 1:**
- **Board side passed:** 647 s at 48 kHz, 485,574 blocks, **0 underruns**. Rendering the sine took 64 µs per block
  on average (118 µs at worst), against 1,333.
- **Not heard:** Big-S3 has no speaker, only a "speaker connector" (Waveshare's spec). Chad heard nothing, and
  the ear test is dropped with the S3 track.

**Felucca's render time, slice 2:** every engine's first preset with 1, 4 and 8 notes held, 4 s each after a 1 s
settle, speaker amp off. Budget: 1,451 µs per 64-frame block at 44.1 kHz. **0 underruns and 0 blocks over budget
in every row.**

| Engine (preset) | 1 note avg / max µs | 4 notes | 8 notes | Worst share |
|---|---|---|---|---|
| ANALOG (SAW LEAD) | 334 / 373 | 344 / 374 | 344 / 376 | 26% |
| FM6 (TINE EP) | 441 / 492 | 573 / 654 | 668 / 762 | 53% |
| PHASE (BRASS) | 297 / 342 | 392 / 441 | 503 / 549 | 38% |
| LOFI (PULSE LD) | 282 / 316 | 282 / 315 | 282 / 310 | 22% |
| SAMPLE (PIANO) | 206 / 220 | 206 / 325 | 206 / 313 | 22% |
| VOICE (CHOIR AAH) | 437 / 485 | 693 / 745 | 700 / 743 | 51% |
| TRIO (FAT BASS) | 414 / 457 | 412 / 455 | 414 / 452 | 31% |
| WHEEL (FULL ORGAN) | 377 / 414 | 532 / 574 | 745 / 785 | 54% |
| GRAIN (CLOUD PAD) | 450 / 591 | 610 / 790 | 570 / 713 | 54% |
| PHYS (BELL TREE) | 618 / 656 | 983 / 1,023 | 983 / 1,028 | 71% |
| NOISE (WIND) | 335 / 385 | 484 / 523 | 687 / 724 | 50% |
| SLICE (CHOP) | 206 / 222 | 207 / 223 | 207 / 220 | 15% |
| DRUM (DRUM KIT) | 206 / 219 | 208 / 392 | 422 / 525 | 36% |

"Worst share" is the worst block against the budget.

Raw logs: `soak.log` and `sweep.log`, in the orchestrator's side-session notes folder (not in the repo).

**How to read it:**
- **One part of Felucca's four,** on one core, with the other core free for USB and the screen. That's the
  cartridge port's scope.
- **The cartridge measured worse on its own S3:** Felucca at 765–1,854 µs, with underruns. Its board had quad
  PSRAM at 40 MHz. Ours has octal PSRAM at 80 MHz and QIO flash, even with Arduino's smaller caches (16 KB
  instruction, 32 KB data).
- **Flat engines (LOFI, TRIO, SLICE):** their numbers barely change with note count. They're mono or
  paraphonic presets, or the cost is mostly effects.
- **What the sweep didn't cover:** effects or presets heavier than each engine's first; the sequencer running;
  more than one part; LVGL and USB host load beside it (the synth app set has that; not measured yet).
- **For the P4:** these numbers are an upper bound. Its cores are faster and the USB DAC path is proven there, so
  fit isn't the risk on the P4. Wiring Felucca into the DAC is.

**The S3 code that carries over to the P4:**
- `felucca_unit.c`: one translation unit, -O2 -fwrapv, PSRAM lines.
- `synth_engine` (the audio task); only its output step changes from I2S to a ring buffer the USB DAC pulls from
  (the P4 probe's advice).
- `Router::to_synth`.

**What doesn't carry over:**
- The ES8311 and I2S output (`audio_out`).
- The S3 bench.

## Felucca upstream's `platforms/` (the orchestrator's second question): which route measures render time?

**I'm using the fm1-cartridge port for the measurement, and upstream's host for the synth app later.**

**What upstream has.** `hugelton/Felucca` gained `platforms/` on 2026-10-30 (`1350697b5`), reworked on 2026-11-01
(`425f1a275`, "host platforms on one host layer"). Every frontend (web, SDL2, libretro) builds
`platforms/host/felucca_host.c`.
- That file is **the whole firmware against a simulated FM-1**: the UI drawing the 240×240 screen, all four parts,
  the sequencer, storage in a 1 MiB "NOR".
- Its API is `felucca_host.h`: `fh_boot`, `fh_render(n)` (float output), `fh_buttons`, `fh_keys`, `fh_enc`,
  `fh_midi`, `fh_screen`, the LEDs, and `fh_nor`.

**What upstream's own porting guide says** (`docs/PORTING.md`): there are two ways to port.
- A **host frontend** is for "anything with an OS". A **HAL port** is for "a microcontroller board with keys,
  encoders, a display and an audio codec". Big-S3 is the second kind.
- It asks for an issue or discussion before a large port, so the work can be shared. So an ESP32-S3 entry in
  `platforms/` is possible, but on the maintainer's terms.

**Why the cartridge port for the measurement:**
1. **It measures the engine alone.** The cartridge adapter is Felucca's sound path only: one part, no UI, int16
   output. That's the minimum any route pays. If it doesn't fit one S3 core, nothing built on the full firmware will.
2. **The host costs more on top.** `fh_render` also runs the UI's drawing, all four parts' sequencers and float
   conversion on the same thread, so its numbers would mix the engine with the shell.
3. **It builds without running anyone's tools on the Mac.** The cartridge vendors Felucca's generated tables.
   Upstream's need `./build.sh gen` (Python with Pillow and fontTools), and this session's safety rules block
   running downloaded code on the Mac. CI or a later session could generate them.
4. **Same DSP source.** The cartridge pins Felucca 1.7.0 (`d7b1b7c4`) and changes one upstream file
   (`fm1_xip.h`, the user sample slots). So its render time is Felucca's render time.

**Where upstream's host fits:** step 3, the synth app with Felucca's own screen and panel. Run `felucca_host` on the
S3:
- the screen goes to an LVGL canvas, the panel to the Launchkey plus on-screen buttons;
- the NOR goes to a flash partition, saved only while the sequencer is stopped.

That gives the real FM-1 experience, and it would be the natural thing to offer upstream (an issue first, as their
guide asks). Its extra cost over the engine (UI, four parts) is the number to measure then.

**Relation to fm1-base:** two different shells. fm1-base's `plat.h` runs fm1-base apps; Felucca's host runs
Felucca. Pick between them at step 3, on what Chad wants the synth to be: "an FM-1 without an FM-1" (Felucca's host)
or "a platform for FM-1 apps" (fm1-base).

**Context noted:** the browser-emulator approach started with Charles Vestal's X0X in WebAssembly (fm1-x0x,
2026-10-04), and JIANT's emulator follows it.

## fm1-base (the orchestrator's question): does it change slice 2?

**Short answer: no for this slice, and probably yes for the synth app after it.** fm1-base doesn't contain Felucca,
so it can't replace the cartridge route for measuring Felucca's fit. It's the better model for the shell around the
engine: screen, panel, storage.

**What it is.** [charlesvestal/fm1-base](https://github.com/charlesvestal/fm1-base) (MPL-2.0, pushed 2026-11-02, 17
forks) is a platform layer for FM-1 firmwares, built on felucca-bsp.
- An app implements a small contract (`template/app.h`):
  - `app_init`;
  - `app_render(int32_t *lr, n, master)`: 24-bit stereo, 256-frame blocks, called from the audio interrupt;
  - `app_frame` (about 60 times a second, draws the 240×240 screen);
  - `app_panic`;
  - optionally a 1 kHz `app_tick` in a timer interrupt.
- The app calls the platform through `include/plat.h`.
- The same app builds for the FM-1, a host simulator (`host/plat_host.c`, 1,655 lines) and the browser.

**How big `plat.h` is.** 352 lines, about 90 functions. Most of it is FM-1 hardware Outrigger doesn't have:
Bluetooth MIDI and Bluetooth audio, Wi-Fi, flash "data windows" at FM-1 flash addresses, the battery, USB host mode,
SysEx updates and key-velocity sensing. An S3 port would implement only the core:

| Core `plat.h` piece | Where it would come from on Outrigger |
|---|---|
| Time | `millis()` |
| Buttons, keys, encoders, MASTER | Launchkey pads, keys and knobs, plus on-screen buttons |
| Lights | On screen, or Launchkey pad colours |
| MIDI in and out | Outrigger's router |
| Settings store | A flash partition |
| Lock and yield | FreeRTOS |

Every other `plat.h` function would be a stub. The host simulator is the model to copy: it does exactly that
(panel from a script, screen to PNG, no radio). My estimate is about the same work as the synth app step was
already sized at (1.5–2.5k lines), but in a reusable shape.

**Does Felucca go through it?** No. Felucca and its forks (Skiff, SLOOP, JIANT, X0X…) have their own platform code.
fm1-base ships a Claude skill (`skill/felucca-to-fm1-base/`) that moves a Felucca-based firmware onto it, one
firmware at a time, at that firmware's author's choice. Native fm1-base apps today are the template, CTL-1 (a
General MIDI synth), LOOM-8 and fm1-radio.

**Memory.** FM-1 apps are sized for 96 KB of RAM plus a 336 KB pool. LOOM-8 uses 88 KB plus 327 KB, and CTL-1 is
491 KB of flash.
- On the S3 the pool goes in PSRAM, as Felucca's lines already do.
- The 88 KB of fast RAM sits beside LVGL and USB. The synth firmware currently leaves 168 KB of static RAM free,
  so LOOM-8 would be tight but plausible.

**Licence.** MPL-2.0 is file-level copyleft. Its files can be part of a GPL-3.0 work and stay MPL-2.0 file by file
(fm1-base's README says so for Felucca-based firmwares). Outrigger (GPL-3.0) can include it if each file keeps its
notice and NOTICE lists it.

**Risks.**
- `plat.h` and `fm1.toml` are marked as changing before 1.0, so a port would chase the API.
- `app_render` and `app_tick` run in interrupts on the FM-1. On the S3 they'd run on the audio task and a timer
  task, so apps that rely on interrupt timing might behave differently.
- Apps store settings at fixed FM-1 flash addresses (`APP_STORE_MAP`), so those would need mapping onto a partition.

**Recommendation.**
- Keep the cartridge route for slice 2. It answers "does a Felucca-class engine fit on one S3 core?", and that CPU
  answer holds whichever platform layer sits on top.
- Build step 3, the synth app shell, as an S3 implementation of `plat.h`'s core rather than a Felucca-specific
  panel. Then fm1-base apps can run too, and Felucca-family firmwares as they move onto fm1-base.
- Decide that after the render numbers are in.
