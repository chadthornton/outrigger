// Deliberately broken nodes: each breaks one rule of lib/graph/CONTRACT.md,
// and test_graph_check proves the checker fails it on that rule. Built with
// the counting allocator (-Dmalloc=gr_count_malloc -Dfree=gr_count_free).
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "gr_check.h"
#include "gr_graph.h"
#include "gr_ump.h"

typedef struct bad {
  uint32_t n;
  gr_events held;
} bad;

static int init_bad(void *self) {
  memset(self, 0, sizeof(bad));
  return 1;
}

static void copy_in(gr_io *io) {
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) gr_events_push(io->ev_out[0], in->ev[i].frame, in->ev[i].ump);
}

// Assumes 64 frames: a CC on frame 63 of every block.
static void fixed64(void *self, const gr_block *b, gr_io *io) {
  (void)self, (void)b;
  copy_in(io);
  uint32_t w[4] = {gr_ump_m1(0, 0xB0, 99, 1), 0, 0, 0};
  gr_events_push(io->ev_out[0], 63, w);
}
// Reverses its output (out of frame order).
static void reverse(void *self, const gr_block *b, gr_io *io) {
  (void)self, (void)b;
  const gr_events *in = io->ev_in[0];
  gr_events *out = io->ev_out[0];
  for (int i = in->count - 1; i >= 0; --i) {
    out->ev[out->count] = in->ev[i];
    out->count++;
  }
}
// Writes into its input.
static void mutate(void *self, const gr_block *b, gr_io *io) {
  (void)self, (void)b;
  gr_events *in = (gr_events *)io->ev_in[0];
  for (int i = 0; i < in->count; ++i) in->ev[i].ump[0] ^= 1;
  copy_in(io);
}
// Allocates on the audio flow.
static void allocates(void *self, const gr_block *b, gr_io *io) {
  (void)self, (void)b;
  void *p = malloc(64);
  free(p);
  copy_in(io);
}
// NaN on a note on.
static void nan_out(void *self, const gr_block *b, gr_io *io) {
  (void)self;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i)
    if (gr_ump_is_note_on(in->ev[i].ump) && in->ev[i].frame < b->frames) io->audio_out[0][in->ev[i].frame] = NAN;
}
// Writes the largest block's worth of audio, whatever the block.
static void overrun(void *self, const gr_block *b, gr_io *io) {
  (void)self, (void)b;
  for (int i = 0; i < GR_MAX_FRAMES; ++i) io->audio_out[0][i] = 0.25f;
}
// Drops a note off once in a while (stuck notes).
static void stuck(void *self, const gr_block *b, gr_io *io) {
  (void)b;
  bad *n = (bad *)self;
  const gr_events *in = io->ev_in[0];
  for (int i = 0; i < in->count; ++i) {
    if (gr_ump_is_note_off(in->ev[i].ump) && (++n->n % 7) == 0) continue;
    gr_events_push(io->ev_out[0], in->ev[i].frame, in->ev[i].ump);
  }
}
// Keeps its state per block: plays the last block's input on the same frames.
static void one_block_late(void *self, const gr_block *b, gr_io *io) {
  bad *n = (bad *)self;
  for (int i = 0; i < n->held.count; ++i)
    if (n->held.ev[i].frame < b->frames) gr_events_push(io->ev_out[0], n->held.ev[i].frame, n->held.ev[i].ump);
  memcpy(&n->held, io->ev_in[0], sizeof(gr_events));
}
// Crashes after a while.
static void crashes(void *self, const gr_block *b, gr_io *io) {
  (void)b;
  bad *n = (bad *)self;
  copy_in(io);
  if (++n->n == 5000) raise(SIGSEGV);  // as a wild pointer would (portable: gcc rejects a literal one)
}
// Slow: spins far past a 64-frame block's budget.
static void slow(void *self, const gr_block *b, gr_io *io) {
  (void)self, (void)b;
  uint32_t t0 = gr_clock_ticks();
  while (gr_clock_ticks() - t0 < 600000u) {
  }
  copy_in(io);
}

static const gr_node_desc d_fixed64 = {"bad_fixed64", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS}, fixed64};
static const gr_node_desc d_reverse = {"bad_reverse", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS}, reverse};
static const gr_node_desc d_mutate = {"bad_mutate", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS}, mutate};
static const gr_node_desc d_alloc = {"bad_alloc", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS}, allocates};
static const gr_node_desc d_nan = {"bad_nan", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_AUDIO}, nan_out};
static const gr_node_desc d_overrun = {"bad_overrun", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_AUDIO}, overrun};
static const gr_node_desc d_stuck = {"bad_stuck", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS}, stuck};
static const gr_node_desc d_late = {"bad_late", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS}, one_block_late};
static const gr_node_desc d_crash = {"bad_crash", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS}, crashes};
static const gr_node_desc d_slow = {"bad_slow", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS}, slow};
static const gr_node_desc d_name = {"bad name with spaces and far too long", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_EVENTS},
                                    fixed64};
static const gr_node_desc d_ports = {"bad_ports", 1, 1, {GR_PORT_EVENTS}, {GR_PORT_NONE}, fixed64};

#define BAD(d, fl) {d.name, fl, &d, sizeof(bad), init_bad, 0, 0, 0, 0, 0, 0}
const gr_ck_subject gr_bad_subjects[] = {
    BAD(d_fixed64, 0), BAD(d_reverse, 0), BAD(d_mutate, 0), BAD(d_alloc, 0),
    BAD(d_nan, 0),     BAD(d_overrun, 0), BAD(d_stuck, GR_CK_NOTES), BAD(d_late, 0),
    BAD(d_crash, 0),   BAD(d_slow, 0),    BAD(d_name, 0), BAD(d_ports, 0),
};
// The check each one must fail.
const char *const gr_bad_expect[] = {"Blocks", "Order",     "Inputs",    "Realtime", "Audio",  "Audio",
                                     "Notes",  "Partition", "Robustness", "Time",    "Format", "Format"};
const int gr_bad_count = (int)(sizeof(gr_bad_subjects) / sizeof(gr_bad_subjects[0]));
