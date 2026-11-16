# Vendored for the Schwung shim (spike, night 0, C1-4)

All three are MIT (LICENSE in each folder). They build only for the host tests
(`make test-graph`); nothing here is in an Arduino build (Arduino compiles
`lib/graph/src/` only).

| Folder | Upstream | Commit | Files |
|---|---|---|---|
| `schwung/host/` | https://github.com/charlesvestal/schwung | 7175f155ad77 | `src/host/midi_fx_api_v1.h`, `src/host/plugin_api_v1.h`, unchanged |
| `eucalypso/` | https://github.com/handcraftedcc/move-everything-eucalypso | f0ff52557829 | `src/dsp/eucalypso.c` |
| `superarp/` | https://github.com/handcraftedcc/move-everything-superarp | 6eefd02af918 | `src/dsp/superarp.c`, unchanged |

## Patches

- `eucalypso.c` line 33: upstream has `#define EUCALYPSO_DEBUG_LOG 1`, which
  makes every `dlog()` (each note on, note off, clock start) try to `fopen`
  `/data/UserData/schwung/eucalypso.log` until it succeeds. On the Mac (and on
  an ESP32) that path never opens, so each event pays a failed `fopen` on the
  real-time path. Now `#ifndef ... #define EUCALYPSO_DEBUG_LOG 0`. Nothing else.

## Build notes

- Both modules export the same symbol, `move_midi_fx_init`. The Makefile
  renames it per module with `-Dmove_midi_fx_init=<name>_midi_fx_init`, so
  several link into one binary. Every other function in them is `static`.
- `-Dmalloc=gr_count_malloc` (and calloc, free) route their allocations
  through `tests/support/gr_alloc_count.c`, so the tests prove the process
  path never allocates (each module callocs once, in `create_instance`).
- `schwung/host/plugin_api_v1.h`: its `_Static_assert` that
  `host_api_v1_t::reserved` sits at +120 holds only for Move's 64-bit ABI. On
  a 32-bit ESP32 it fails, so it is now inside `#if UINTPTR_MAX > 0xFFFFFFFFu`
  (still checked on the Mac). The shim compiles modules from source, so there
  is no binary ABI to protect on the ESP32.
