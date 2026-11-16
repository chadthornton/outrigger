// clock_gettime under strict -std=c11 on glibc (CI builds on Linux).
#if !defined(ESP_PLATFORM) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "gr_graph.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_cpu.h"
uint32_t gr_clock_ticks(void) { return (uint32_t)esp_cpu_get_cycle_count(); }
#define GR_TICKS_PER_US_DEFAULT 0  // set g->ticks_per_us to the CPU MHz
#elif defined(__APPLE__)
#include <time.h>
uint32_t gr_clock_ticks(void) { return (uint32_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
#define GR_TICKS_PER_US_DEFAULT 1000
#else
#include <time.h>
uint32_t gr_clock_ticks(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}
#define GR_TICKS_PER_US_DEFAULT 1000
#endif

int gr_events_push(gr_events *e, uint32_t frame, const uint32_t *ump) {
  if (e->count >= GR_MAX_EVENTS) {
    e->dropped++;
    return 0;
  }
  int n = gr_ump_words(ump[0]);
  int i = e->count;
  // Keep frame order; the common case (in order) never enters the loop.
  while (i > 0 && e->ev[i - 1].frame > frame) {
    e->ev[i] = e->ev[i - 1];
    --i;
  }
  gr_event *ev = &e->ev[i];
  ev->frame = frame;
  for (int k = 0; k < 4; ++k) ev->ump[k] = k < n ? ump[k] : 0;
  e->count++;
  return 1;
}

int gr_graph_init(gr_graph *g, uint32_t sample_rate, uint32_t frames) {
  if (!g || frames == 0 || frames > GR_MAX_FRAMES || sample_rate == 0) return GR_ERR_ARG;
  memset(g, 0, sizeof(*g));
  g->block.frames = frames;
  g->default_frames = frames;
  g->block.sample_rate = sample_rate;
  g->ticks_per_us = GR_TICKS_PER_US_DEFAULT;
  return GR_OK;
}

int gr_graph_add(gr_graph *g, const gr_node_desc *desc, void *self) {
  if (!g || !desc || !desc->process || desc->n_in > GR_MAX_PORTS || desc->n_out > GR_MAX_PORTS)
    return GR_ERR_ARG;
  if (g->compiled) return GR_ERR_COMPILED;
  if (g->n_nodes >= GR_MAX_NODES) return GR_ERR_FULL;
  int id = g->n_nodes++;
  gr_slot *s = &g->slot[id];
  memset(s, 0, sizeof(*s));
  s->desc = desc;
  s->self = self;
  for (int p = 0; p < GR_MAX_PORTS; ++p) s->src_node[p] = -1;
  return id;
}

int gr_graph_connect(gr_graph *g, int src, int src_port, int dst, int dst_port) {
  if (!g || src < 0 || dst < 0 || src >= g->n_nodes || dst >= g->n_nodes) return GR_ERR_ARG;
  if (g->compiled) return GR_ERR_COMPILED;
  const gr_node_desc *sd = g->slot[src].desc, *dd = g->slot[dst].desc;
  if (src_port < 0 || src_port >= sd->n_out || dst_port < 0 || dst_port >= dd->n_in)
    return GR_ERR_ARG;
  if (sd->out_type[src_port] != dd->in_type[dst_port]) return GR_ERR_TYPE;
  if (g->slot[dst].src_node[dst_port] >= 0) return GR_ERR_TAKEN;
  g->slot[dst].src_node[dst_port] = (int8_t)src;
  g->slot[dst].src_port[dst_port] = (uint8_t)src_port;
  return GR_OK;
}

int gr_graph_compile(gr_graph *g) {
  if (!g) return GR_ERR_ARG;
  if (g->compiled) return GR_ERR_COMPILED;
  // Outputs first: every output port gets its own buffer from the pools.
  for (int n = 0; n < g->n_nodes; ++n) {
    gr_slot *s = &g->slot[n];
    for (int p = 0; p < s->desc->n_out; ++p) {
      if (s->desc->out_type[p] == GR_PORT_EVENTS) {
        if (g->n_ev >= GR_EVENT_POOL) return GR_ERR_FULL;
        s->io.ev_out[p] = &g->ev_pool[g->n_ev++];
      } else if (s->desc->out_type[p] == GR_PORT_AUDIO) {
        if (g->n_audio >= GR_AUDIO_POOL) return GR_ERR_FULL;
        s->io.audio_out[p] = g->audio_pool[g->n_audio++];
      }
    }
  }
  // Inputs point at their source's output, or at the shared empty / silence.
  for (int n = 0; n < g->n_nodes; ++n) {
    gr_slot *s = &g->slot[n];
    for (int p = 0; p < s->desc->n_in; ++p) {
      int src = s->src_node[p];
      if (s->desc->in_type[p] == GR_PORT_EVENTS)
        s->io.ev_in[p] = src >= 0 ? g->slot[src].io.ev_out[s->src_port[p]] : &g->empty;
      else if (s->desc->in_type[p] == GR_PORT_AUDIO)
        s->io.audio_in[p] = src >= 0 ? g->slot[src].io.audio_out[s->src_port[p]] : g->silence;
    }
  }
  // Kahn's topological sort; ties keep insertion order, so runs are repeatable.
  uint8_t indeg[GR_MAX_NODES] = {0};
  uint8_t done[GR_MAX_NODES] = {0};
  for (int n = 0; n < g->n_nodes; ++n)
    for (int p = 0; p < g->slot[n].desc->n_in; ++p)
      if (g->slot[n].src_node[p] >= 0) indeg[n]++;
  int placed = 0;
  while (placed < g->n_nodes) {
    int pick = -1;
    for (int n = 0; n < g->n_nodes; ++n)
      if (!done[n] && indeg[n] == 0) {
        pick = n;
        break;
      }
    if (pick < 0) return GR_ERR_CYCLE;
    done[pick] = 1;
    g->order[placed++] = (uint8_t)pick;
    for (int n = 0; n < g->n_nodes; ++n)
      for (int p = 0; p < g->slot[n].desc->n_in; ++p)
        if (g->slot[n].src_node[p] == pick) indeg[n]--;
  }
  g->compiled = 1;
  return GR_OK;
}

void gr_graph_run(gr_graph *g) { gr_graph_run_frames(g, g->default_frames); }

int gr_graph_set_sample_rate(gr_graph *g, uint32_t sample_rate) {
  if (!g || sample_rate == 0) return GR_ERR_ARG;
  g->block.sample_rate = sample_rate;
  return GR_OK;
}

int gr_graph_run_frames(gr_graph *g, uint32_t frames) {
  if (!g || frames == 0 || frames > GR_MAX_FRAMES) return GR_ERR_ARG;
  g->block.frames = frames;
  for (int i = 0; i < g->n_nodes; ++i) {
    gr_slot *s = &g->slot[g->order[i]];
    const gr_node_desc *d = s->desc;
    for (int p = 0; p < d->n_out; ++p) {
      if (s->io.ev_out[p]) gr_events_clear(s->io.ev_out[p]);
      if (s->io.audio_out[p]) memset(s->io.audio_out[p], 0, frames * sizeof(float));
    }
    uint32_t t0 = gr_clock_ticks();
    d->process(s->self, &g->block, &s->io);
    uint32_t dt = gr_clock_ticks() - t0;
    s->t.last = dt;
    if (dt > s->t.max) s->t.max = dt;
    s->t.total += dt;
    s->t.blocks++;
    for (int p = 0; p < d->n_out; ++p)
      if (s->io.ev_out[p]) g->ev_dropped_total += s->io.ev_out[p]->dropped;
  }
  g->block.index++;
  g->block.frame_pos += frames;
  return GR_OK;
}

const gr_events *gr_graph_events(const gr_graph *g, int node, int port) {
  if (!g || node < 0 || node >= g->n_nodes || port < 0 || port >= GR_MAX_PORTS) return 0;
  return g->slot[node].io.ev_out[port];
}

const float *gr_graph_audio(const gr_graph *g, int node, int port) {
  if (!g || node < 0 || node >= g->n_nodes || port < 0 || port >= GR_MAX_PORTS) return 0;
  return g->slot[node].io.audio_out[port];
}

const gr_timing *gr_graph_timing(const gr_graph *g, int node) {
  if (!g || node < 0 || node >= g->n_nodes) return 0;
  return &g->slot[node].t;
}

void gr_graph_reset_timing(gr_graph *g) {
  for (int n = 0; n < g->n_nodes; ++n) memset(&g->slot[n].t, 0, sizeof(gr_timing));
}
