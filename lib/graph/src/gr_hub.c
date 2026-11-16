#include "gr_hub.h"

#include <string.h>

// ---- the clock: microseconds <-> absolute frames (core 1 only) ----

// Absolute frame for microsecond time t (wrap-safe within +-35 minutes of the anchor).
static uint64_t clk_frame_of(const gr_hub_clock *c, uint32_t t_us) {
  int32_t d = (int32_t)(t_us - c->anchor_us);
  int64_t f = (int64_t)d * (int64_t)c->sample_rate;
  // floor division, so a time just before the anchor maps to the frame before it
  int64_t q = f >= 0 ? f / 1000000 : -((-f + 999999) / 1000000);
  int64_t abs = (int64_t)c->anchor_frame + q;
  return abs < 0 ? 0 : (uint64_t)abs;
}

// Microsecond time of an absolute frame (rounded up, so "due" is never early).
static uint32_t clk_us_of(const gr_hub_clock *c, uint64_t frame) {
  int64_t df = (int64_t)(frame - c->anchor_frame);
  int64_t us = df >= 0 ? (df * 1000000 + c->sample_rate - 1) / c->sample_rate
                       : -((-df * 1000000) / c->sample_rate);
  return c->anchor_us + (uint32_t)us;
}

// Keeps the anchor within a second of the block clock, exactly (1 s = sample_rate frames).
static void clk_advance(gr_hub_clock *c, uint64_t frame_pos) {
  while (frame_pos - c->anchor_frame >= 2ull * c->sample_rate) {
    c->anchor_frame += c->sample_rate;
    c->anchor_us += 1000000u;
  }
}

// ---- edge nodes ----

static void tsource_process(void *self, const gr_block *b, gr_io *io) {
  gr_tsource *n = (gr_tsource *)self;
  const uint64_t end = b->frame_pos + b->frames;
  const gr_tevent *e;
  while ((e = (const gr_tevent *)gr_spsc_peek(n->q)) != 0) {
    uint64_t at = clk_frame_of(n->clk, e->t_us) + n->clk->latency_frames;
    if (at >= end) break;
    uint32_t frame = 0;
    if (at >= b->frame_pos)
      frame = (uint32_t)(at - b->frame_pos);
    else
      n->late++;
    if (!gr_events_push(io->ev_out[0], frame, e->ump)) break;  // full: next block
    gr_spsc_pop(n->q, 0);
    n->taken++;
  }
}
const gr_node_desc gr_tsource_desc = {"tsource", 0, 1, {0}, {GR_PORT_EVENTS}, tsource_process};

static void tsink_process(void *self, const gr_block *b, gr_io *io) {
  gr_tsink *n = (gr_tsink *)self;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) {
    gr_tevent t;
    t.t_us = clk_us_of(n->clk, b->frame_pos + in->ev[i].frame) + n->clk->out_delay_us;
    memcpy(t.ump, in->ev[i].ump, sizeof(t.ump));
    if (gr_spsc_push(n->q, &t)) n->sent++;
  }
}
const gr_node_desc gr_tsink_desc = {"tsink", 1, 0, {GR_PORT_EVENTS}, {0}, tsink_process};

// ---- the hub ----

void gr_hub_config_default(gr_hub_config *c, gr_hub_app app) {
  memset(c, 0, sizeof(*c));
  c->app = app;
  c->sample_rate = 48000;
  c->default_frames = 48;
  c->split_key = 60;
  c->ch_lower = 0;
  c->ch_upper = 1;
  c->transpose = 12;
  c->bpm = 120.0f;
}

#define TRY(x)             \
  do {                     \
    int r_ = (x);          \
    if (r_ < 0) return r_; \
  } while (0)

int gr_hub_app_inputs(gr_hub_app app) { return app == GR_HUB_APP_MERGE ? 2 : 1; }

int gr_hub_app_build(gr_graph *g, const gr_hub_config *c, gr_hub_app_nodes *m, const int *in,
                     int *out_node, int *out_port) {
  switch (c->app) {
    case GR_HUB_APP_THRU:
      out_node[0] = in[0], out_port[0] = 0;
      return 1;
    case GR_HUB_APP_MERGE: {
      int mg = gr_graph_add(g, &gr_merge2_desc, &m->merge[0]);
      TRY(mg);
      TRY(gr_graph_connect(g, in[0], 0, mg, 0));
      TRY(gr_graph_connect(g, in[1], 0, mg, 1));
      out_node[0] = mg, out_port[0] = 0;
      return 1;
    }
    case GR_HUB_APP_SPLIT: {
      gr_split_init(&m->split, c->split_key, GR_SPLIT_BOTH);
      gr_chanmap_init(&m->chanmap[0]);
      gr_chanmap_init(&m->chanmap[1]);
      for (int ch = 0; ch < 16; ++ch) {
        m->chanmap[0].map[ch] = c->ch_lower & 15;
        m->chanmap[1].map[ch] = c->ch_upper & 15;
      }
      int s = gr_graph_add(g, &gr_split_desc, &m->split);
      int lo = gr_graph_add(g, &gr_chanmap_desc, &m->chanmap[0]);
      int hi = gr_graph_add(g, &gr_chanmap_desc, &m->chanmap[1]);
      TRY(s); TRY(lo); TRY(hi);
      TRY(gr_graph_connect(g, in[0], 0, s, 0));
      TRY(gr_graph_connect(g, s, 0, lo, 0));
      TRY(gr_graph_connect(g, s, 1, hi, 0));
      out_node[0] = lo, out_port[0] = 0;
      out_node[1] = hi, out_port[1] = 0;
      return 2;
    }
    case GR_HUB_APP_LAYER: {
      gr_transpose_init(&m->transpose, c->transpose);
      gr_chanmap_init(&m->chanmap[1]);
      for (int ch = 0; ch < 16; ++ch) m->chanmap[1].map[ch] = c->ch_upper & 15;
      int t = gr_graph_add(g, &gr_transpose_desc, &m->transpose);
      int hi = gr_graph_add(g, &gr_chanmap_desc, &m->chanmap[1]);
      TRY(t); TRY(hi);
      TRY(gr_graph_connect(g, in[0], 0, t, 0));
      TRY(gr_graph_connect(g, t, 0, hi, 0));
      out_node[0] = in[0], out_port[0] = 0;  // fan-out is free
      out_node[1] = hi, out_port[1] = 0;
      return 2;
    }
    case GR_HUB_APP_CLOCK: {
      gr_clockgen_init(&m->clockgen, (uint32_t)(c->bpm * 1000.0f + 0.5f));
      gr_clockgen_start(&m->clockgen);
      int k = gr_graph_add(g, &gr_clockgen_desc, &m->clockgen);
      int mg = gr_graph_add(g, &gr_merge2_desc, &m->merge[0]);
      TRY(k); TRY(mg);
      TRY(gr_graph_connect(g, in[0], 0, mg, 0));
      TRY(gr_graph_connect(g, k, 0, mg, 1));
      out_node[0] = mg, out_port[0] = 0;
      out_node[1] = k, out_port[1] = 0;
      return 2;
    }
    default:
      return GR_ERR_ARG;
  }
}

static int build(gr_hub *h) {
  int n_in = gr_hub_app_inputs(h->cfg.app), in[GR_HUB_ENDPOINTS], on[GR_HUB_ENDPOINTS], op[GR_HUB_ENDPOINTS];
  for (int i = 0; i < n_in; ++i) {
    gr_spsc_init(&h->in_q[i], h->in_buf[i], GR_HUB_RING, sizeof(gr_tevent));
    h->src[i].q = &h->in_q[i];
    h->src[i].clk = &h->clk;
    TRY(in[i] = h->src_id[i] = gr_graph_add(&h->g, &gr_tsource_desc, &h->src[i]));
    h->n_in++;
  }
  int n_out = gr_hub_app_build(&h->g, &h->cfg, &h->app, in, on, op);
  TRY(n_out);
  for (int i = 0; i < n_out; ++i) {
    gr_spsc_init(&h->out_q[i], h->out_buf[i], GR_HUB_RING, sizeof(gr_tevent));
    h->snk[i].q = &h->out_q[i];
    h->snk[i].clk = &h->clk;
    TRY(h->snk_id[i] = gr_graph_add(&h->g, &gr_tsink_desc, &h->snk[i]));
    TRY(gr_graph_connect(&h->g, on[i], op[i], h->snk_id[i], 0));
    h->n_out++;
  }
  return GR_OK;
}

int gr_hub_init(gr_hub *h, const gr_hub_config *cfg) {
  if (!h || !cfg || cfg->sample_rate == 0) return GR_ERR_ARG;
  memset(h, 0, sizeof(*h));
  h->cfg = *cfg;
  if (!h->cfg.default_frames) h->cfg.default_frames = 48;
  int r = gr_graph_init(&h->g, h->cfg.sample_rate, h->cfg.default_frames);
  if (r != GR_OK) return r;
  if (h->cfg.cpu_mhz) h->g.ticks_per_us = h->cfg.cpu_mhz;
  h->clk.sample_rate = h->cfg.sample_rate;
  h->clk.latency_frames = h->cfg.latency_frames ? h->cfg.latency_frames : h->cfg.default_frames;
  h->clk.out_delay_us = h->cfg.out_delay_us
                            ? h->cfg.out_delay_us
                            : (uint32_t)(2ull * h->cfg.default_frames * 1000000u / h->cfg.sample_rate);
  r = build(h);
  if (r < 0) return r;
  r = gr_graph_compile(&h->g);
  if (r != GR_OK) return r;
  gr_spsc_init(&h->stats_q, h->stats_buf, 4, sizeof(gr_hub_stats));
  h->stats_every = h->cfg.sample_rate / 10;  // frames between snapshots: ~100 ms
  return GR_OK;
}

int gr_hub_in_ump(gr_hub *h, int in, uint32_t t_us, const uint32_t *ump) {
  if (in < 0 || in >= h->n_in) return 0;
  gr_tevent e;
  e.t_us = t_us;
  int n = gr_ump_words(ump[0]);
  for (int k = 0; k < 4; ++k) e.ump[k] = k < n ? ump[k] : 0;
  return gr_spsc_push(&h->in_q[in], &e);
}

int gr_hub_in_midi1(gr_hub *h, int in, uint32_t t_us, const uint8_t *msg, int len) {
  uint32_t w[4] = {0, 0, 0, 0};
  if (gr_ump_from_midi1(msg, len, 0, 0, w) <= 0) return 0;  // SysEx etc.: not tonight
  return gr_hub_in_ump(h, in, t_us, w);
}

uint32_t gr_hub_frames_due(gr_hub *h, uint32_t now_us) {
  gr_hub_clock *c = &h->clk;
  if (!c->started) return h->cfg.default_frames;
  // The anchor maps microseconds to frames exactly, so there is no remainder to keep.
  int64_t due = (int64_t)clk_frame_of(c, now_us) - (int64_t)h->g.block.frame_pos;
  if (due <= 0) return 0;
  return due > GR_MAX_FRAMES ? GR_MAX_FRAMES : (uint32_t)due;  // more: call again to catch up
}

static void publish(gr_hub *h) {
  gr_hub_stats s;
  memset(&s, 0, sizeof(s));
  s.blocks = (uint32_t)h->g.block.index;
  s.frames = (uint32_t)h->g.block.frame_pos;
  s.ticks_per_us = h->g.ticks_per_us;
  s.block_max_ticks = h->block_max_ticks;
  s.ev_dropped = h->g.ev_dropped_total;
  s.resyncs = h->clk.resyncs;
  for (int i = 0; i < h->n_in; ++i) {
    s.in_dropped += gr_spsc_dropped(&h->in_q[i]);  // core 0 writes it: read it atomically
    s.late += h->src[i].late;
    s.in_events += h->src[i].taken;
  }
  for (int i = 0; i < h->n_out; ++i) {
    s.out_dropped += gr_spsc_dropped(&h->out_q[i]);
    s.out_events += h->snk[i].sent;
  }
  s.n_nodes = h->g.n_nodes < GR_HUB_STATS_NODES ? h->g.n_nodes : GR_HUB_STATS_NODES;
  for (int n = 0; n < s.n_nodes; ++n) {
    const gr_timing *t = &h->g.slot[n].t;
    s.node[n].name = h->g.slot[n].desc->name;
    s.node[n].last_ticks = t->last;
    s.node[n].max_ticks = t->max;
    s.node[n].mean_ticks = t->blocks ? (uint32_t)(t->total / t->blocks) : 0;
  }
  gr_spsc_push(&h->stats_q, &s);  // full: core 0 is not reading; skip this one
}

int gr_hub_run(gr_hub *h, uint32_t frames, uint32_t now_us) {
  if (frames == 0) return GR_OK;
  gr_hub_clock *c = &h->clk;
  if (!c->started) {
    c->anchor_us = now_us;
    c->anchor_frame = h->g.block.frame_pos;
    c->started = 1;
  } else {
    // More than 100 ms between the block clock and the microsecond clock (a
    // stall, a clock jump, or a caller whose own block clock runs at another
    // rate): re-anchor. Inputs already queued then count as late.
    int64_t drift = (int64_t)clk_frame_of(c, now_us) - (int64_t)(h->g.block.frame_pos + frames);
    int64_t limit = c->sample_rate / 10;
    if (drift > limit || drift < -limit) {
      c->anchor_us = now_us;
      c->anchor_frame = h->g.block.frame_pos + frames;
      c->resyncs++;
    }
  }
  uint32_t t0 = gr_clock_ticks();
  int r = gr_graph_run_frames(&h->g, frames);
  uint32_t dt = gr_clock_ticks() - t0;
  if (dt > h->block_max_ticks) h->block_max_ticks = dt;
  clk_advance(c, h->g.block.frame_pos);
  h->stats_count += frames;
  if (h->stats_count >= h->stats_every) {
    h->stats_count = 0;
    publish(h);
  }
  return r;
}

int gr_hub_out_ump(gr_hub *h, int out, uint32_t now_us, gr_tevent *ev) {
  if (out < 0 || out >= h->n_out) return 0;
  const gr_tevent *e = (const gr_tevent *)gr_spsc_peek(&h->out_q[out]);
  if (!e || !gr_us_reached(now_us, e->t_us)) return 0;
  return gr_spsc_pop(&h->out_q[out], ev);
}

int gr_hub_out_midi1(gr_hub *h, int out, uint32_t now_us, uint8_t msg[3], int *len) {
  gr_tevent e;
  while (gr_hub_out_ump(h, out, now_us, &e)) {
    int n = gr_ump_to_midi1(e.ump, msg);
    if (n > 0) {
      *len = n;
      return 1;
    }
  }
  return 0;
}

int gr_hub_stats_take(gr_hub *h, gr_hub_stats *out) {
  int got = 0;
  while (gr_spsc_pop(&h->stats_q, out)) got = 1;  // keep the newest
  return got;
}
