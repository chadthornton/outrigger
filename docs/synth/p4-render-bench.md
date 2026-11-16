# Felucca on Big-P4: render time (spike night 0, C1-5)

Measured 2026-11-15, 19:47-19:55, on Big-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 rev v3.2, chip rev 3.2,
MAC e8:f6:0a:e8:a5:55), CPU 400 MHz, one core (core 1). Bench `firmware/synth_bench_p4` at 3b17fe7, built and
flashed with `make flash-synth-bench-p4`, console over UART-C (`p4_console.py`). Raw logs are outside the repo
(`~/Projects/spike-ext/a1logs/`).

Same method as the S3 bench (`s3-first-sound.md`): every engine's first preset with 1, 4 and 8 notes held, 1 s to
settle, then 4 s measured. Budget: 1,451 us per 64-frame block at 44.1 kHz. No audio device on the board tonight,
so a 1,451 us timer stands in for the DAC: it takes one block from a 4-block ring per period and counts an underrun
when the ring is empty.

**0 underruns and 0 blocks over budget in every row (39 of 39).**

| Engine (preset) | 1 note avg / max us | 4 notes | 8 notes | Worst share | S3 worst block, 8 notes | S3 / P4 | Capacity, 8 notes |
|---|---|---|---|---|---|---|---|
| ANALOG (SAW LEAD) | 138 / 149 | 138 / 151 | 138 / 148 | 10% | 376 | 2.5x | 10 |
| FM6 (TINE EP) | 148 / 163 | 227 / 252 | 282 / 322 | 22% | 762 | 2.4x | 13 (inflated, ~4) |
| PHASE (BRASS) | 130 / 149 | 178 / 197 | 234 / 253 | 17% | 549 | 2.2x | 6 |
| LOFI (PULSE LD) | 126 / 141 | 126 / 137 | 126 / 134 | 10% | 310 | 2.3x | 11 |
| SAMPLE (PIANO) | 106 / 115 | 106 / 140 | 106 / 152 | 10% | 313 | 2.1x | 13 |
| VOICE (CHOIR AAH) | 166 / 179 | 293 / 304 | 291 / 303 | 21% | 743 | 2.5x | 4 |
| TRIO (FAT BASS) | 152 / 162 | 152 / 163 | 152 / 160 | 11% | 452 | 2.8x | 9 |
| WHEEL (FULL ORGAN) | 155 / 164 | 266 / 277 | 411 / 420 | 29% | 785 | 1.9x | 3 |
| GRAIN (CLOUD PAD) | 149 / 218 | 224 / 308 | 204 / 270 | 21% | 713 | 2.6x | 6 |
| PHYS (BELL TREE) | 240 / 248 | 479 / 488 | 480 / 490 | 34% | 1,028 | 2.1x | 3 |
| NOISE (WIND) | 141 / 149 | 211 / 222 | 307 / 318 | 22% | 724 | 2.3x | 4 |
| SLICE (CHOP) | 106 / 114 | 106 / 115 | 106 / 115 | 8% | 220 | 1.9x | 13 |
| DRUM (DRUM KIT) | 106 / 114 | 106 / 147 | 151 / 175 | 12% | 525 | 3.0x | 13 (inflated, ~8) |

"Worst share" is the worst block against the budget. "S3 / P4" is the S3's worst 8-note block over the P4's.

**How to read it:**
- **The worst case is the S3's worst case, at half the time:** PHYS (BELL TREE) 490 us at worst with 8 notes, 34%
  of the budget (S3: 1,028 us, 71%). Across the table the P4 is 1.9x to 3.0x faster than the S3 (median 2.3x),
  from the same Felucca source. Every engine fits at 8 notes with two thirds of the core spare.
- **About 106 us per block is Felucca's floor** (SAMPLE, SLICE, DRUM at 1 note; S3: about 206 us).
- **Capacity** is renders per block with 8 notes held that still give 0 underruns and 0 blocks over budget for 4 s.
  It runs ONE Felucca instance k times per block, a proxy for k instances' work. The proxy breaks for decaying
  sounds: k renders per block also run its clock k times faster, so FM6's tine and the drum kit die away and the
  bench measures the floor (FM6 at k = 6 renders at 105 us each). For those two the honest figure is the budget over
  the 1-render worst block (1,451 / 322 = 4 for FM6, 1,451 / 175 = 8 for DRUM). The sustained sounds' figures hold:
  **the fewest is 3 (PHYS and WHEEL)**, so one P4 core has room for about three Felucca parts' worst case.

## Float vs double (cycles per operation, 400 MHz, 16,384 operations each)

| Operation | float | double | double / float |
|---|---|---|---|
| multiply-accumulate | 9.1 | 182.3 | 20x |
| divide | 31.2 | 436.5 | 14x |
| sin | 70.6 | 1,581.9 | 22x |
| exp | 153.4 | 2,255.9 | 15x |
| tanh | 186.2 | 3,734.2 | 20x |
| float x double literal (`x * 0.5`) | 386.6 | | 42x a float MAC |

Integer for comparison: Q31 multiply-accumulate 12.1, int64 16.1. The P4 has hardware single precision only
(`-march=rv32imafc...`, `-mabi=ilp32f`), so every double operation is a soft-float call. Felucca itself uses no
float or double at all (its object has no float, double or libm symbols), so this matters only for float code we
write or port: keep it single precision, with `f` literals.
