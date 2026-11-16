// vh.h: a thin host for VIBE FM SDK firmwares ("Vibes"), built with the VIBE FM SDK.
// One interface, several backends: a .wasm or .aot file run by WAMR, or the same C compiled natively.
// The same code builds on the Mac (firmware/vibe_p4/mac) and on the ESP32-P4 (firmware/vibe_p4/p4).
//
// What the SDK's .wasm needs from a host (sdk/src/vibe_sdk.c, tools/wasmtest.mjs):
//   imports  env.vibe_host_log(ptr, len), env.vibe_host_request(what), env.vibe_host_store(op, name, nlen, data, len) -> int
//   exports  vibe_wasm_init(sr), vibe_wasm_time(), vibe_wasm_event(type, id, down, value, midi),
//            vibe_wasm_io() (in | out_l | out_r | in2, 256 floats each), vibe_wasm_audio(frames, has_input)
// Drawing, storage, MIDI out, the transport and worker jobs are not used here (a bench host: sound only).
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vh_backend {
  const char *name;                                         // "native", "wamr-classic", "wamr-fast", "wamr-aot"
  void *ctx;
  int (*init)(struct vh_backend *b, float sample_rate);     // 0 = ok
  void (*event)(struct vh_backend *b, int type, int id, int down, float value, int midi, uint64_t frame);
  int (*audio)(struct vh_backend *b, int frames, const float **l, const float **r);   // 0 = ok
  void (*fini)(struct vh_backend *b);
  char err[128];
} vh_backend;

// ---- WAMR backend (vh_wamr.c) --------------------------------------------------------------------------------------
typedef void (*vh_log_fn)(const char *s, int len);
int vh_wamr_runtime_init(void *pool, uint32_t pool_size);    // pool 0 = the system allocator
void vh_wamr_runtime_fini(void);
const char *vh_wamr_mode(void);                              // "classic" or "fast" (how the interpreter was built), "+aot"
// Loads a .wasm or an .aot file (WAMR tells them apart by their magic). The buffer is copied (WAMR may write into it).
// max_pages caps the linear memory: the SDK links with --shared-memory --max-memory=64 MB, and WAMR reserves a
// shared memory's maximum up front, which the P4 (32 MB PSRAM) does not have. 0 = the module's own maximum.
int vh_wamr_open(vh_backend *b, const uint8_t *module, uint32_t size, uint32_t max_pages,
                 uint32_t stack_size, vh_log_fn log);
int vh_wamr_mem_pages(vh_backend *b, uint32_t *cur, uint32_t *max);   // after init: the linear memory's pages now / at most

// ---- native backends (one per firmware, in their own files) --------------------------------------------------------
void vh_native_vibe_synth(vh_backend *b);                    // examples/example-synth compiled natively
void vh_native_faust_synth(vh_backend *b);                   // the Faust patch's Vibe compiled natively
extern vh_log_fn vh_native_log;                              // where a native Vibe's vibe_log goes (0 = dropped)

// ---- the fixed scenario: the same on the Mac, in wasmtest.mjs and on the P4 ---------------------------------------
// n notes (1..8) as MIDI note on at t = 0, note off at t = 1.5 s, 2.5 s in all. Events go in before the first block
// whose start time is >= the event's time (wasmtest.mjs does the same).
typedef struct {
  int notes;            // 1..8
  float sample_rate;    // 48000
  int block;            // 128 (wasmtest's block)
  float seconds;        // 2.5
  float off_at;         // 1.5
} vh_scenario;

typedef struct {
  uint32_t out_hash;    // FNV-1a over the output's bits, L and R interleaved: wasmtest.mjs's out_hash
  float peak, rms;
  uint32_t nan;
  uint32_t blocks;
  // timing, from the clock the caller hands in (0 = untimed)
  double us_mean, us_max, us_mean_held;  // all blocks / blocks while the notes are held
  uint32_t us_max_block;
} vh_result;

typedef uint64_t (*vh_clock_ns)(void);
// Runs the scenario. out (optional) gets blocks*block frames of L and R each.
int vh_run(vh_backend *b, const vh_scenario *s, vh_clock_ns clock, vh_result *r, float *out_l, float *out_r);

// the note numbers the scenario plays
extern const int vh_scenario_notes[8];

#ifdef __cplusplus
}
#endif
