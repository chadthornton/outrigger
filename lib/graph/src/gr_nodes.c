#include "gr_nodes.h"

#include <string.h>

static inline int bit_get(const uint32_t *set, uint32_t n) { return (set[n >> 5] >> (n & 31)) & 1; }
static inline void bit_set(uint32_t *set, uint32_t n) { set[n >> 5] |= 1u << (n & 31); }
static inline void bit_clr(uint32_t *set, uint32_t n) { set[n >> 5] &= ~(1u << (n & 31)); }

static inline int is_note(uint32_t w0) {
  uint32_t op = gr_ump_opcode(w0);
  return gr_ump_is_cv(w0) && (op == GR_OP_NOTE_ON || op == GR_OP_NOTE_OFF);
}

// ---- source / sink ----

void gr_source_init(gr_source *n, gr_spsc *q) {
  memset(n, 0, sizeof(*n));
  n->q = q;
}

static void source_process(void *self, const gr_block *b, gr_io *io) {
  gr_source *n = (gr_source *)self;
  const uint64_t end = b->frame_pos + b->frames;
  const gr_qevent *e;
  while ((e = (const gr_qevent *)gr_spsc_peek(n->q)) != 0 && e->abs_frame < end) {
    uint32_t frame = 0;
    if (e->abs_frame >= b->frame_pos)
      frame = (uint32_t)(e->abs_frame - b->frame_pos);
    else
      n->late++;
    if (!gr_events_push(io->ev_out[0], frame, e->ump)) break;  // full: leave it for next block
    gr_spsc_pop(n->q, 0);
    n->taken++;
  }
}

const gr_node_desc gr_source_desc = {"source", 0, 1, {0}, {GR_PORT_EVENTS}, source_process};

void gr_sink_init(gr_sink *n, gr_spsc *q) {
  memset(n, 0, sizeof(*n));
  n->q = q;
}

static void sink_process(void *self, const gr_block *b, gr_io *io) {
  gr_sink *n = (gr_sink *)self;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) {
    gr_qevent q;
    q.abs_frame = b->frame_pos + in->ev[i].frame;
    memcpy(q.ump, in->ev[i].ump, sizeof(q.ump));
    if (gr_spsc_push(n->q, &q)) n->sent++;
  }
}

const gr_node_desc gr_sink_desc = {"sink", 1, 0, {GR_PORT_EVENTS}, {0}, sink_process};

// ---- merge ----

static void merge_n(gr_io *io, int n_in) {
  int pos[GR_MAX_PORTS] = {0};
  gr_events *out = io->ev_out[0];
  for (;;) {
    int best = -1;
    uint32_t best_frame = 0;
    for (int p = 0; p < n_in; ++p) {
      const gr_events *in = io->ev_in[p];
      if (pos[p] < in->count && (best < 0 || in->ev[pos[p]].frame < best_frame)) {
        best = p;
        best_frame = in->ev[pos[p]].frame;
      }
    }
    if (best < 0) return;
    const gr_event *e = &io->ev_in[best]->ev[pos[best]++];
    gr_events_push(out, e->frame, e->ump);
  }
}

static void merge2_process(void *self, const gr_block *b, gr_io *io) {
  (void)self;
  (void)b;
  merge_n(io, 2);
}
static void merge4_process(void *self, const gr_block *b, gr_io *io) {
  (void)self;
  (void)b;
  merge_n(io, 4);
}

const gr_node_desc gr_merge2_desc = {
    "merge2", 2, 1, {GR_PORT_EVENTS, GR_PORT_EVENTS}, {GR_PORT_EVENTS}, merge2_process};
const gr_node_desc gr_merge4_desc = {
    "merge4",
    4,
    1,
    {GR_PORT_EVENTS, GR_PORT_EVENTS, GR_PORT_EVENTS, GR_PORT_EVENTS},
    {GR_PORT_EVENTS},
    merge4_process};

// ---- split ----

void gr_split_init(gr_split *n, uint8_t key, uint8_t other) {
  memset(n, 0, sizeof(*n));
  n->key = key;
  n->other = other;
}

static void split_process(void *self, const gr_block *b, gr_io *io) {
  (void)b;
  gr_split *n = (gr_split *)self;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) {
    const gr_event *e = &in->ev[i];
    uint32_t w0 = e->ump[0];
    int to_lower, to_upper;
    if (is_note(w0)) {
      uint32_t ch = gr_ump_channel(w0), note = gr_ump_index(w0) & 0x7F;
      if (gr_ump_is_note_off(e->ump)) {
        // Follow the note on, wherever the key has moved since.
        if (bit_get(n->upper_on[ch], note)) {
          to_upper = 1, to_lower = 0;
          bit_clr(n->upper_on[ch], note);
        } else if (bit_get(n->lower_on[ch], note)) {
          to_upper = 0, to_lower = 1;
          bit_clr(n->lower_on[ch], note);
        } else {
          to_upper = note >= n->key;
          to_lower = !to_upper;
        }
      } else {
        to_upper = note >= n->key;
        to_lower = !to_upper;
        bit_set(to_upper ? n->upper_on[ch] : n->lower_on[ch], note);
      }
    } else {
      to_lower = n->other != GR_SPLIT_UPPER;
      to_upper = n->other != GR_SPLIT_LOWER;
    }
    if (to_lower) gr_events_push(io->ev_out[0], e->frame, e->ump);
    if (to_upper) gr_events_push(io->ev_out[1], e->frame, e->ump);
  }
}

const gr_node_desc gr_split_desc = {
    "split", 1, 2, {GR_PORT_EVENTS}, {GR_PORT_EVENTS, GR_PORT_EVENTS}, split_process};

// ---- channel map ----

void gr_chanmap_init(gr_chanmap *n) {
  for (int c = 0; c < 16; ++c) n->map[c] = (uint8_t)c;
  memset(n->note_ch, GR_DROP, sizeof(n->note_ch));
}

static void chanmap_process(void *self, const gr_block *b, gr_io *io) {
  (void)b;
  gr_chanmap *n = (gr_chanmap *)self;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) {
    const gr_event *e = &in->ev[i];
    uint32_t w0 = e->ump[0];
    if (!gr_ump_is_cv(w0)) {
      gr_events_push(io->ev_out[0], e->frame, e->ump);
      continue;
    }
    uint32_t ch = gr_ump_channel(w0);
    uint8_t to = n->map[ch];
    if (is_note(w0)) {
      uint32_t note = gr_ump_index(w0) & 0x7F;
      if (gr_ump_is_note_off(e->ump)) {
        if (n->note_ch[ch][note] != GR_DROP || to == GR_DROP) to = n->note_ch[ch][note];
        n->note_ch[ch][note] = GR_DROP;
      } else {
        n->note_ch[ch][note] = to;
      }
    }
    if (to == GR_DROP) continue;
    uint32_t w[4] = {gr_ump_with_channel(w0, to), e->ump[1], e->ump[2], e->ump[3]};
    gr_events_push(io->ev_out[0], e->frame, w);
  }
}

const gr_node_desc gr_chanmap_desc = {"chanmap", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS},
                                      chanmap_process};

// ---- transpose ----

void gr_transpose_init(gr_transpose *n, int semitones) {
  memset(n, 0, sizeof(*n));
  n->semitones = (int8_t)semitones;
  memset(n->applied, GR_TR_NONE, sizeof(n->applied));
}

static void transpose_process(void *self, const gr_block *b, gr_io *io) {
  (void)b;
  gr_transpose *n = (gr_transpose *)self;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) {
    const gr_event *e = &in->ev[i];
    uint32_t w0 = e->ump[0];
    uint32_t op = gr_ump_opcode(w0);
    if (!gr_ump_is_cv(w0) || (op != GR_OP_NOTE_ON && op != GR_OP_NOTE_OFF && op != GR_OP_POLY_PRESSURE)) {
      gr_events_push(io->ev_out[0], e->frame, e->ump);
      continue;
    }
    uint32_t ch = gr_ump_channel(w0), note = gr_ump_index(w0) & 0x7F;
    int8_t *slot = &n->applied[ch][note];
    int off;
    if (gr_ump_is_note_on(e->ump)) {
      off = n->semitones;
      int t = (int)note + off;
      *slot = (t < 0 || t > 127) ? GR_TR_DROPPED : (int8_t)off;
    } else {
      off = *slot == GR_TR_NONE ? n->semitones : *slot;
      if (gr_ump_is_note_off(e->ump)) *slot = GR_TR_NONE;
    }
    if (off == GR_TR_DROPPED) continue;
    int t = (int)note + off;
    if (t < 0 || t > 127) {
      n->out_of_range++;
      continue;
    }
    uint32_t w[4] = {gr_ump_with_index(w0, (uint32_t)t), e->ump[1], e->ump[2], e->ump[3]};
    gr_events_push(io->ev_out[0], e->frame, w);
  }
}

const gr_node_desc gr_transpose_desc = {"transpose", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS},
                                        transpose_process};

// ---- CC remap ----

void gr_ccremap_init(gr_ccremap *n) {
  for (int c = 0; c < 128; ++c) n->map[c] = (uint8_t)c;
  n->channel_mask = 0xFFFF;
}

static void ccremap_process(void *self, const gr_block *b, gr_io *io) {
  (void)b;
  gr_ccremap *n = (gr_ccremap *)self;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) {
    const gr_event *e = &in->ev[i];
    uint32_t w0 = e->ump[0];
    if (!gr_ump_is_cv(w0) || gr_ump_opcode(w0) != GR_OP_CC ||
        !((n->channel_mask >> gr_ump_channel(w0)) & 1)) {
      gr_events_push(io->ev_out[0], e->frame, e->ump);
      continue;
    }
    uint8_t to = n->map[gr_ump_index(w0) & 0x7F];
    if (to == GR_DROP) continue;
    uint32_t w[4] = {gr_ump_with_index(w0, to), e->ump[1], e->ump[2], e->ump[3]};
    gr_events_push(io->ev_out[0], e->frame, w);
  }
}

const gr_node_desc gr_ccremap_desc = {"ccremap", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS},
                                      ccremap_process};

// ---- impulse (test) ----

static void impulse_process(void *self, const gr_block *b, gr_io *io) {
  gr_impulse *n = (gr_impulse *)self;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) {
    const gr_event *e = &in->ev[i];
    if (!gr_ump_is_note_on(e->ump) || e->frame >= b->frames) continue;
    float v = gr_ump_type(e->ump[0]) == GR_UMP_M2_CV ? (float)(e->ump[1] >> 16) / 65535.0f
                                                     : (float)(e->ump[0] & 0x7F) / 127.0f;
    io->audio_out[0][e->frame] += v;
    n->hits++;
  }
}

const gr_node_desc gr_impulse_desc = {"impulse", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_AUDIO},
                                      impulse_process};

// ---- pass (overhead probe) ----

static void pass_process(void *self, const gr_block *b, gr_io *io) {
  (void)self;
  (void)b;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) gr_events_push(io->ev_out[0], in->ev[i].frame, in->ev[i].ump);
}

const gr_node_desc gr_pass_desc = {"pass", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS}, pass_process};
