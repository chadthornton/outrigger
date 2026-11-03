# Outrigger as a synth engine: scope (research, 2026-11-01)

> **Where things stand (2026-11-03):** Outrigger Synth is its own track.
> - **Board:** the ESP32-P4 3.5", with USB audio through a powered hub. See `p4-synth-proposal.md`: its USB probe
>   passed.
> - **Engine:** Felucca compiled from source (§6); AMY is the fallback.
> - **Read this file for:** the background research. Engines and licenses (§2, §4), CPU and USB contention (§5),
>   and FM-1 firmware without an FM-1 (§6).
> - **Outdated:** the S3-only parts of §3 and §5 hold only if the P4 path fails.

> **Update (same day, after the FM-1 question; see §6):** the engine choice has changed. Make **Felucca, compiled
> from source**, the first engine to try. Felucca is GPL-3.0, its DSP is fixed-point, and it has a host API that its
> browser build already uses. AMY becomes the fallback.
>
> **Why Felucca:** a working ESP32-S3 port already exists (andrerezemini/fm1-cartridge). It brings 13 engines, 72
> presets and a sequencer, with no sound design. It's what "FM-1 firmware without an FM-1" actually means.
>
> **The risk is CPU:** that port measured over its time budget on a weaker S3 setup.
>
> **The first slice below still stands:** sine tone, then audio path. Felucca replaces AMY in the second slice. If
> Felucca's render time won't fit in one core, fall back to AMY.

**Recommendation:** adopt **AMY** (shorepine/amy, MIT, C, fixed-point, runs on the ESP32-S3 today) as the sound engine. Run it headless: Outrigger keeps USB, MIDI, I2S and the screen, and AMY only turns note events into samples. Don't port Schwung's synths. They're Linux plugins written for a 1.5 GHz quad-core ARM, and most use `double` and per-sample `tanh`/`exp`, which the S3 runs slowly or in software. A "port" would be a rewrite. Do take Schwung's **arp and chord** MIDI effects (MIT, ~600 lines each, no audio code).

**Size:** a playable synth app is about **3–5k lines of Outrigger code plus vendored AMY**, roughly **3–4 weeks of agent sessions** with Chad's hand tests in between. It breaks into four slices:

| Slice | What it adds | Size |
|---|---|---|
| Sine tone | A tone out of the board | 200–300 lines, 1–2 sessions |
| AMY + router | Router notes play an AMY patch | 500–800 lines, 2–4 sessions |
| Synth app | LVGL pages, broker lease, CPU meter | 1.5–2.5k lines |
| Arp/chord port | Schwung's MIDI effects | ~1.2k ported + ~300 glue |

**Smallest first slice:** a 440 Hz sine out of the **Waveshare 3.5"** speaker:
1. Release EXIO7, so the NS4150B amp turns on.
2. Set up the ES8311 codec over the I2C bus the touch chip already uses.
3. Run I2S at 48 kHz on MCLK 12, BCLK 13, LRCK 15, DOUT 16.
4. Play the tone from an audio task pinned to core 1.

**Pass:** the tone is audible, the chain screen stays green at 8/8 while a controller plays through to the FM-1, and the serial log shows the audio task's CPU share and zero I2S underruns over 10 minutes. The next slice routes one router note into AMY's Juno patch 0.

**Honest limits:**
- The 3.5" has only a small mono speaker output.
- The 1.69" has only a buzzer.
- Real line or headphone audio needs an external I2S DAC (PCM5102A class, ~$5–10), and the free pins for one still need checking on both boards.
- Flash writes (the Loader's store, NVS) will cause dropouts unless the synth pauses during them.

---

## 1. What Schwung is

- **What:** an unofficial framework that runs extra instruments, effects and controllers on the Ableton Move, alongside the stock UI. It was formerly called "Move Everything" and builds on bobbydigitales/move-anything. Its README says it is "heavily written by coding agents".
- **Language:**
  - the host is C (`src/host`, ~176 files) plus jack2;
  - the UI is JavaScript run by QuickJS;
  - modules are C/C++ shared objects (`.so`) built for aarch64 Linux.
- **License:** Schwung's own code is **MIT**. Its release ships GPL parts: jack2 and Ableton Link are separate GPL-2.0+ programs, and the screen-reader shim links eSpeak NG, which makes that one binary GPL-3.0+. **Each module is its own repo with its own license** (see §2).
- **Plugin ABI** (`src/host/plugin_api_v1.h`):
  - `on_midi(msg, len, source)`;
  - `set_param(key, val)` and `get_param`, both string-keyed;
  - `render_block(int16_t *out_interleaved_lr, frames)`, always **128 frames at 44.1 kHz, stereo int16**.

  v2 is the same with per-instance state. The ABI is clean and small, and it's the most reusable idea in the repo.
- **Catalog** (`module-catalog.json`): 174 modules.
  - **Kinds:** 73 sound generators, 50 audio FX, 19 tools, 18 "overtake" (full-screen apps), 14 MIDI FX.
  - **Sound generators include:**
    - emulations: Dexed (DX7), OB-Xd, Surge, Hera (Juno-60), Osirus (Virus DSP56300 emulation), JP-8000, mini-JV (JV-880 ROM emulation);
    - samplers and players: SF2 (FluidLite), SFZ (sfizz);
    - Mutable ports: Braids, Plaits;
    - monosynths and voices: 303, SH-101, Moog-style mono, nusaw (supersaw), Hank (2-op FM), tablor (wavetable);
    - drum machines: many;
    - streamers: AirPlay, web radio, yt-dlp.
  - **In-tree MIDI FX:** arp and chord.
- **How it couples to the Move:**
  - It injects a shim into Move's own process.
  - It owns `/dev/ablspi0.0`, the SPI link to the pads/encoders/LED controller: 768-byte frames at 20 MHz, with mailbox and cable-number conventions.
  - It draws to the 128×64 1-bit display, mixes into Move's audio path through shared memory, uses Move's tempo and Link, and installs over SSH.
  - None of that exists on an ESP32. Only the module DSP and the ABI shape could move.
- **Move hardware.** These are community teardown figures, not Ableton specs:
  - Raspberry Pi **Compute Module 4**: BCM2711, **4× Cortex-A72 at 1.5 GHz**, 64-bit, NEON SIMD, double-precision FPU.
  - **2 or 4 GB RAM**: sources conflict; Ableton's FAQ, as relayed on KVR, says 2 GB.
  - Linux on microSD.
  - Probably an XMOS chip for audio/IO, unconfirmed.

  Against the S3, which has 2× 240 MHz LX7 cores, a single-precision FPU only, 512 KB SRAM and 8 MB PSRAM behind a cache, the Move has very roughly **30–50× the DSP throughput** and ~250× the memory. That ratio is my estimate, not a measurement.

## 2. What could run on the ESP32-S3

**Method, so the numbers can be checked.** At 48 kHz, one core has 240 MHz ÷ 48,000 = **5,000 cycles per sample**. Assume the synth gets about half a core (2,500 cycles per sample), and leave the rest for LVGL and the loader. The per-voice costs below are my estimates for single-precision float or fixed-point code with tables in internal RAM. **Measure them in slice 2.**

The S3's PIE SIMD is integer-only (128-bit vectors of int8/16/32). That favors fixed-point engines like AMY and ESP32Synth, and it doesn't help float code.

| Module type | Est. cycles/voice/sample | Est. voices (½ core, 48 kHz) |
|---|---|---|
| Table oscillator (sine or wavetable, linear interp) | 25–40 | 60+ oscillators |
| 2-op FM + 2 envelopes | 80–120 | 20–30 |
| Wavetable, 2 osc with morph + SVF filter + 2 envelopes | 150–200 | 12–16 |
| Subtractive (2 band-limited osc + SVF + 2 ADSR) | 150–250 | 10–16 |
| 4-op FM | ~200 | ~12 |
| 6-op DX7-style (MSFA is fixed point) | 350–450 | 5–7 |
| Supersaw (7 saws + filter) | ~300 | 6–8 |
| Ladder filter with real `tanhf` per stage | 500+ | 4–5 (2–3× more with a tanh approximation) |
| Arpeggiator, chord, step sequencer | MIDI-rate, ~0 | n/a |

**Claims from other projects** (theirs, not verified):
- Tulip CC (ESP32-S3 + LVGL + AMY) advertises a "120-oscillator" engine.
- ESP32Synth claims 150+ voices by default and up to 500 on the S3. Its "voice" is one oscillator with an envelope, not a full synth voice.

An AMY Juno patch spends several oscillators per voice, so 120 oscillators is roughly 15–25 Juno voices across both cores. Outrigger can't give the synth both cores.

**Schwung modules, sorted:**
- **Portable as-is (MIDI only, MIT):** `arp.c` and `chord.c`.
- **Portable with rework** (MIT, ~1.7–4k lines, float/`double` code to rewrite):
  - nusaw: 8 voices × 7 saws, double counters;
  - moog: mono, ladder;
  - hush1: SH-101.

  At least 4–8 voices each is realistic after the rework. It's cheaper to get the same sounds from AMY patches.
- **Better from upstream:** Braids and Plaits. Mutable's own repo is MIT; Schwung's Braids repo says NOASSERTION. Braids ran on a 72 MHz Cortex-M3 with no FPU, so 2–4 voices on the S3 is plausible. Plaits ran on a 72 MHz M4F, so 1–2 voices is plausible. Good later add-ons.
- **Unusable for licensing:** repos with no license are all rights reserved: Hank, TinyK, libpo32. NOASSERTION repos (303, tablor, Braids wrapper) need a per-file check first.
- **Can't run, for CPU, memory or OS reasons:**
  - Osirus/Virus and JP-8000: DSP56300 emulation through a JIT, which needs a multi-GHz desktop-class CPU.
  - mini-JV: ROM emulation.
  - Surge: huge, built around SSE float SIMD.
  - OB-Xd and Helm: oversampled float/double.
  - sfizz and FluidLite at Move scale.
  - All streaming modules, which need Linux, networking and ffmpeg.
  - Anything that uses JACK or Link.

  Dexed/MSFA is borderline: GPL-3.0, which is compatible, but 5–7 voices at best, and AMY's DX7 mode covers it.

## 3. Audio out

- **Waveshare 3.5"** (ESP32-S3-Touch-LCD-3.5):
  - **Codec:** an ES8311 at I2C 0x18 on SDA 8/SCL 7, shared with the touch chip, the TCA9554 expander, the AXP2101, the RTC and the IMU.
  - **I2S pins:** MCLK 12, BCLK 13, LRCK 15, DOUT 16, from Waveshare's `01_audio_out` example. That example runs at 48 kHz with MCLK = 256 × fs. Waveshare's own Arduino ES8311 driver is in their repo.
  - **Amplifier:** an NS4150B mono class-D, enabled by EXIO7, which Outrigger holds low today.
  - **What you can plug in:** Waveshare lists only a "microphone and speaker connector". The ES8311 is a mono codec. So this board makes a **toy-speaker mono synth**. I did not check the schematic for a line-out tap, so check that before promising headphones.
  - **Spare pins for an external DAC:** almost none. These are taken:
    - LCD: 1–3, 5, 6
    - I2C: 7, 8
    - SD: 9–11
    - I2S: 12, 13, 15, 16
    - USB: 19, 20
    - camera FPC: 17, 18, 21, 38–48

    The camera pins are free when no camera is fitted, but they're reachable only through the FPC connector.
  - **Shared I2C:** codec writes (volume, mute) must come from the same task that polls touch, or take a lock.
- **Waveshare 1.69":**
  - **Built-in sound:** none beyond the **buzzer on GPIO42**. PWM or sigma-delta through it gives toy-grade 1-bit sound and proves nothing about a synth.
  - **External DAC:** a PCM5102A needs 3 GPIOs (BCLK, LRCK, DIN; it makes its own MCLK).
  - **Pins taken:** 4–8, 10, 11, 13–15, 42, USB 19/20, octal PSRAM 33–37, flash 27–32.
  - **Spare-pin candidates:** 1–3, 9, 12, 16–18, 21, 38–41. **Unverified:** check Waveshare's pinout for which of them reach a pad or header, and which carry the battery ADC or the power key.
- **I2S DAC options:**

  | DAC | Price | Pins | Notes |
  |---|---|---|---|
  | PCM5102A breakout | $5–10 | 3, no MCLK | Line out, the default pick |
  | UDA1334A (Adafruit) | ~$7 | 3 | Line/headphone |
  | MAX98357A | ~$6 | 3 | Mono 3 W speaker amp |
  | WM8960 / ES8388 codecs | — | 3 + I2C setup | In and out |

- **USB audio class output.** There are two separate answers:
  - **Outrigger as a USB audio device to a computer: impossible while it hosts MIDI.** The S3 has one USB OTG controller and it's busy as host. The USB-Serial-JTAG block is fixed-function.
  - **Outrigger sending audio to a USB DAC or interface on the hub: possible in principle.**
    - EspUsbHost 2.10.1 has an **experimental** UAC1/UAC2 isochronous OUT path, peer-tested only.
    - The S3 host is full-speed only, so UAC2-only devices fail.
    - A DAC costs about 2–3 of the S3's **8 host channels**, which the hub, FM-1 and controllers already compete for.
    - Isochronous traffic through a hub is the riskiest thing Outrigger could add to its USB chain.

    Not a first choice.

## 4. Other engines, ranked for Outrigger

Outrigger is **GPL-3.0** (`LICENSE` on master; GitHub reports gpl-3.0). MIT, BSD, Apache, LGPL and GPL-3 sources are all compatible. The blockers are missing licenses, closed binaries and odd licenses.

| # | Engine | License | Fit | Notes |
|---|---|---|---|---|
| 1 | **AMY** (shorepine) | MIT | **Best** | Details below the table. |
| 2 | **Write minimal modules** | ours | Good, slow | Full control and smallest footprint, but every new sound costs design and tuning time. Borrow blocks (SVF, ADSR, polyBLEP oscillator) from DaisySP (MIT core; check the repo's NOASSERTION per file). Use for anything AMY lacks. |
| 3 | **Mutable Braids/Plaits** (upstream) | MIT | Good add-on | One macro-oscillator "voice" with dozens of models, proven on 72 MHz MCUs. A second engine after AMY. |
| 4 | **ESP32Synth** (danilogcrf2-oss) | MIT, **but bundles the Helix MP3 decoder** (RealNetworks RPSL/RCSL, not MIT; exclude it) | Medium | Integer and SIMD, high oscillator counts. Strong on S3 efficiency, but it's an oscillator/sample engine with fewer synth models, a young project with unverified claims, and it wants its own core-1 task and I2S. |
| 5 | **Mozzi** | LGPL-2.1 | Poor | Built for 8-bit AVR: mostly mono, low control rate, and it owns the audio output loop. |
| 6 | **ML_SynthTools** (Marcel Licence) | Labelled GPL-3.0, but the core ships as **precompiled `.a` libraries** | Poor | Can't inspect, fix or rebuild it for 3.3.x. Its README recommends arduino-esp32 ≤ 2.0.17. |
| 7 | **Teensy Audio Library** | MIT-style | Ideas only | Tied to Teensy's Cortex-M DMA and CMSIS-DSP. |
| 8 | **Daisy (DaisySP)** | MIT | Ideas, blocks | The STM32H750 is a 480 MHz Cortex-M7 with a double FPU, about 2–3× one S3 core for float DSP. The library is portable float C++. |
| 9 | **Dirtywave M8, Polyend Tracker** | Closed | Reference only | i.MX RT1062 at 600 MHz. Product inspiration, no code. |
| 10 | **Pico/RP2040/RP2350 synths** | Various | No gain | The RP2040 has no FPU. AMY itself runs on RP2350, so nothing here beats adopting AMY. |

**Why AMY is first:**
- **Sounds:** Juno-6 and DX7 patch sets, wavetable, FM, filters, additive, Karplus-Strong, PCM, and a sequencer.
- **Speed:** fixed-point throughout (`AMY_USE_FIXEDPOINT`), and it can render on both cores.
- **Headless:** checked in source.
  - `amy_config` accepts `AMY_AUDIO_IS_NONE` and `AMY_MIDI_IS_NONE`.
  - `amy_simple_fill_buffer()` returns an int16 block for our own I2S writer.
  - `amy_event_midi_message_received()` takes raw MIDI bytes from our router.
  - TinyUSB is used only for its USB-gadget mode, so it won't touch our host stack when that mode is off.
- **Block size:** set at compile time (`BLOCK_SIZE_BITS`; 7 = 128 samples).
- **Memory:** `ram_caps_*` knobs choose internal RAM or PSRAM for each kind of state.
- **Proven alongside LVGL:** Tulip CC already runs it on an S3 with LVGL.

**AMY's risks:**
- Its docs say "tested arduino-esp32 3.2.0", and Outrigger is on 3.3.x.
- The source is ~145k lines because it includes large PCM tables (`pcm_samples_gamma808.h` 2.5 MB of source, `pcm_samples_tiny.h` 1 MB). Check the compiled size against our **3 MB app0 slot** and build with the tiny or no PCM set.
- Default `max_oscs` is 250. Size it down.

## 5. Sharing the CPU and the USB task

- **Today:**
  - EspUsbHost's task runs at priority 5 with **no core affinity** (`tskNO_AFFINITY`).
  - LVGL runs in Arduino `loop()` on core 1.
  - `httpd` runs on core 0 at priority 3, and Wi-Fi runs on core 0.
  - The loader task runs on core 1 at priority 2.
- **Proposed layout:**
  - Pin the USB host task to **core 0**, with Wi-Fi and httpd.
  - Put the **audio task on core 1** at a priority above `loop()` and the loader, so LVGL redraws yield to audio.
  - Audio blocks on the I2S write, so it uses CPU only for its render burst.
- **Event hand-off:** the router callback on the USB task pushes note events into a lock-free queue, and the audio task drains it at each block. Never call AMY from the USB task. The FM-1 forwarding path doesn't change.
- **Latency:** a 128-sample block at 48 kHz is 2.67 ms. With 2–3 DMA buffers queued, note to sound is about **5–8 ms**, plus ≤1 ms of USB polling.
- **Jitter:** events land on block boundaries, so they can be off by **up to 2.67 ms** unless they're timestamped. A 64-sample block halves that, at more per-block overhead. Fine for playing, audible on tight external clock sync.
- **Flash writes stall audio. This is the biggest real risk.**
  - On the S3, a flash write or erase (LittleFS store writes, NVS) disables the cache that also serves octal PSRAM, and it stalls code on the other core that isn't in IRAM.
  - A 4 KB sector erase takes tens of milliseconds, so the I2S DMA runs dry and you hear a dropout.
  - Hub + Loader ships with writes on.
  - **Fix:** a broker lease. The synth fades and pauses while a Loader job or settings save writes flash, and the screen says so. Keeping the whole audio path in IRAM/DRAM is the harder alternative, and Arduino's precompiled core limits it.
- **PSRAM:** fine for wavetables and samples that are read sequentially. Keep per-voice state and hot tables in internal RAM; AMY's comments warn about PSRAM cache-miss stalls. Internal SRAM is the scarce budget, so measure free heap after AMY starts.
- **USB starvation:** the open question from the router spike (whether LVGL starves the USB task) gets easier once USB is pinned to core 0, away from both LVGL and audio. Re-run the 8/8 chain check with the synth playing.

## 6. FM-1 firmware without an FM-1

**Verdicts:**
- **(b) Port from source: yes for the Felucca family; no for stock.** This is the lead option. Felucca, Skiff,
  SLOOP, JIANT, X0X, Clipper and the other forks are GPL-3.0 C with a host layer. That host layer already runs them
  in a browser (WASM), on desktops and in libretro. Someone already runs three of them on an ESP32-S3. CPU is the
  open question.
- **(a) Emulate the JieLi chip: no.** A chip emulator exists, but it's QEMU on desktops. On a 240 MHz S3 it would
  run 15–75× too slowly.
- **(c) An open FM engine instead: already covered.** Felucca's FM6 engine is Dexed/MSFA (Apache-2.0) and ships with
  the port. AMY's DX7 mode is the fallback. Stock FM-1 patch import is blocked: the stock voice format isn't
  documented.
- **(d) Control proxy for a real FM-1: easy for CC remaps; full reach on Felucca-family firmware; thin on stock.**
  Felucca has on-device MIDI learn plus a SysEx editor protocol that covers every parameter. Stock M-VAVE documents
  only a "CC guide" that I didn't find, and owners report that only CC1 and pitch bend work.

### (b) Porting Felucca-family firmware from source (lead option)

**What's open:**

| Firmware | License | Host/sim layer | Notes |
|---|---|---|---|
| **Felucca** (hugelton/Felucca, 622★) | GPL-3.0-only | `platforms/host/felucca_host.{c,h}`, `platforms/web` (WASM + AudioWorklet), SDL, libretro | The reference. Its hardware layer, felucca-bsp, is MPL-2.0. Bundled third parts: MSFA (Apache-2.0), Rings and DaisySP (MIT), snes_spc (LGPL-2.1). |
| **Skiff** (illuminus5/skiff) | GPL-3.0 | `tests/hostsim.c`, `tests/c1_host.h` | Felucca fork. |
| **SLOOP** (isod89/sloop-fm1) | GPL-3.0 | Web editor with a preview worklet | Felucca 0.9-based. |
| **JIANT** (juanjiant-bit/JIANT) | GPL-3.0 | `tests/hostsim.c`, web emulator (the one Chad found) | Felucca family. |
| **X0X** (charlesvestal/fm1-x0x) | GPL-3.0 | `host/x0x_host.c` | Felucca fork. |
| Clipper, Melodee, Caravel, Jangada, Hortator… | GPL-3.0 (most) | Mostly hostsim | Same structure. |
| **Stock M-VAVE** (m-vave/fm1_open) | App code GPL-3.0-only with a linking exception | — | **The synth engine is `sdk/lib/libfm1_engine.a`, precompiled for JieLi with no source.** Stock sound can't be ported, only emulated, and emulation is ruled out. |

Outrigger's LICENSE is "GPL, version 3", the same terms as Felucca's GPL-3.0-only, so the licenses fit.

**The host API is the seam.** `felucca_host.h` is one-threaded and builds as one translation unit:
- `fh_render(n)`: the device's clock advances with the audio asked for, and the output is float −1..1 at 44.1 kHz.
- Buttons by printed label, keys as bits, encoders as detents.
- `fh_midi`: the USB-MIDI packet form, which is what Outrigger's router already has.
- The screen: 240×240 RGB565.
- LED state.
- `fh_nor*`: the FM-1's 1 MiB NOR, which the platform keeps.

The web build's `web_render`, `web_out_l/r`, `web_buttons`, `web_keys`, `web_enc`, `web_midi`, `web_screen` and
`web_nor` are thin exports of these functions. An Outrigger layer would be the same thin wrapper.

**Prior art on our exact chip: andrerezemini/fm1-cartridge** (GPL-3.0, Oct 2026, experimental). It's an ESP32-S3
Super Mini that plugs into an FM-1:
- It renders Clipper 1.3, Felucca 1.7.0 or Melodee 1.5.1 from their upstream DSP source.
- It sends the audio back to the FM-1 over USB audio.
- It ships native tests and a simulator.

Its notes (`docs/PORTS.md`) say:
- The DSPs are fixed-point with signed wrap, built with `-fwrapv`. Melodee's FM6 is float.
- The tables are 44.1 kHz only.
- It renders in 32-frame control blocks on one core. The FM-1's second core isn't ported.
- It renders one part of four.
- It runs 8 voices per part.

Its measured render times (`docs/BENCH_TESTS.md`, G4) are against a budget of 1,451 µs per 64-frame block (one block
at 44.1 kHz). The setup was poor: DIO flash, quad PSRAM at 40 MHz, code run from flash.

| Firmware | Patch, playing | Render time | Share of budget | Underruns |
|---|---|---|---|---|
| Clipper | not recorded | 673 µs | 46% | 0 |
| Felucca | ANALOG SAW LEAD, one key | 765 µs | 53% | 22 |
| Felucca | engine not recorded | 1,854 µs | 128% | 78 |
| Melodee PROPHET, first build | one key | 1,583 µs | over budget | 31 |
| Melodee PROPHET, after tuning | chords | 2,379 µs peak | over budget | 0 |

The tuning (QIO flash, PSRAM at 80 MHz, the hot loops moved into internal RAM) is what brought Melodee to zero
underruns. Felucca's ANALOG still counted underruns while inside its budget, a cause the port hasn't identified.

**CPU, honestly:**
- **The FM-1's chip:** a JieLi AC79-family part (the FM-1's AC7911 / WL82). Felucca's own docs call it a "360 MHz
  DSP", with an optional second core that's off by default.
- **What Felucca fits on it:** 4 parts × 8 voices on one core. 8 ANALOG voices on one part cost **17% of the FM-1
  core** (Felucca's BUILDING.md).
- **Heavy engines cost more.** Felucca's instruction budgets (`tests/target_budget.txt`): `analog_render` is 614,
  `grain_render` is 1,723.
- **The ESP32-S3:** one 240 MHz LX7 core is, by my estimate, about **0.5–0.7× one FM-1 core** on this code. That's
  an estimate, not a measurement.
- **What fits:**
  - Expected to fit: one part of 8 voices of ANALOG, LOFI, PHASE, TRIO or SAMPLE on one S3 core.
  - Doubtful: FM6, GRAIN or PHYS at 8 voices.
  - Doesn't fit: Felucca's four-part heavy test song.
- **Outrigger's advantage:** our boards have **octal PSRAM at 80 MHz** and 16 MB QIO-capable flash. That's better
  than the cartridge's 2 MB quad PSRAM at 40 MHz, which is the setting that hurt it most.

**RAM:**
- **On the FM-1:** Felucca uses ~456 KiB of SRAM: 24 KiB of RAM-run code, 72 KiB `.data/.bss`, and a 352–360 KiB
  `.pool` of large buffers (FX lines, canvases, engine memory) (`firmware/app.ld`).
- **On the S3:** the pool won't fit in internal SRAM next to LVGL, USB and Wi-Fi. Put `.pool` in PSRAM and keep the
  interrupt-touched `.data/.bss` and hot code in internal RAM/IRAM. One part instead of four also shrinks the engine
  memory. Melodee's single-part arena peaked at ~31 KB.
- **The 1 MiB NOR image** goes in a flash partition, or in PSRAM with lazy saves. Flash writes stall audio (§5), so
  save only when the transport is stopped. `fh_flash_writes` says when a save is due.

**How the FM-1's panel maps onto Outrigger:**
- **Keys:** come from the Launchkey via `fh_midi`, or via `fh_keys` for the FM-1's two-octave key bitmask.
- **The four knobs plus SELECT:** come from Launchkey knobs, converted from absolute CC to detents (send the delta).
- **Buttons:** FX, GLO, PLAY, OCT− and OCT+ go on Launchkey pads or buttons.
- **MASTER:** a fader.
- **Screen:** the 240×240 frame drawn as an LVGL canvas.
  - The **1.69" (240×280) shows it 1:1**, with 40 px to spare.
  - The 3.5" (480×320 landscape) puts it beside an on-screen panel.
- **LEDs:** drawn on screen, or mirrored to Launchkey pad colors.

**Size:** about **1.5–2.5k lines** of Outrigger glue:

| Piece | Lines |
|---|---|
| Audio task and codec (shared with the sine slice) | ~300 |
| Launchkey ↔ panel map | 300–500 |
| Screen canvas and on-screen panel | 300–500 |
| NOR partition and save policy | ~200 |
| App shell and broker lease | ~300 |

On top of that:
- **Build:** bring in Felucca's generated tables (`./build.sh gen`) and compile it as one translation unit under
  arduino-cli.
- **Flash size:** check it against our 3 MB app0 slot. `felucca.wasm` is 1.1 MB, a rough proxy.
- **Performance tuning:** IRAM/PSRAM placement is the uncertain part, and it may take as long as the glue.

That's roughly **2–3 weeks of sessions**, and it replaces most of the AMY slices.

**Smallest first slice: Felucca playing from the Launchkey, no FM-1 screen yet.**
1. **Prerequisite:** the sine-tone first slice (top of this file) works on the 3.5" (ES8311 + I2S + an audio task on core 1).
2. **Build:** a diagnostic build that includes `felucca_host.c`, with a RAM-backed blank 1 MiB NOR in PSRAM, then
   `fh_boot()`.
3. **Audio:** the audio task calls `fh_render(64)`, converts float to int16 and writes I2S at **44.1 kHz** (ES8311
   MCLK = 256 × 44.1 kHz).
4. **MIDI:** Router Launchkey notes go through a queue into `fh_midi` on the audio task.
5. **Pass:**
   - The ANALOG preset plays chords from the Launchkey.
   - The serial log shows render µs per block: maximum and average for 1, 4 and 8 held notes, against the 1,451 µs
     budget.
   - Zero underruns over 10 minutes.
   - The chain screen stays green.
6. **Same build, extra numbers:** FM6 and GRAIN render times. That settles "Felucca or AMY" on data instead of
   estimates.

### (a) Emulating the JieLi core

- **What exists:** simonjohansson/fm1-emulator (GPL-2.0) runs real FM-1 `.bin`/`.fwsc` images on a QEMU `pi32v2` CPU
  and board model, on macOS and Linux. markterm/harbor runs FM-1 firmwares on Android using a similar emulator.
- **The S3 can't do it:**
  - Interpreting a 360 MHz DSP's instructions costs about 10–50 host instructions each. A 240 MHz LX7 would run
    **~15–75× short** of real time, before modeling the audio DMA, the timers and the interrupts the firmware's
    timing depends on.
  - pi32v2 is JieLi's own instruction set, documented only through reverse engineering.
  - QEMU's dynamic translator doesn't target Xtensa hosts.
- **The only thing emulation would add is stock M-VAVE's sound,** because its engine has no source. It isn't
  achievable on this hardware.

### (c) An open FM engine with FM-1-like voicing

- **Felucca ships the FM answer already:** FM6 is a 6-operator Dexed/MSFA engine (Apache-2.0).
- **The 4-operator engine is retired but kept.** Felucca's old 4-op DIGITAL engine is in the tree (`eng_digital.c`,
  `FELUCCA_FM4=1`). `fm4_convert.c` maps its 8 algorithms, ratios and index onto FM6 patches, which is a documented
  4-op → 6-op translation table.
- **Two options:**
  - for a 4-op voice cheaper than FM6 on the S3, build with `FELUCCA_FM4=1`;
  - otherwise use AMY's DX7/FM mode.
- **Stock FM-1 patch import: blocked.**
  - I found no public description of M-VAVE's voice format.
  - The factory voices live at flash `0xE9000–0xFBFFF` (our loader plan), so Outrigger's ROM-boot backups already
    contain them as bytes.
  - Decoding them would be reverse engineering. That's a separate, uncertain project, and redistributing M-VAVE's
    voices raises license questions.
  - Defer it.

### (d) Launchkey as a control proxy for a real FM-1

- **The CC-remap half is cheap and safe.**
  - A router table maps each Launchkey knob's CC number to a target CC on the FM-1's channel, forwarded on the USB
    task as it is today.
  - About 150–300 lines plus tests, and it's light tier.
  - It needs a per-firmware map, chosen by the family that Outrigger's identity checks already detect.
- **Felucca family:**
  - **MIDI LEARN on the device:** hold GLO + D4, turn a knob, send a CC. Any knob's parameter on any page becomes
    CC-controllable, and the map is saved in the FM-1's settings. So Outrigger needs no FM-1-specific table: the user
    learns the Launchkey's knobs once.
  - **The editor SysEx protocol** (`F0 7D 46 4C cmd …`; GET, SET, DESC, track params, presets, steps; 14-bit
    values) reaches every parameter, the presets and the sequencer steps. It doesn't reach button navigation.
  - **The catch with SysEx:** Outrigger sending SysEx to the FM-1 crosses the "only the Loader sends SysEx" rule,
    which is guarded tier and the same carve-out question as the Mirror scope. CC remap plus on-device learn avoids
    it entirely.
- **Stock M-VAVE: thin.**
  - V15 added a "CC parameter guide" that I couldn't retrieve.
  - Owners on Elektronauts report CC1 (mod) and pitch bend working, and CC7, 10, 11, 91 and 93 doing nothing,
    possibly because of a separate effects channel.
  - Get the V15 guide from M-VAVE's download page and test it on the bench before promising stock coverage.

## 7. The ESP32-P4 3.5" (ESP32-P4-WIFI6-Touch-LCD-3.5): a better synth board, if the hub works

> **Superseded by `p4-synth-proposal.md`.** That file has the schematic-verified USB facts. Corrections:
> - Only the OTG USB-C can host.
> - The second USB controller is reachable only by header wiring, so "USB audio to a computer" is out without
>   pin work.
> - The hub needs EspUsbHost's experimental forced-full-speed mode.

Chad has one (photo, 2026-11-01). The silkscreen shows:
- ESP32-P4NRW32 with **32 MB PSRAM** and 16 MB flash;
- ESP32-C6 as the Wi-Fi co-processor;
- an ES8311 codec, a speaker connector and a mic;
- AXP2101 power management;
- the same ST7796 and FT6336 as the S3 3.5";
- two USB-C ports (USB-C and UART-C);
- a header with DP/DM and **about 17 free GPIOs**: 2–5, 30–36, 46–48, 51, 52.

**Better for the synth:**
- **CPU (estimate):** each of its two RISC-V cores is roughly on par with the FM-1's 360 MHz DSP, and about 1.5–2×
  an S3 core on Felucca's integer code. Felucca's four parts at FM-1 settings become plausible, not just one part.
  This is a P4 datasheet-class figure, not measured.
- **Memory:** much more internal SRAM than the S3 (768 KB class vs 512 KB) and much faster in-package PSRAM.
  Felucca's 360 KiB pool fits comfortably.
- **Real audio out:** the free header pins make an external I2S DAC (PCM5102A) a simple wiring job. The onboard
  ES8311 is still a mono speaker path.
- **Possible USB audio to a computer:** the P4 has two USB controllers (high-speed OTG and full-speed OTG). In
  principle, one could host the hub and MIDI while the other acts as a USB audio device to a computer. Unverified:
  which USB-C and which header pins reach which controller has to come from the schematic.

**What gates it:** the same open question as in board-support's `docs/boards.md`. EspUsbHost says hubs are
"effectively not usable" on the P4's high-speed port. The full-speed port takes hubs but has only 8 channels, like
the S3. Outrigger has no P4 build yet; arduino-esp32 3.x does support the P4.

**Advice:**
1. Board support's planned hub spike on this exact board comes first.
2. If the hub works, this becomes the preferred "Outrigger synth" board.
3. Until then, prove the synth on the S3 3.5". The audio code carries over: same ES8311, same Felucca host layer.

## Sources

- Schwung repo: https://github.com/charlesvestal/schwung (cloned 2026-11-01; `README.md`, `CLAUDE.md`, `module-catalog.json`, `src/host/plugin_api_v1.h`, `src/modules/midi_fx/{arp,chord}`). Module licenses came from the GitHub API.
- Move hardware: https://recapio.com/digest/raspberry-pi-compute-module-is-the-brain-of-ableton-move-by-raspberry-pi, https://forum.ableton.com/viewtopic.php?p=1829843, https://www.kvraudio.com/forum/viewtopic.php?p=8976389
- AMY: https://github.com/shorepine/amy (`README.md`, `docs/arduino.md`, `src/amy.h`, `src/api.c`). Tulip CC: https://github.com/shorepine/tulipcc
- ESP32Synth: https://github.com/danilogcrf2-oss/ESP32Synth, https://hackaday.com/2026/04/23/esp32synth-an-audio-synthesis-library-for-the-esp32/
- ML_SynthTools: https://github.com/marcel-licence/ML_SynthTools
- Waveshare 3.5": https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.5 (`examples/arduino/examples/01_audio_out`). Board-support notes: `docs/boards.md` on `claude/board-support-c5f996`.
- FM-1 section (§6):
  - Felucca: https://github.com/hugelton/Felucca. Cloned 2026-11-01: `platforms/host/felucca_host.h`,
    `platforms/README.md`, `docs/PORTING.md`, `BUILDING.md`, `firmware/app.ld`, `firmware/src/{core.h,fm4_convert.c,midi_learn.c,editor.c}`,
    `tests/target_budget.txt`.
  - fm1-cartridge: https://github.com/andrerezemini/fm1-cartridge (`README.md`, `docs/PORTS.md`, `docs/ROADMAP.md`,
    `docs/BENCH_TESTS.md`).
  - Emulators: https://github.com/simonjohansson/fm1-emulator and https://juanjiant-bit.github.io/JIANT/emu/
    (orchestrator's reading).
  - M-VAVE: https://github.com/m-vave/fm1_open (`LICENSE.md`, `sdk/lib/libfm1_engine.a`).
  - Fork list: GitHub search for Skiff, SLOOP, JIANT and X0X, with their licenses.
  - Stock CCs: https://www.elektronauts.com/t/m-vave-fm-1/252170?page=2 and
    https://www.matrixsynth.com/2026/07/m-vave-fm-1-v15-firmware-update-new.html
- Outrigger:
  - `lib/EspUsbHost/README.md`: UAC status and channel limits.
  - `docs/architecture.md`: tasks, priorities, 8 host channels.
  - `Makefile`: 3 MB app0.
  - `LICENSE` on master.
