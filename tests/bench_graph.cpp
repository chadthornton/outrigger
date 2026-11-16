// Per-node timing for lib/graph on the host: `make graph-bench`.
// Each node runs in its own graph (feed -> node) for kBlocks blocks of 64
// frames at 48 kHz, fed `load` events per block; the graph's own per-node
// timer gives each block's time. Prints p50 / p99 / max / mean in ns.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

extern "C" {
#include "gr_graph.h"
#include "gr_nodes.h"
#include "gr_schwung.h"
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"
midi_fx_api_v1_t *eucalypso_midi_fx_init(const host_api_v1_t *host);
midi_fx_api_v1_t *superarp_midi_fx_init(const host_api_v1_t *host);
}

static const int kBlocks = 200000;  // 266 s of audio at 64 / 48 kHz
static gr_graph g;

struct Feed {
  int load;
  uint32_t n;
  bool clocked;  // MIDI clock (every 1,000 frames) plus a held chord, for the sequencers
};

static void feed_process(void *self, const gr_block *b, gr_io *io) {
  Feed *f = (Feed *)self;
  gr_events *o = io->ev_out[0];
  if (f->clocked) {
    if (b->index == 0) {
      uint32_t w[4] = {gr_ump_system(0, 0xFA, 0, 0), 0, 0, 0};
      gr_events_push(o, 0, w);
      for (int k = 0; k < 3; ++k) {
        w[0] = gr_ump_m1(0, 0x90, (uint8_t)(60 + 4 * k - (k == 2)), 100);
        gr_events_push(o, 0, w);
      }
    }
    uint64_t start = b->frame_pos, end = start + b->frames;
    uint64_t c = (start + 999) / 1000 * 1000;
    for (; c < end; c += 1000) {
      uint32_t w[4] = {gr_ump_system(0, 0xF8, 0, 0), 0, 0, 0};
      gr_events_push(o, (uint32_t)(c - start), w);
    }
    return;
  }
  for (int i = 0; i < f->load; ++i) {
    uint32_t frame = (uint32_t)(i * b->frames / (f->load ? f->load : 1));
    uint32_t note = 36 + (f->n++ % 48);
    uint32_t w[4] = {0, 0, 0, 0};
    switch (i % 4) {
      case 0: gr_ump_m2_note(0, GR_OP_NOTE_ON, i % 16, note, 0x8000, w); break;
      case 1: gr_ump_m2_note(0, GR_OP_NOTE_OFF, i % 16, note, 0, w); break;
      case 2: gr_ump_m2_cc(0, i % 16, 1 + (i % 8), 0x40000000u, w); break;
      default: w[0] = gr_ump_m1(0, 0x90 | (i % 16), (uint8_t)note, 64); break;
    }
    gr_events_push(io->ev_out[0], frame, w);
  }
}
static const gr_node_desc feed_desc = {"feed", 0, 1, {0}, {GR_PORT_EVENTS}, feed_process};

struct Result {
  const char *name;
  int load;
  double p50, p99, max, mean;
};

static Result measure(const char *name, const gr_node_desc *d, void *self, int load, bool clocked,
                      int n_in = 1) {
  static Feed feeds[4];
  gr_graph_init(&g, 48000, 64);
  int node = gr_graph_add(&g, d, self);
  for (int p = 0; p < n_in; ++p) {
    feeds[p] = {load / n_in, 0, clocked};
    int f = gr_graph_add(&g, &feed_desc, &feeds[p]);
    gr_graph_connect(&g, f, 0, node, p);
  }
  if (gr_graph_compile(&g) != GR_OK) printf("compile failed for %s\n", name);
  static std::vector<uint32_t> t;
  t.assign(kBlocks, 0);
  for (int i = 0; i < 1000; ++i) gr_graph_run(&g);  // warm up
  gr_graph_reset_timing(&g);
  for (int i = 0; i < kBlocks; ++i) {
    gr_graph_run(&g);
    t[i] = gr_graph_timing(&g, node)->last;
  }
  const gr_timing *tm = gr_graph_timing(&g, node);
  double mean = (double)tm->total / (double)tm->blocks;
  std::sort(t.begin(), t.end());
  return {name, load, (double)t[kBlocks / 2], (double)t[(size_t)(kBlocks * 0.99)], (double)t.back(), mean};
}

int main() {
  std::vector<Result> r;
  static gr_merge mg;
  static gr_split sp;
  static gr_chanmap cm;
  static gr_transpose tr;
  static gr_ccremap cc;
  static gr_impulse im;
  for (int load : {0, 4, 16, 60}) {
    gr_split_init(&sp, 60, GR_SPLIT_BOTH);
    gr_chanmap_init(&cm);
    cm.map[0] = 5;
    gr_transpose_init(&tr, 7);
    gr_ccremap_init(&cc);
    cc.map[1] = 74;
    r.push_back(measure("pass (overhead)", &gr_pass_desc, nullptr, load, false));
    r.push_back(measure("merge2", &gr_merge2_desc, &mg, load, false, 2));
    r.push_back(measure("split", &gr_split_desc, &sp, load, false));
    r.push_back(measure("chanmap", &gr_chanmap_desc, &cm, load, false));
    r.push_back(measure("transpose", &gr_transpose_desc, &tr, load, false));
    r.push_back(measure("ccremap", &gr_ccremap_desc, &cc, load, false));
    r.push_back(measure("impulse (audio out)", &gr_impulse_desc, &im, load, false));
  }
  static gr_schwung fx;
  struct Cfg {
    const char *name;
    gr_midi_fx_init_fn init;
    uint32_t quantum;
  } cfgs[] = {
      {"schwung:eucalypso q=64", (gr_midi_fx_init_fn)eucalypso_midi_fx_init, 64},
      {"schwung:eucalypso q=1", (gr_midi_fx_init_fn)eucalypso_midi_fx_init, 1},
      {"schwung:superarp q=64", (gr_midi_fx_init_fn)superarp_midi_fx_init, 64},
      {"schwung:superarp q=1", (gr_midi_fx_init_fn)superarp_midi_fx_init, 1},
  };
  for (auto &c : cfgs) {
    gr_schwung_init(&fx, c.init, "", c.quantum, 0);
    gr_schwung_set(&fx, "sync", "clock");
    r.push_back(measure(c.name, &gr_schwung_desc, &fx, -1, true));
    gr_schwung_destroy(&fx);
  }
  // The timer's own step: the smallest non-zero difference between two reads.
  uint32_t step = 0xFFFFFFFFu;
  for (int i = 0; i < 100000; ++i) {
    uint32_t a = gr_clock_ticks(), b;
    while ((b = gr_clock_ticks()) == a) {
    }
    if (b - a < step) step = b - a;
  }
  // A whole hub graph (2 feeds -> merge -> ccremap -> split -> 2 x (chanmap / transpose
  // + chanmap) -> merge), 16 events per 64 frames, timed around gr_graph_run_frames,
  // at the host block sizes a firmware sees (48 = 1 ms of USB audio).
  for (uint32_t F : {48u, 64u, 128u, 256u}) {
    static Feed fa, fb;
    static gr_merge m1, m2;
    static gr_split s1;
    static gr_chanmap lo, hi;
    static gr_transpose t1;
    static gr_ccremap c1;
    fa = {(int)(8 * F / 64), 0, false};
    fb = {(int)(8 * F / 64), 7, false};
    gr_split_init(&s1, 60, GR_SPLIT_LOWER);
    gr_chanmap_init(&lo);
    gr_chanmap_init(&hi);
    lo.map[0] = 1;
    hi.map[0] = 2;
    gr_transpose_init(&t1, 12);
    gr_ccremap_init(&c1);
    c1.map[1] = 74;
    gr_graph_init(&g, 48000, 64);
    int a = gr_graph_add(&g, &feed_desc, &fa), b = gr_graph_add(&g, &feed_desc, &fb);
    int m = gr_graph_add(&g, &gr_merge2_desc, &m1), c = gr_graph_add(&g, &gr_ccremap_desc, &c1);
    int sp2 = gr_graph_add(&g, &gr_split_desc, &s1), l = gr_graph_add(&g, &gr_chanmap_desc, &lo);
    int t = gr_graph_add(&g, &gr_transpose_desc, &t1), h = gr_graph_add(&g, &gr_chanmap_desc, &hi);
    int o = gr_graph_add(&g, &gr_merge2_desc, &m2);
    gr_graph_connect(&g, a, 0, m, 0);
    gr_graph_connect(&g, b, 0, m, 1);
    gr_graph_connect(&g, m, 0, c, 0);
    gr_graph_connect(&g, c, 0, sp2, 0);
    gr_graph_connect(&g, sp2, 0, l, 0);
    gr_graph_connect(&g, sp2, 1, t, 0);
    gr_graph_connect(&g, t, 0, h, 0);
    gr_graph_connect(&g, l, 0, o, 0);
    gr_graph_connect(&g, h, 0, o, 1);
    gr_graph_compile(&g);
    std::vector<uint32_t> tt(kBlocks);
    for (int i = 0; i < kBlocks; ++i) {
      uint32_t t0 = gr_clock_ticks();
      gr_graph_run_frames(&g, F);
      tt[i] = gr_clock_ticks() - t0;
    }
    double sum = 0;
    for (auto v : tt) sum += v;
    std::sort(tt.begin(), tt.end());
    const double budget = F * 1e9 / 48000.0;
    printf("hub graph (9 nodes, %u frames, %u ev/blk): p50 %u ns, p99 %u ns, max %u ns, mean %.1f ns per block (%.3f%% of %.0f ns)\n",
           F, 16 * F / 64, tt[kBlocks / 2], tt[(size_t)(kBlocks * 0.99)], tt.back(), sum / kBlocks,
           100.0 * sum / kBlocks / budget, budget);
  }
  printf("timer step: %u ns (per-node p50/p99 are multiples of it; means average over every block)\n", step);
  printf("lib/graph per-node time per block, host (%d blocks of 64 frames @ 48 kHz; budget 1,333,333 ns)\n",
         kBlocks);
  printf("%-24s %6s %9s %9s %9s %9s\n", "node", "ev/blk", "p50 ns", "p99 ns", "max ns", "mean ns");
  for (auto &x : r)
    printf("%-24s %6s %9.0f %9.0f %9.0f %9.1f\n", x.name,
           x.load < 0 ? "clock" : std::to_string(x.load).c_str(), x.p50, x.p99, x.max, x.mean);
  return 0;
}
