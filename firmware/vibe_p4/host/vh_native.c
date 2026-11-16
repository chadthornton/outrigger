// vh_native.c: a Vibe's C, compiled natively and driven through the same calls the .wasm exports.
// Built once per firmware, with -include <prefix>.h (tools/gen_prefix.sh) so two firmwares, each with its own copy of
// the SDK, link into one image, and -DVH_NATIVE_ENTRY=<the backend's constructor name>.
#include "vh.h"
#include <string.h>

#ifndef VH_NATIVE_ENTRY
#error "-DVH_NATIVE_ENTRY=vh_native_<firmware> required"
#endif
#ifndef VH_NATIVE_NAME
#define VH_NATIVE_NAME "native"
#endif

// the SDK's host glue (sdk/src/vibe_sdk.c, section 4), native: plain functions (renamed by the prefix header)
void vibe_wasm_init(float sr);
void *vibe_wasm_time(void);
void vibe_wasm_event(int type, int id, int down, float value, int midi);
float *vibe_wasm_io(void);
void vibe_wasm_audio(int frames, int has_input);

static int n_init(vh_backend *b, float sr) { (void)b; vibe_wasm_init(sr); return 0; }
static void n_event(vh_backend *b, int type, int id, int down, float value, int midi, uint64_t frame)
{
  (void)b;
  uint32_t *vt = vibe_wasm_time();
  vt[0] = (uint32_t)frame; vt[1] = (uint32_t)(frame >> 32); vt[2] = 0; vt[3] = 0; vt[4] = 0;
  vibe_wasm_event(type, id, down, value, midi);
}
static int n_audio(vh_backend *b, int frames, const float **l, const float **r)
{
  (void)b;
  vibe_wasm_audio(frames, 0);
  float *io = vibe_wasm_io();
  *l = io + 256; *r = io + 512;
  return 0;
}
static void n_fini(vh_backend *b) { (void)b; }

void VH_NATIVE_ENTRY(vh_backend *b)
{
  memset(b, 0, sizeof *b);
  b->name = VH_NATIVE_NAME;
  b->init = n_init; b->event = n_event; b->audio = n_audio; b->fini = n_fini;
}
