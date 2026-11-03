# Vendored for Outrigger Synth

## fm1-cartridge/ (GPL-3.0)

A subset of [andrerezemini/fm1-cartridge](https://github.com/andrerezemini/fm1-cartridge) at
**527889055b24d2b172cb0997dc91c809767e5cbb** (2026-10-30), the ESP32-S3 port that compiles
Felucca's DSP from source behind one interface, `synth/fm1_synth.h`:

- `synth/felucca_synth.{c,h}` and `synth/fm1_synth.h`: the adapter, unchanged;
- `third_party/felucca/`: Felucca 1.7.0's DSP sources and generated tables, as the cartridge vendors
  them (its `README.md` says what, from which commits, and its one changed upstream file). Left
  out: the generators (`tools/`, `*.py`) and the CC0 sample WAVs, whose sound is already in
  `generated/felucca_samples.h`; their attribution files stay in `assets/samples-cc0/`.
- `LICENSE`: the cartridge's GPL-3.0.

Felucca's licenses are in `third_party/felucca/LICENSING.md` and `LICENSES/`: GPL-3.0-only for
Felucca, MPL-2.0 for felucca-bsp, Apache-2.0 for MSFA (FM6), MIT for Felucca-Engines, Rings and
DaisySP.

How Outrigger builds it differs from the cartridge's ESP-IDF build, and only in
`../src/felucca_unit.c`, never in these files:
- one translation unit (`felucca_unit.c` includes the adapter), at `-O2` with `-fwrapv`;
- the cartridge's `felucca.lf` puts upstream's long lines in PSRAM through a linker fragment.
  Arduino's prebuilt ESP-IDF can't, so `felucca_unit.c` turns `dly_buf`, `cho_buf`, `rev_comb`
  and `sl_buf` into pointers to PSRAM by macro (`felucca_memory_begin`). `fl_buf` (1.4 KB) stays
  in internal RAM.
- the include path for `third_party/felucca/generated` comes from the Makefile (`SYNTH_C_FLAGS`).

## ../src/es8311/ (Apache-2.0)

Espressif's ES8311 codec driver, as Waveshare ships it: see `../src/es8311/README.md`.
