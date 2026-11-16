// vh_wamr.c: the VIBE host shim on WAMR (wasm-micro-runtime, Apache-2.0 WITH LLVM-exception).
// Runs a firmware's .wasm (classic or fast interpreter, whichever WAMR was built with) or its .aot (wamrc output).
#include "vh.h"
#include "wasm_export.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  wasm_module_t module;
  wasm_module_inst_t inst;
  wasm_exec_env_t env;
  uint8_t *buf;                       // our copy of the module (WAMR keeps pointers into it)
  wasm_function_inst_t f_init, f_time, f_event, f_io, f_audio;
  uint32_t io_off, time_off;          // app offsets, read once
  float *l, *r;                       // native pointers into the linear memory
  uint32_t *vtime;
  vh_log_fn log;
  uint32_t max_pages, stack_size;
  uint32_t size;
} wamr_ctx;

static wamr_ctx *g_ctx;               // the natives find their logger here (one instance at a time)

// ---- the three imports -------------------------------------------------------------------------------------------
static void n_log(wasm_exec_env_t env, const char *s, uint32_t len)
{
  (void)env;
  if (g_ctx && g_ctx->log) g_ctx->log(s, (int)len);
}
static void n_request(wasm_exec_env_t env, int32_t what) { (void)env; (void)what; }
static int32_t n_store(wasm_exec_env_t env, int32_t op, const char *name, uint32_t nlen, void *data, uint32_t len)
{
  (void)env; (void)op; (void)name; (void)nlen; (void)data; (void)len;
  return -1;                          // no storage on the bench: every file is "not there"
}
static NativeSymbol g_natives[] = {
  { "vibe_host_log", (void *)n_log, "(*~)", NULL },
  { "vibe_host_request", (void *)n_request, "(i)", NULL },
  { "vibe_host_store", (void *)n_store, "(i*~*~)i", NULL },
};

int vh_wamr_runtime_init(void *pool, uint32_t pool_size)
{
  RuntimeInitArgs a;
  memset(&a, 0, sizeof a);
  if (pool) {
    a.mem_alloc_type = Alloc_With_Pool;
    a.mem_alloc_option.pool.heap_buf = pool;
    a.mem_alloc_option.pool.heap_size = pool_size;
  } else {
    a.mem_alloc_type = Alloc_With_System_Allocator;
  }
  a.native_module_name = "env";
  a.native_symbols = g_natives;
  a.n_native_symbols = sizeof g_natives / sizeof g_natives[0];
  return wasm_runtime_full_init(&a) ? 0 : -1;
}

void vh_wamr_runtime_fini(void) { wasm_runtime_destroy(); }

const char *vh_wamr_mode(void)
{
#if WASM_ENABLE_FAST_INTERP != 0
#if WASM_ENABLE_AOT != 0
  return "fast+aot";
#else
  return "fast";
#endif
#else
#if WASM_ENABLE_AOT != 0
  return "classic+aot";
#else
  return "classic";
#endif
#endif
}

static int w_call(vh_backend *b, wasm_function_inst_t f, uint32_t argc, uint32_t *argv)
{
  wamr_ctx *c = b->ctx;
  if (!wasm_runtime_call_wasm(c->env, f, argc, argv)) {
    const char *e = wasm_runtime_get_exception(c->inst);
    snprintf(b->err, sizeof b->err, "trap: %s", e ? e : "?");
    return -1;
  }
  return 0;
}

static void w_drop_instance(wamr_ctx *c)
{
  if (c->env) wasm_runtime_destroy_exec_env(c->env);
  if (c->inst) wasm_runtime_deinstantiate(c->inst);
  c->env = 0; c->inst = 0;
}

static int w_init(vh_backend *b, float sr)
{
  wamr_ctx *c = b->ctx;
  char err[96];
  w_drop_instance(c);               // a fresh instance per run: the same start state as wasmtest.mjs
  InstantiationArgs ia;
  memset(&ia, 0, sizeof ia);
  ia.default_stack_size = c->stack_size;
  ia.host_managed_heap_size = 0;    // the SDK has no heap (no malloc)
  ia.max_memory_pages = c->max_pages;
  c->inst = wasm_runtime_instantiate_ex(c->module, &ia, err, sizeof err);
  if (!c->inst) { snprintf(b->err, sizeof b->err, "instantiate: %s", err); return -1; }
  c->env = wasm_runtime_create_exec_env(c->inst, c->stack_size);
  if (!c->env) { snprintf(b->err, sizeof b->err, "exec env"); return -1; }
  c->f_init = wasm_runtime_lookup_function(c->inst, "vibe_wasm_init");
  c->f_time = wasm_runtime_lookup_function(c->inst, "vibe_wasm_time");
  c->f_event = wasm_runtime_lookup_function(c->inst, "vibe_wasm_event");
  c->f_io = wasm_runtime_lookup_function(c->inst, "vibe_wasm_io");
  c->f_audio = wasm_runtime_lookup_function(c->inst, "vibe_wasm_audio");
  if (!c->f_init || !c->f_event || !c->f_io || !c->f_audio) { snprintf(b->err, sizeof b->err, "missing export"); return -1; }
  uint32_t argv[5];
  if (w_call(b, c->f_io, 0, argv)) return -1;
  c->io_off = argv[0];
  if (!wasm_runtime_validate_app_addr(c->inst, c->io_off, 4 * 256 * 4)) { snprintf(b->err, sizeof b->err, "io outside memory"); return -1; }
  float *io = wasm_runtime_addr_app_to_native(c->inst, c->io_off);
  c->l = io + 256; c->r = io + 512;
  c->vtime = 0;
  if (c->f_time) {
    if (w_call(b, c->f_time, 0, argv)) return -1;
    c->time_off = argv[0];
    if (wasm_runtime_validate_app_addr(c->inst, c->time_off, 32)) c->vtime = wasm_runtime_addr_app_to_native(c->inst, c->time_off);
  }
  memcpy(&argv[0], &sr, 4);
  g_ctx = c;
  return w_call(b, c->f_init, 1, argv);
}

static void w_event(vh_backend *b, int type, int id, int down, float value, int midi, uint64_t frame)
{
  wamr_ctx *c = b->ctx;
  if (c->vtime) {                   // the time table as wasmtest.mjs fills it: the frame, "heard" unknown
    c->vtime[0] = (uint32_t)frame; c->vtime[1] = (uint32_t)(frame >> 32);
    c->vtime[2] = 0; c->vtime[3] = 0; c->vtime[4] = 0;
  }
  uint32_t argv[5] = { (uint32_t)type, (uint32_t)id, (uint32_t)down, 0, (uint32_t)midi };
  memcpy(&argv[3], &value, 4);
  if (w_call(b, c->f_event, 5, argv) && c->log) c->log(b->err, (int)strlen(b->err));
}

static int w_audio(vh_backend *b, int frames, const float **l, const float **r)
{
  wamr_ctx *c = b->ctx;
  uint32_t argv[2] = { (uint32_t)frames, 0 };
  if (w_call(b, c->f_audio, 2, argv)) return -1;
  *l = c->l; *r = c->r;
  return 0;
}

static void w_fini(vh_backend *b)
{
  wamr_ctx *c = b->ctx;
  if (!c) return;
  w_drop_instance(c);
  if (c->module) wasm_runtime_unload(c->module);
  free(c->buf);
  if (g_ctx == c) g_ctx = 0;
  free(c);
  b->ctx = 0;
}

int vh_wamr_mem_pages(vh_backend *b, uint32_t *cur, uint32_t *max)
{
  wamr_ctx *c = b->ctx;
  if (!c || !c->inst) return -1;
  wasm_memory_inst_t m = wasm_runtime_get_default_memory(c->inst);
  if (!m) return -1;
  *cur = (uint32_t)wasm_memory_get_cur_page_count(m);
  *max = (uint32_t)wasm_memory_get_max_page_count(m);
  return 0;
}

int vh_wamr_open(vh_backend *b, const uint8_t *module, uint32_t size, uint32_t max_pages, uint32_t stack_size, vh_log_fn log)
{
  memset(b, 0, sizeof *b);
  wamr_ctx *c = calloc(1, sizeof *c);
  if (!c) return -1;
  c->buf = malloc(size);
  if (!c->buf) { free(c); return -1; }
  memcpy(c->buf, module, size);
  c->size = size; c->max_pages = max_pages; c->stack_size = stack_size ? stack_size : 32 * 1024; c->log = log;
  const int aot = size >= 4 && memcmp(module, "\0aot", 4) == 0;
  char err[96];
  c->module = wasm_runtime_load(c->buf, size, err, sizeof err);
  b->ctx = c;
  b->init = w_init; b->event = w_event; b->audio = w_audio; b->fini = w_fini;
  b->name = aot ? "wamr-aot" : (strncmp(vh_wamr_mode(), "fast", 4) == 0 ? "wamr-fast" : "wamr-classic");
  if (!c->module) { snprintf(b->err, sizeof b->err, "load: %s", err); return -1; }
  return 0;
}
