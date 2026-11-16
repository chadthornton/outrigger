# Vibes on Big-P4: timing (spike night 1, track HW3)

S1's Vibe bench images (branch `spike/platform-p4-s1-vibe`, PR #117) run on Big-P4 (ESP32-P4 rev 3.2, 400 MHz), measured 2026-11-16 10:00–10:18.
Both Vibes are VIBE FM SDK `example-synth` and S1's Faust patch (saw, resonant low-pass, ADSR; 8 voices).

**Answer.** WAMR AOT runs both Vibes in real time with room to spare: 8 notes take 34–37% of a 64-frame block at worst.
Both interpreters miss real time from 4 notes on, and classic misses even at 1 note for example-synth. On the P4, AOT costs about 4x native, not the Mac's 1.35–1.5x.
Every interpreter and AOT row that ran matched the Mac's / SDK Node host's `out_hash` bit for bit, so the riscv32 AOT path and S1's WAMR patch work on hardware.

## 64-frame blocks, us per block, held mean / max (% of 1,451 us)

The budget is 64 frames at 44.1 kHz = 1,451 us. The bench's scenario runs at 48 kHz (its hashes depend on that); time per block depends on the frame count, not the rate.
"held" is the mean while the notes are held; max is the worst block of the run (native's max is its first, cold block).

| Vibe | runtime | 1 note | 4 notes | 8 notes |
|---|---|---|---|---|
| example-synth | WAMR classic interp | 1966 / 2377 (135% / 164%) | 4429 / 4872 (305% / 336%) | 7716 / 8276 (532% / 570%) |
| example-synth | WAMR fast interp | load failed | 2270 / 2500 (156% / 172%) | 4003 / 4251 (276% / 293%) |
| example-synth | WAMR AOT | 116 / 161 (8% / 11%) | 278 / 318 (19% / 22%) | 488 / 530 (34% / 37%) |
| example-synth | native C | 33 / 195 (2% / 13%) | 70 / 220 (5% / 15%) | 118 / 276 (8% / 19%) |
| faust-synth | WAMR classic interp | 1070 / 1330 (74% / 92%) | 3184 / 3488 (219% / 240%) | 6006 / 6377 (414% / 439%) |
| faust-synth | WAMR fast interp | crash (heap assert) | crash | crash |
| faust-synth | WAMR AOT | 84 / 112 (6% / 8%) | 252 / 281 (17% / 19%) | 477 / 508 (33% / 35%) |
| faust-synth | native C | 23 / 166 (2% / 11%) | 64 / 207 (4% / 14%) | 117 / 265 (8% / 18%) |

Sources: classic and AOT and native from the `classic` image (faust-synth from its standalone `bf` rerun); fast from the `fast` image.
AOT rows on the `fast` and `fast-int` images agree with the classic image's within 1 us.

**128-frame blocks** (classic image, 18/18 rows match): time scales linearly. 8 notes, held mean: AOT 966 / 932 us, native 234 / 229 us, classic 15,392 / 11,804 us (example / faust), budget 2,902 us at 44.1 kHz.

**Native state in internal SRAM vs PSRAM** (`fast-int` image, example-synth): 32.0 / 69.1 / 117.5 us vs 33.3 / 69.5 / 117.7 us. No difference.

## What's broken

- **The fast interpreter is unreliable on the P4.** example-synth's module sometimes fails to load ("fast interpreter offset overflow": 3 of 6 loads) and loads fine otherwise.
  faust-synth's run on the fast image panics before its first row, twice, with `assert failed: block_trim_free tlsf_control_functions.h:548 (block must be free)`: heap corruption. Not diagnosed.
- **The classic interpreter failed one load** of faust-synth ("type mismatch: stack size does not match block type") in 1 of 6 loads; the rerun loaded all three. The wasm buffer is copied fresh per load (`vh_wamr.c`), so it isn't loader rewriting. Not diagnosed.
- Why AOT costs ~4x native here (vs 1.35–1.5x on the Mac) is open: software bounds checks and the 832 KB linear memory in PSRAM are the obvious suspects.

## How it was run

- Flashed as pieces (bootloader 0x2000, table 0x8000, app 0x10000), never S1's merged image at 0x0: its padding would erase NVS (Big-P4's saved Wi-Fi). MAC `e8:f6:0a:e8:a5:55` checked with a hard reset before each flash.
- `tools/vibe_console.py` from S1's branch, commands `ibB` / `bf` / `bs` (64 frames) and `B` (128). Logs in `~/Projects/spike-ext/hw3logs/` (outside the repo).
- Afterwards the hub firmware b121cbd went back on with `tools/p4_flash.sh` (boot 12, Wi-Fi joined, 3 USB devices, both robots `mounted=1`).
