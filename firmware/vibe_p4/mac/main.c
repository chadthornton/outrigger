// vibe_run: renders the fixed scenario with one backend on the Mac and prints one JSON line.
//   vibe_run native-synth|native-faust|wamr [module.wasm|.aot] [--notes N] [--block F] [--repeat R] [--out file.f32]
//            [--max-pages P]
// The output hash is wasmtest.mjs's out_hash, so a run can be checked against the SDK's own Node host.
#include "vh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void logf_(const char *s, int n) { fprintf(stderr, "vibe: %.*s\n", n, s); }

static uint8_t *slurp(const char *p, uint32_t *n)
{
  FILE *f = fopen(p, "rb"); if (!f) return 0;
  fseek(f, 0, SEEK_END); long z = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc((size_t)z); if (fread(b, 1, (size_t)z, f) != (size_t)z) { fclose(f); free(b); return 0; }
  fclose(f); *n = (uint32_t)z; return b;
}

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: vibe_run native-synth|native-faust|wamr [module] [--notes N] [--block F] [--repeat R] [--out f.f32] [--max-pages P]\n"); return 2; }
  const char *kind = argv[1], *mod = 0, *out = 0;
  int notes = 1, block = 128, repeat = 1, ai = 2; uint32_t max_pages = 0, stack = 32 * 1024;
  if (!strcmp(kind, "wamr")) { if (argc < 3) return 2; mod = argv[2]; ai = 3; }
  for (; ai < argc; ai++) {
    if (!strcmp(argv[ai], "--notes") && ai + 1 < argc) notes = atoi(argv[++ai]);
    else if (!strcmp(argv[ai], "--block") && ai + 1 < argc) block = atoi(argv[++ai]);
    else if (!strcmp(argv[ai], "--repeat") && ai + 1 < argc) repeat = atoi(argv[++ai]);
    else if (!strcmp(argv[ai], "--out") && ai + 1 < argc) out = argv[++ai];
    else if (!strcmp(argv[ai], "--max-pages") && ai + 1 < argc) max_pages = (uint32_t)atoi(argv[++ai]);
    else if (!strcmp(argv[ai], "--stack") && ai + 1 < argc) stack = (uint32_t)atoi(argv[++ai]);
    else { fprintf(stderr, "unknown argument %s\n", argv[ai]); return 2; }
  }
  vh_backend b;
  vh_native_log = logf_;
  if (!strcmp(kind, "native-synth")) vh_native_vibe_synth(&b);
  else if (!strcmp(kind, "native-faust")) vh_native_faust_synth(&b);
  else {
    if (vh_wamr_runtime_init(0, 0)) { fprintf(stderr, "wamr init failed\n"); return 1; }
    uint32_t n; uint8_t *m = slurp(mod, &n);
    if (!m) { fprintf(stderr, "can't read %s\n", mod); return 1; }
    if (vh_wamr_open(&b, m, n, max_pages, stack, logf_)) { fprintf(stderr, "%s\n", b.err); return 1; }
    free(m);
  }
  vh_scenario s = { notes, 48000.0f, block, 2.5f, 1.5f, 1000.0 };
  vh_result r, first;
  float *L = 0, *R = 0;
  size_t frames = (size_t)((2.5 * 48000 + block - 1) / block) * block;
  if (out) { L = malloc(frames * 4); R = malloc(frames * 4); }
  double best_mean = 1e30, best_held = 1e30, worst_max = 0;
  for (int k = 0; k < repeat; k++) {
    if (vh_run(&b, &s, now_ns, &r, k == 0 ? L : 0, k == 0 ? R : 0)) { fprintf(stderr, "run failed: %s\n", b.err); return 1; }
    if (k == 0) first = r;
    else if (r.out_hash != first.out_hash) fprintf(stderr, "warning: run %d hash %08x != %08x\n", k, r.out_hash, first.out_hash);
    if (r.us_mean < best_mean) best_mean = r.us_mean;
    if (r.us_mean_held < best_held) best_held = r.us_mean_held;
    if (r.us_max > worst_max) worst_max = r.us_max;
  }
  uint32_t pc = 0, pm = 0;
  if (!strcmp(kind, "wamr") && !vh_wamr_mem_pages(&b, &pc, &pm)) fprintf(stderr, "linear memory: %u pages now, %u at most\n", pc, pm);
  if (out) {   // interleaved stereo float32, raw
    FILE *f = fopen(out, "wb");
    for (size_t i = 0; i < frames; i++) { fwrite(&L[i], 4, 1, f); fwrite(&R[i], 4, 1, f); }
    fclose(f);
  }
  printf("{\"backend\":\"%s\",\"mode\":\"%s\",\"notes\":%d,\"block\":%d,\"out_hash\":\"%08x\",\"peak\":%.3f,\"rms\":%.4f,\"nan\":%u,"
         "\"blocks\":%u,\"repeat\":%d,\"us_mean_best\":%.3f,\"us_held_best\":%.3f,\"us_max\":%.3f}\n",
         b.name, strcmp(kind, "wamr") ? "-" : vh_wamr_mode(), notes, block, first.out_hash, first.peak, first.rms, first.nan,
         first.blocks, repeat, best_mean, best_held, worst_max);
  b.fini(&b);
  return 0;
}
