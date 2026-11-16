#include "gr_schwung.h"

#include <string.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

static int g_clock_status = MOVE_CLOCK_STATUS_STOPPED;
static float g_bpm = 120.0f;

static int host_clock_status(void) { return g_clock_status; }
static float host_bpm(void) { return g_bpm; }
static void host_log(const char *msg) { (void)msg; }

static host_api_v1_t g_host;
static int g_host_ready;

static const host_api_v1_t *shim_host(void) {
  if (!g_host_ready) {
    memset(&g_host, 0, sizeof(g_host));
    g_host.api_version = MOVE_PLUGIN_API_VERSION;
    g_host.sample_rate = GR_DEFAULT_SAMPLE_RATE;
    // The largest block the graph runs. Blocks vary (the host's size), and
    // neither eucalypso nor superarp reads this: they take tick(frames, sr).
    g_host.frames_per_block = GR_MAX_FRAMES;
    g_host.log = host_log;
    g_host.get_clock_status = host_clock_status;
    g_host.get_bpm = host_bpm;
    g_host_ready = 1;
  }
  return &g_host;
}

void gr_schwung_set_bpm(float bpm) { g_bpm = bpm; }

int gr_schwung_init(gr_schwung *n, gr_midi_fx_init_fn init, const char *module_dir,
                    uint32_t tick_quantum, int out_proto2) {
  memset(n, 0, sizeof(*n));
  if (!init) return 0;
  n->api = init(shim_host());
  if (!n->api || n->api->api_version != MIDI_FX_API_VERSION || !n->api->create_instance) return 0;
  n->inst = n->api->create_instance(module_dir ? module_dir : "", 0);
  n->tick_quantum = tick_quantum ? tick_quantum : GR_MAX_FRAMES;
  n->out_proto2 = (uint8_t)(out_proto2 != 0);
  return n->inst != 0;
}

void gr_schwung_destroy(gr_schwung *n) {
  if (n->api && n->inst && n->api->destroy_instance) n->api->destroy_instance(n->inst);
  n->inst = 0;
}

void gr_schwung_set(gr_schwung *n, const char *key, const char *val) {
  if (n->api && n->inst && n->api->set_param) n->api->set_param(n->inst, key, val);
}

int gr_schwung_get(gr_schwung *n, const char *key, char *buf, int len) {
  if (!n->api || !n->inst || !n->api->get_param) return -1;
  return n->api->get_param(n->inst, key, buf, len);
}

static void emit(gr_schwung *n, gr_events *out, uint32_t frame, uint8_t msgs[][3], const int *lens,
                 int count) {
  for (int i = 0; i < count; ++i) {
    uint32_t w[4] = {0, 0, 0, 0};
    if (gr_ump_from_midi1(msgs[i], lens[i], n->group, n->out_proto2, w) > 0)
      gr_events_push(out, frame, w);
  }
}

static void run_ticks(gr_schwung *n, gr_events *out, uint32_t from, uint32_t to, uint32_t sr) {
  if (!n->api->tick) return;
  uint8_t msgs[MIDI_FX_MAX_OUT_MSGS][3];
  int lens[MIDI_FX_MAX_OUT_MSGS];
  while (from < to) {
    uint32_t span = to - from;
    if (span > n->tick_quantum) span = n->tick_quantum;
    int c = n->api->tick(n->inst, (int)span, (int)sr, msgs, lens, MIDI_FX_MAX_OUT_MSGS);
    if (c >= MIDI_FX_MAX_OUT_MSGS) n->truncated++;
    if (c > MIDI_FX_MAX_OUT_MSGS) c = MIDI_FX_MAX_OUT_MSGS;  // believe the buffer, as Schwung does
    emit(n, out, from, msgs, lens, c);
    from += span;
  }
}

static void schwung_process(void *self, const gr_block *b, gr_io *io) {
  gr_schwung *n = (gr_schwung *)self;
  const gr_events *in = io->ev_in[0];
  gr_events *out = io->ev_out[0];
  uint32_t pos = 0;
  if (!n->inst) return;
  for (int i = 0; i < in->count; ++i) {
    const gr_event *e = &in->ev[i];
    uint32_t f = e->frame < b->frames ? e->frame : b->frames - 1;
    run_ticks(n, out, pos, f, b->sample_rate);
    if (f > pos) pos = f;
    uint8_t m1[3];
    int len = gr_ump_to_midi1(e->ump, m1);
    if (len <= 0) {
      gr_events_push(out, f, e->ump);
      n->passed++;
      continue;
    }
    if (m1[0] == 0xFA || m1[0] == 0xFB) g_clock_status = MOVE_CLOCK_STATUS_RUNNING;
    if (m1[0] == 0xFC) g_clock_status = MOVE_CLOCK_STATUS_STOPPED;
    uint8_t msgs[MIDI_FX_MAX_OUT_MSGS][3];
    int lens[MIDI_FX_MAX_OUT_MSGS];
    int c = n->api->process_midi(n->inst, m1, len, msgs, lens, MIDI_FX_MAX_OUT_MSGS);
    if (c >= MIDI_FX_MAX_OUT_MSGS) n->truncated++;
    if (c > MIDI_FX_MAX_OUT_MSGS) c = MIDI_FX_MAX_OUT_MSGS;
    if (c > 0) emit(n, out, f, msgs, lens, c);
  }
  run_ticks(n, out, pos, b->frames, b->sample_rate);
}

const gr_node_desc gr_schwung_desc = {"schwung", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS},
                                      schwung_process};
