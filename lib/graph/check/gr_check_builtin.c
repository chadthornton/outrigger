// Everything lib/graph ships, as checker subjects: the routing nodes, the
// MIDI clock, both Schwung modules (sample-exact, and Schwung's own
// block-quantised model, declared block-dependent) and the hub apps HW2 runs.
#include "gr_check_builtin.h"

#include <string.h>

#include "gr_hub.h"
#include "gr_nodes.h"
#include "gr_schwung.h"

struct midi_fx_api_v1 *eucalypso_midi_fx_init(const struct host_api_v1 *host);
struct midi_fx_api_v1 *superarp_midi_fx_init(const struct host_api_v1 *host);

static int init_zero(void *self) {
  (void)self;
  return 1;
}
static int init_split(void *self) {
  gr_split_init((gr_split *)self, 60, GR_SPLIT_BOTH);
  return 1;
}
static void tw_split(void *self, uint32_t r) { ((gr_split *)self)->key = (uint8_t)(r % 128); }
static int init_chanmap(void *self) {
  gr_chanmap *n = (gr_chanmap *)self;
  gr_chanmap_init(n);
  for (int c = 0; c < 16; ++c) n->map[c] = (uint8_t)((c + 3) & 15);
  return 1;
}
static void tw_chanmap(void *self, uint32_t r) {
  gr_chanmap *n = (gr_chanmap *)self;
  n->map[r & 15] = (r >> 8) % 5 == 0 ? GR_DROP : (uint8_t)((r >> 12) & 15);
}
static int init_transpose(void *self) {
  gr_transpose_init((gr_transpose *)self, 7);
  return 1;
}
static void tw_transpose(void *self, uint32_t r) { ((gr_transpose *)self)->semitones = (int8_t)((int)(r % 49) - 24); }
static int init_ccremap(void *self) {
  gr_ccremap *n = (gr_ccremap *)self;
  gr_ccremap_init(n);
  n->map[1] = 74;
  n->map[7] = GR_DROP;
  return 1;
}
static void tw_ccremap(void *self, uint32_t r) {
  gr_ccremap *n = (gr_ccremap *)self;
  n->map[r & 127] = (uint8_t)((r >> 8) & 127);
  n->channel_mask = (uint16_t)(r >> 16);
}
static int init_clock(void *self) {
  gr_clockgen_init((gr_clockgen *)self, 133000);  // 133 bpm: a clock every 902.25 frames
  gr_clockgen_start((gr_clockgen *)self);
  return 1;
}
static void tw_clock(void *self, uint32_t r) { gr_clockgen_set_tempo((gr_clockgen *)self, 60000 + r % 120000); }

static int init_schwung(gr_schwung *n, gr_midi_fx_init_fn fn, uint32_t q, const char *sync) {
  gr_schwung_reset_transport();
  if (!gr_schwung_init(n, fn, "", q, 0)) return 0;
  gr_schwung_set(n, "sync", sync);
  return 1;
}
static void fini_schwung(void *self) { gr_schwung_destroy((gr_schwung *)self); }
static int init_euc1(void *s) { return init_schwung((gr_schwung *)s, (gr_midi_fx_init_fn)eucalypso_midi_fx_init, 1, "clock"); }
static int init_euc64(void *s) { return init_schwung((gr_schwung *)s, (gr_midi_fx_init_fn)eucalypso_midi_fx_init, 64, "clock"); }
static int init_arp1(void *s) { return init_schwung((gr_schwung *)s, (gr_midi_fx_init_fn)superarp_midi_fx_init, 1, "internal"); }
static int init_arp64(void *s) { return init_schwung((gr_schwung *)s, (gr_midi_fx_init_fn)superarp_midi_fx_init, 64, "internal"); }

// The hub apps, built by the same code the hub runs (gr_hub_app_build).
#define HUB_APP(fn, app)                                                         \
  static int fn(gr_graph *g, void *mem, const int *feed, int *on, int *op) {     \
    gr_hub_config c;                                                             \
    gr_hub_config_default(&c, app);                                              \
    return gr_hub_app_build(g, &c, (gr_hub_app_nodes *)mem, feed, on, op);       \
  }
HUB_APP(app_thru, GR_HUB_APP_THRU)
HUB_APP(app_merge, GR_HUB_APP_MERGE)
HUB_APP(app_split, GR_HUB_APP_SPLIT)
HUB_APP(app_layer, GR_HUB_APP_LAYER)
HUB_APP(app_clock, GR_HUB_APP_CLOCK)
static void tw_app(void *mem, uint32_t r) {  // the routing parameters a hub might change
  gr_hub_app_nodes *m = (gr_hub_app_nodes *)mem;
  m->split.key = (uint8_t)(r % 128);
  m->transpose.semitones = (int8_t)((int)((r >> 8) % 25) - 12);
  m->chanmap[1].map[(r >> 16) & 15] = (uint8_t)((r >> 20) & 15);
}

#define NODE(nm, d, T, in, tw, fl) {nm, fl, &d, sizeof(T), in, 0, 0, 0, 0, tw, 0}
#define NODE_F(nm, d, T, in, fi, fl) {nm, fl, &d, sizeof(T), in, fi, 0, 0, 0, 0, 0}
#define NODE_K(nm, d, T, in, fi, fl, kn) {nm, fl, &d, sizeof(T), in, fi, 0, 0, 0, 0, kn}
#define APP(nm, n, b, fl) {nm, fl, 0, 0, 0, 0, n, sizeof(gr_hub_app_nodes), b, tw_app, 0}

const gr_ck_subject gr_ck_builtin[] = {
    NODE("pass", gr_pass_desc, int, init_zero, 0, GR_CK_NOTES),
    NODE("merge2", gr_merge2_desc, gr_merge, init_zero, 0, GR_CK_NOTES),
    NODE("merge4", gr_merge4_desc, gr_merge, init_zero, 0, GR_CK_NOTES),
    NODE("split", gr_split_desc, gr_split, init_split, tw_split, GR_CK_NOTES),
    NODE("chanmap", gr_chanmap_desc, gr_chanmap, init_chanmap, tw_chanmap, GR_CK_NOTES),
    NODE("transpose", gr_transpose_desc, gr_transpose, init_transpose, tw_transpose, GR_CK_NOTES),
    NODE("ccremap", gr_ccremap_desc, gr_ccremap, init_ccremap, tw_ccremap, GR_CK_NOTES),
    NODE("impulse", gr_impulse_desc, gr_impulse, init_zero, 0, 0),
    NODE("clockgen", gr_clockgen_desc, gr_clockgen, init_clock, tw_clock, 0),
    NODE_F("schwung:eucalypso q=1", gr_schwung_desc, gr_schwung, init_euc1, fini_schwung,
           GR_CK_NOTES | GR_CK_CLOCKED),
    NODE_F("schwung:eucalypso q=64", gr_schwung_desc, gr_schwung, init_euc64, fini_schwung,
           GR_CK_NOTES | GR_CK_CLOCKED | GR_CK_BLOCK_DEPENDENT),
    NODE_F("schwung:superarp q=1", gr_schwung_desc, gr_schwung, init_arp1, fini_schwung, GR_CK_NOTES),
    NODE_F("schwung:superarp q=64", gr_schwung_desc, gr_schwung, init_arp64, fini_schwung,
           GR_CK_NOTES | GR_CK_BLOCK_DEPENDENT),
    APP("hub:thru", 1, app_thru, GR_CK_NOTES),
    APP("hub:merge", 2, app_merge, GR_CK_NOTES),
    APP("hub:split", 1, app_split, GR_CK_NOTES),
    APP("hub:layer", 1, app_layer, GR_CK_NOTES),
    APP("hub:clock", 1, app_clock, GR_CK_NOTES),
};
const int gr_ck_builtin_count = (int)(sizeof(gr_ck_builtin) / sizeof(gr_ck_builtin[0]));

// Not checked here: the edge nodes (source, sink, tsource, tsink) talk to
// rings, not to the graph alone; test_graph and test_graph_hub cover them.
const char *const gr_ck_not_checked = "source, sink, tsource, tsink (ring edges: see test_graph, test_graph_hub)";
