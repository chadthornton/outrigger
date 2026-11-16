// Host tests for lib/graph: UMP helpers, the SPSC ring, the graph core and
// the routing nodes. Run with `make test` (or `make test-graph`).
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <thread>

#include "gr_graph.h"
#include "gr_nodes.h"
#include "gr_spsc.h"
#include "gr_ump.h"

static int failures = 0;
static int checks = 0;

#define EXPECT(cond)                                                \
  do {                                                              \
    ++checks;                                                       \
    if (!(cond)) {                                                  \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      ++failures;                                                   \
    }                                                               \
  } while (0)
#define EXPECT_EQ(got, want)                                                              \
  do {                                                                                    \
    ++checks;                                                                             \
    long long g_ = (long long)(got), w_ = (long long)(want);                              \
    if (g_ != w_) {                                                                       \
      printf("FAIL %s:%d: %s = %lld (0x%llx), want %lld (0x%llx)\n", __FILE__, __LINE__, \
             #got, g_, (unsigned long long)g_, w_, (unsigned long long)w_);               \
      ++failures;                                                                         \
    }                                                                                     \
  } while (0)

// ---- helpers ----

static void m1(gr_events *e, uint32_t frame, uint8_t st, uint8_t d1, uint8_t d2) {
  uint32_t w[4] = {gr_ump_m1(0, st, d1, d2), 0, 0, 0};
  gr_events_push(e, frame, w);
}

static void m2note(gr_events *e, uint32_t frame, uint32_t op, uint32_t ch, uint32_t note,
                   uint32_t vel16) {
  uint32_t w[4] = {0, 0, 0, 0};
  gr_ump_m2_note(0, op, ch, note, vel16, w);
  gr_events_push(e, frame, w);
}

// A node whose output the test fills directly: the graph's input in tests.
struct Feed {
  gr_events next;
};
static void feed_process(void *self, const gr_block *b, gr_io *io) {
  (void)b;
  Feed *f = (Feed *)self;
  *io->ev_out[0] = f->next;
  gr_events_clear(&f->next);
}
static const gr_node_desc feed_desc = {"feed", 0, 1, {0}, {GR_PORT_EVENTS}, feed_process};

// ---- UMP ----

static void test_ump() {
  EXPECT_EQ(gr_scale_up(0, 7, 16), 0);
  EXPECT_EQ(gr_scale_up(64, 7, 16), 0x8000);
  EXPECT_EQ(gr_scale_up(127, 7, 16), 0xFFFF);
  EXPECT_EQ(gr_scale_up(127, 7, 32), 0xFFFFFFFFu);
  EXPECT_EQ(gr_scale_up(0x2000, 14, 32), 0x80000000u);
  EXPECT_EQ(gr_scale_up(0x3FFF, 14, 32), 0xFFFFFFFFu);

  EXPECT_EQ(gr_ump_words(0x20000000), 1);
  EXPECT_EQ(gr_ump_words(0x40000000), 2);
  EXPECT_EQ(gr_ump_words(0x30000000), 2);
  EXPECT_EQ(gr_ump_words(0x50000000), 4);
  EXPECT_EQ(gr_ump_words(0x10000000), 1);

  uint32_t w[2];
  uint8_t on0[3] = {0x93, 60, 0};
  EXPECT_EQ(gr_ump_from_midi1(on0, 3, 2, 1, w), 2);
  EXPECT_EQ(gr_ump_type(w[0]), GR_UMP_M2_CV);
  EXPECT_EQ(gr_ump_opcode(w[0]), GR_OP_NOTE_OFF);  // spec: vel 0 note on -> note off
  EXPECT_EQ(gr_ump_group(w[0]), 2);
  EXPECT_EQ(gr_ump_channel(w[0]), 3);

  // MIDI 1.0 -> 2.0 -> 1.0 is lossless for every 7-bit value.
  int lossless = 1;
  for (int v = 1; v < 128; ++v) {
    uint8_t on[3] = {0x90, 64, (uint8_t)v}, cc[3] = {0xB5, 74, (uint8_t)v}, back[3];
    gr_ump_from_midi1(on, 3, 0, 1, w);
    if (gr_ump_to_midi1(w, back) != 3 || memcmp(on, back, 3) != 0) lossless = 0;
    gr_ump_from_midi1(cc, 3, 0, 1, w);
    if (gr_ump_to_midi1(w, back) != 3 || memcmp(cc, back, 3) != 0) lossless = 0;
  }
  EXPECT(lossless);
  uint8_t pb[3] = {0xE1, 0x00, 0x40}, back[3];
  EXPECT_EQ(gr_ump_from_midi1(pb, 3, 0, 1, w), 2);
  EXPECT_EQ(w[1], 0x80000000u);
  EXPECT_EQ(gr_ump_to_midi1(w, back), 3);
  EXPECT(memcmp(pb, back, 3) == 0);
  uint8_t pc[2] = {0xC4, 17};
  EXPECT_EQ(gr_ump_from_midi1(pc, 2, 0, 1, w), 2);
  EXPECT_EQ(gr_ump_to_midi1(w, back), 2);
  EXPECT_EQ(back[1], 17);

  // A MIDI 2.0 note on too quiet for 7 bits stays a note on.
  gr_ump_m2_note(0, GR_OP_NOTE_ON, 0, 60, 5, w);
  EXPECT_EQ(gr_ump_to_midi1(w, back), 3);
  EXPECT_EQ(back[2], 1);

  uint8_t clk[1] = {0xF8};
  EXPECT_EQ(gr_ump_from_midi1(clk, 1, 0, 1, w), 1);
  EXPECT_EQ(gr_ump_type(w[0]), GR_UMP_SYSTEM);
  EXPECT_EQ(gr_ump_to_midi1(w, back), 1);
  EXPECT_EQ(back[0], 0xF8);
  uint8_t sx[1] = {0xF0};
  EXPECT_EQ(gr_ump_from_midi1(sx, 1, 0, 1, w), 0);

  uint32_t on_m1[1] = {gr_ump_m1(0, 0x90, 60, 0)};
  EXPECT(gr_ump_is_note_off(on_m1));
  EXPECT(!gr_ump_is_note_on(on_m1));
}

// ---- events ----

static void test_events() {
  static gr_events e;
  gr_events_clear(&e);
  m1(&e, 10, 0x90, 1, 1);
  m1(&e, 5, 0x90, 2, 1);
  m1(&e, 10, 0x90, 3, 1);
  m1(&e, 0, 0x90, 4, 1);
  EXPECT_EQ(e.count, 4);
  EXPECT_EQ(e.ev[0].frame, 0);
  EXPECT_EQ(gr_ump_index(e.ev[1].ump[0]), 2);
  EXPECT_EQ(gr_ump_index(e.ev[2].ump[0]), 1);  // stable at equal frames
  EXPECT_EQ(gr_ump_index(e.ev[3].ump[0]), 3);
  gr_events_clear(&e);
  for (int i = 0; i < GR_MAX_EVENTS + 5; ++i) m1(&e, 0, 0x90, 1, 1);
  EXPECT_EQ(e.count, GR_MAX_EVENTS);
  EXPECT_EQ(e.dropped, 5);
  // Unused UMP words are zero, so events compare by value.
  gr_events_clear(&e);
  uint32_t junk[4] = {gr_ump_m1(0, 0x90, 1, 1), 0xDEAD, 0xBEEF, 0xF00D};
  gr_events_push(&e, 0, junk);
  EXPECT_EQ(e.ev[0].ump[1], 0);
}

// ---- SPSC ----

static void test_spsc_basic() {
  uint32_t buf[8];
  gr_spsc q;
  EXPECT(!gr_spsc_init(&q, buf, 6, 4));  // not a power of two
  EXPECT(gr_spsc_init(&q, buf, 8, 4));
  uint32_t v;
  EXPECT(!gr_spsc_pop(&q, &v));
  EXPECT(gr_spsc_peek(&q) == nullptr);
  for (uint32_t i = 0; i < 8; ++i) EXPECT(gr_spsc_push(&q, &i));
  uint32_t nine = 9;
  EXPECT(!gr_spsc_push(&q, &nine));
  EXPECT_EQ(q.dropped, 1);
  EXPECT_EQ(gr_spsc_count(&q), 8);
  EXPECT_EQ(*(const uint32_t *)gr_spsc_peek(&q), 0);
  int fifo = 1;
  for (uint32_t i = 0; i < 8; ++i) fifo &= gr_spsc_pop(&q, &v) && v == i;
  EXPECT(fifo);
  // The 32-bit indices wrap.
  q.head = q.tail = 0xFFFFFFFCu;
  int wrap = 1;
  for (uint32_t i = 0; i < 100; ++i) {
    wrap &= gr_spsc_push(&q, &i);
    wrap &= gr_spsc_pop(&q, &v) && v == i;
  }
  EXPECT(wrap);
}

// Two threads (two cores on the Mac): every pushed value arrives, in order.
static void test_spsc_threads() {
  static uint64_t buf[1024];
  static gr_spsc q;
  gr_spsc_init(&q, buf, 1024, sizeof(uint64_t));
  const uint64_t kN = 5000000;
  std::atomic<uint64_t> pushed{0};
  std::thread prod([&] {
    uint64_t i = 0;
    while (i < kN) {
      if (gr_spsc_push(&q, &i)) ++i;
    }
    pushed = i;
  });
  uint64_t expect = 0, bad = 0, v;
  while (expect < kN) {
    if (gr_spsc_pop(&q, &v)) {
      if (v != expect) ++bad;
      expect = v + 1;
    }
  }
  prod.join();
  EXPECT_EQ(bad, 0);
  EXPECT_EQ(expect, kN);
  EXPECT_EQ(pushed.load(), kN);
  printf("spsc: %llu values across two threads, %u full-ring retries, 0 lost\n",
         (unsigned long long)kN, q.dropped);
}

// ---- graph ----

static gr_graph g;  // ~80 KB: not on the stack

static void test_graph_build() {
  EXPECT_EQ(gr_graph_init(&g, 48000, 0), GR_ERR_ARG);
  EXPECT_EQ(gr_graph_init(&g, 48000, GR_MAX_FRAMES + 1), GR_ERR_ARG);
  EXPECT_EQ(gr_graph_init(&g, 48000, 64), GR_OK);
  static Feed feed;
  static gr_impulse imp;
  static gr_transpose t1, t2;
  gr_transpose_init(&t1, 0);
  gr_transpose_init(&t2, 0);
  int f = gr_graph_add(&g, &feed_desc, &feed);
  int i = gr_graph_add(&g, &gr_impulse_desc, &imp);
  int a = gr_graph_add(&g, &gr_transpose_desc, &t1);
  int b = gr_graph_add(&g, &gr_transpose_desc, &t2);
  EXPECT_EQ(gr_graph_connect(&g, i, 0, a, 0), GR_ERR_TYPE);  // audio into events
  EXPECT_EQ(gr_graph_connect(&g, f, 1, a, 0), GR_ERR_ARG);   // no such port
  EXPECT_EQ(gr_graph_connect(&g, a, 0, b, 0), GR_OK);
  EXPECT_EQ(gr_graph_connect(&g, f, 0, b, 0), GR_ERR_TAKEN);
  EXPECT_EQ(gr_graph_connect(&g, b, 0, a, 0), GR_OK);
  EXPECT_EQ(gr_graph_compile(&g), GR_ERR_CYCLE);

  // Nodes added out of order still run sources first.
  gr_graph_init(&g, 48000, 64);
  static Feed feed2;
  static gr_sink sink;
  static uint8_t qbuf[64 * sizeof(gr_qevent)];
  static gr_spsc q;
  gr_spsc_init(&q, qbuf, 64, sizeof(gr_qevent));
  gr_sink_init(&sink, &q);
  int s = gr_graph_add(&g, &gr_sink_desc, &sink);
  int t = gr_graph_add(&g, &gr_transpose_desc, &t1);
  f = gr_graph_add(&g, &feed_desc, &feed2);
  gr_graph_connect(&g, t, 0, s, 0);
  gr_graph_connect(&g, f, 0, t, 0);
  EXPECT_EQ(gr_graph_compile(&g), GR_OK);
  EXPECT_EQ(g.order[0], f);
  EXPECT_EQ(g.order[1], t);
  EXPECT_EQ(g.order[2], s);
  EXPECT_EQ(gr_graph_add(&g, &gr_pass_desc, nullptr), GR_ERR_COMPILED);
  gr_events_clear(&feed2.next);
  m1(&feed2.next, 7, 0x90, 60, 100);
  gr_graph_run(&g);
  gr_graph_run(&g);
  m1(&feed2.next, 63, 0x80, 60, 0);
  gr_graph_run(&g);
  gr_qevent out;
  EXPECT(gr_spsc_pop(&q, &out));
  EXPECT_EQ(out.abs_frame, 7);
  EXPECT(gr_spsc_pop(&q, &out));
  EXPECT_EQ(out.abs_frame, 2 * 64 + 63);
  EXPECT_EQ(g.block.index, 3);
  EXPECT_EQ(g.block.frame_pos, 192);
  EXPECT_EQ(gr_graph_timing(&g, t)->blocks, 3);

  // Block size and rate are configurable.
  EXPECT_EQ(gr_graph_init(&g, 44100, 128), GR_OK);
  EXPECT_EQ(g.block.frames, 128);
  EXPECT_EQ(g.block.sample_rate, 44100);
}

// Source: events queued ahead of time land on their own block and frame.
static void test_source() {
  gr_graph_init(&g, 48000, 64);
  static uint8_t qbuf[64 * sizeof(gr_qevent)];
  static gr_spsc q;
  static gr_source src;
  static gr_impulse imp;
  gr_spsc_init(&q, qbuf, 64, sizeof(gr_qevent));
  gr_source_init(&src, &q);
  int s = gr_graph_add(&g, &gr_source_desc, &src);
  int i = gr_graph_add(&g, &gr_impulse_desc, &imp);
  gr_graph_connect(&g, s, 0, i, 0);
  EXPECT_EQ(gr_graph_compile(&g), GR_OK);
  gr_qevent e = {};
  e.ump[0] = gr_ump_m1(0, 0x90, 60, 127);
  e.abs_frame = 100;  // block 1, frame 36
  gr_spsc_push(&q, &e);
  e.abs_frame = 200;  // block 3, frame 8
  gr_spsc_push(&q, &e);
  gr_graph_run(&g);
  EXPECT_EQ(gr_graph_events(&g, s, 0)->count, 0);
  gr_graph_run(&g);
  EXPECT_EQ(gr_graph_events(&g, s, 0)->count, 1);
  EXPECT_EQ(gr_graph_events(&g, s, 0)->ev[0].frame, 36);
  const float *a = gr_graph_audio(&g, i, 0);
  EXPECT(a[36] == 1.0f);
  EXPECT(a[35] == 0.0f && a[37] == 0.0f);
  gr_graph_run(&g);
  EXPECT(gr_graph_audio(&g, i, 0)[36] == 0.0f);  // audio outs zeroed every block
  gr_graph_run(&g);
  EXPECT_EQ(gr_graph_events(&g, s, 0)->ev[0].frame, 8);
  // Late: an event for a block already run lands on frame 0.
  e.abs_frame = 10;
  gr_spsc_push(&q, &e);
  gr_graph_run(&g);
  EXPECT_EQ(gr_graph_events(&g, s, 0)->count, 1);
  EXPECT_EQ(gr_graph_events(&g, s, 0)->ev[0].frame, 0);
  EXPECT_EQ(src.late, 1);
  EXPECT_EQ(imp.hits, 3);
}

// ---- routing nodes ----

static void test_merge() {
  gr_graph_init(&g, 48000, 64);
  static Feed a, b;
  static gr_merge m;
  gr_events_clear(&a.next);
  gr_events_clear(&b.next);
  int fa = gr_graph_add(&g, &feed_desc, &a);
  int fb = gr_graph_add(&g, &feed_desc, &b);
  int mg = gr_graph_add(&g, &gr_merge2_desc, &m);
  gr_graph_connect(&g, fa, 0, mg, 0);
  gr_graph_connect(&g, fb, 0, mg, 1);
  EXPECT_EQ(gr_graph_compile(&g), GR_OK);
  m1(&a.next, 0, 0x90, 1, 1);
  m1(&a.next, 20, 0x90, 2, 1);
  m1(&a.next, 40, 0x90, 3, 1);
  m1(&b.next, 10, 0x91, 11, 1);
  m1(&b.next, 20, 0x91, 12, 1);
  m1(&b.next, 63, 0x91, 13, 1);
  gr_graph_run(&g);
  const gr_events *o = gr_graph_events(&g, mg, 0);
  EXPECT_EQ(o->count, 6);
  int want[6] = {1, 11, 2, 12, 3, 13};  // by frame; at frame 20 input 0 first
  int ok = 1;
  for (int i = 0; i < 6 && i < o->count; ++i) ok &= (int)gr_ump_index(o->ev[i].ump[0]) == want[i];
  EXPECT(ok);
  int sorted = 1;
  for (int i = 1; i < o->count; ++i) sorted &= o->ev[i - 1].frame <= o->ev[i].frame;
  EXPECT(sorted);
}

static void test_split() {
  gr_graph_init(&g, 48000, 64);
  static Feed f;
  static gr_split sp;
  gr_events_clear(&f.next);
  gr_split_init(&sp, 60, GR_SPLIT_BOTH);
  int fi = gr_graph_add(&g, &feed_desc, &f);
  int si = gr_graph_add(&g, &gr_split_desc, &sp);
  gr_graph_connect(&g, fi, 0, si, 0);
  gr_graph_compile(&g);
  m2note(&f.next, 0, GR_OP_NOTE_ON, 0, 59, 0x8000);
  m2note(&f.next, 1, GR_OP_NOTE_ON, 0, 60, 0x8000);
  m1(&f.next, 2, 0xB0, 64, 127);  // sustain: both sides
  gr_graph_run(&g);
  const gr_events *lo = gr_graph_events(&g, si, 0), *hi = gr_graph_events(&g, si, 1);
  EXPECT_EQ(lo->count, 2);
  EXPECT_EQ(hi->count, 2);
  EXPECT_EQ(gr_ump_index(lo->ev[0].ump[0]), 59);
  EXPECT_EQ(gr_ump_index(hi->ev[0].ump[0]), 60);
  // Move the split above both notes: the note offs still follow their note ons.
  sp.key = 72;
  m2note(&f.next, 5, GR_OP_NOTE_OFF, 0, 60, 0);
  m1(&f.next, 6, 0x90, 59, 0);  // MIDI 1.0 note off (velocity 0)
  m2note(&f.next, 7, GR_OP_NOTE_ON, 0, 65, 0x8000);  // now lower
  gr_graph_run(&g);
  EXPECT_EQ(hi->count, 1);
  EXPECT_EQ(gr_ump_index(hi->ev[0].ump[0]), 60);
  EXPECT(gr_ump_is_note_off(hi->ev[0].ump));
  EXPECT_EQ(lo->count, 2);
  EXPECT_EQ(gr_ump_index(lo->ev[0].ump[0]), 59);
  EXPECT_EQ(gr_ump_index(lo->ev[1].ump[0]), 65);
  // `other` routes non-note messages to one side only.
  sp.other = GR_SPLIT_UPPER;
  m1(&f.next, 0, 0xE0, 0, 64);
  gr_graph_run(&g);
  EXPECT_EQ(lo->count, 0);
  EXPECT_EQ(hi->count, 1);
}

static void test_chanmap() {
  gr_graph_init(&g, 48000, 64);
  static Feed f;
  static gr_chanmap cm;
  gr_events_clear(&f.next);
  gr_chanmap_init(&cm);
  cm.map[0] = 9;
  cm.map[1] = GR_DROP;
  int fi = gr_graph_add(&g, &feed_desc, &f);
  int ci = gr_graph_add(&g, &gr_chanmap_desc, &cm);
  gr_graph_connect(&g, fi, 0, ci, 0);
  gr_graph_compile(&g);
  const gr_events *o = gr_graph_events(&g, ci, 0);
  m1(&f.next, 0, 0x90, 60, 100);
  m1(&f.next, 1, 0x91, 61, 100);
  m2note(&f.next, 2, GR_OP_NOTE_ON, 2, 62, 0x8000);
  uint32_t clk[4] = {gr_ump_system(0, 0xF8, 0, 0), 0, 0, 0};
  gr_events_push(&f.next, 3, clk);
  gr_graph_run(&g);
  EXPECT_EQ(o->count, 3);
  EXPECT_EQ(gr_ump_channel(o->ev[0].ump[0]), 9);
  EXPECT_EQ(gr_ump_channel(o->ev[1].ump[0]), 2);
  EXPECT_EQ(gr_ump_type(o->ev[1].ump[0]), GR_UMP_M2_CV);
  EXPECT_EQ(o->ev[1].ump[1], 0x80000000u);  // the MIDI 2.0 velocity word is kept
  EXPECT_EQ(gr_ump_type(o->ev[2].ump[0]), GR_UMP_SYSTEM);
  // Remap mid-note: the note off goes to channel 9, where its note on went.
  cm.map[0] = 4;
  m1(&f.next, 0, 0x80, 60, 0);
  m1(&f.next, 1, 0xB0, 7, 100);
  gr_graph_run(&g);
  EXPECT_EQ(o->count, 2);
  EXPECT_EQ(gr_ump_channel(o->ev[0].ump[0]), 9);
  EXPECT_EQ(gr_ump_channel(o->ev[1].ump[0]), 4);
}

static void test_transpose() {
  gr_graph_init(&g, 48000, 64);
  static Feed f;
  static gr_transpose tr;
  gr_events_clear(&f.next);
  gr_transpose_init(&tr, 12);
  int fi = gr_graph_add(&g, &feed_desc, &f);
  int ti = gr_graph_add(&g, &gr_transpose_desc, &tr);
  gr_graph_connect(&g, fi, 0, ti, 0);
  gr_graph_compile(&g);
  const gr_events *o = gr_graph_events(&g, ti, 0);
  m1(&f.next, 0, 0x90, 60, 100);
  m1(&f.next, 1, 0x90, 120, 100);  // 132: out of range, dropped
  m1(&f.next, 2, 0xB0, 60, 1);     // a CC numbered 60 is not a note
  m1(&f.next, 3, 0xA0, 60, 50);    // poly pressure follows the note
  gr_graph_run(&g);
  EXPECT_EQ(o->count, 3);
  EXPECT_EQ(gr_ump_index(o->ev[0].ump[0]), 72);
  EXPECT_EQ(gr_ump_index(o->ev[1].ump[0]), 60);
  EXPECT_EQ(gr_ump_index(o->ev[2].ump[0]), 72);
  EXPECT_EQ(tr.out_of_range, 1);
  // Change the transpose mid-note: the note off still matches its note on.
  tr.semitones = -5;
  m1(&f.next, 0, 0x80, 60, 0);
  m1(&f.next, 1, 0x80, 120, 0);  // its note on was dropped: so is the off
  m2note(&f.next, 2, GR_OP_NOTE_ON, 0, 3, 0x8000);  // -2: dropped
  m2note(&f.next, 3, GR_OP_NOTE_ON, 0, 64, 0x8000);
  gr_graph_run(&g);
  EXPECT_EQ(o->count, 2);
  EXPECT_EQ(gr_ump_index(o->ev[0].ump[0]), 72);
  EXPECT_EQ(gr_ump_index(o->ev[1].ump[0]), 59);
}

static void test_ccremap() {
  gr_graph_init(&g, 48000, 64);
  static Feed f;
  static gr_ccremap cr;
  gr_events_clear(&f.next);
  gr_ccremap_init(&cr);
  cr.map[1] = 74;
  cr.map[7] = GR_DROP;
  cr.channel_mask = 0x0001;  // channel 0 only
  int fi = gr_graph_add(&g, &feed_desc, &f);
  int ci = gr_graph_add(&g, &gr_ccremap_desc, &cr);
  gr_graph_connect(&g, fi, 0, ci, 0);
  gr_graph_compile(&g);
  const gr_events *o = gr_graph_events(&g, ci, 0);
  m1(&f.next, 0, 0xB0, 1, 99);
  m1(&f.next, 1, 0xB0, 7, 99);
  m1(&f.next, 2, 0xB1, 1, 99);  // channel 1: untouched
  m1(&f.next, 3, 0x90, 1, 99);  // a note numbered 1: untouched
  uint32_t w[4] = {0, 0, 0, 0};
  gr_ump_m2_cc(0, 0, 1, 0x12345678u, w);
  gr_events_push(&f.next, 4, w);
  gr_graph_run(&g);
  EXPECT_EQ(o->count, 4);
  EXPECT_EQ(gr_ump_index(o->ev[0].ump[0]), 74);
  EXPECT_EQ(o->ev[0].ump[0] & 0x7F, 99);
  EXPECT_EQ(gr_ump_index(o->ev[1].ump[0]), 1);
  EXPECT_EQ(gr_ump_channel(o->ev[1].ump[0]), 1);
  EXPECT_EQ(gr_ump_opcode(o->ev[2].ump[0]), GR_OP_NOTE_ON);
  EXPECT_EQ(gr_ump_index(o->ev[3].ump[0]), 74);
  EXPECT_EQ(o->ev[3].ump[1], 0x12345678u);
}

// The hub use cases end to end: two controllers merged, split at middle C,
// the lower half to channel 2, the upper half up an octave on channel 3, one
// CC renamed, out through the sink queue in absolute frames.
static void test_hub_chain() {
  gr_graph_init(&g, 48000, 64);
  static Feed keys, pads;
  static gr_merge mg;
  static gr_split sp;
  static gr_chanmap lo_map, hi_map;
  static gr_transpose hi_tr;
  static gr_ccremap cc;
  static gr_merge out_mg;
  static gr_sink sink;
  static uint8_t qbuf[256 * sizeof(gr_qevent)];
  static gr_spsc q;
  gr_events_clear(&keys.next);
  gr_events_clear(&pads.next);
  gr_split_init(&sp, 60, GR_SPLIT_LOWER);
  gr_chanmap_init(&lo_map);
  gr_chanmap_init(&hi_map);
  for (int c = 0; c < 16; ++c) {
    lo_map.map[c] = 1;
    hi_map.map[c] = 2;
  }
  gr_transpose_init(&hi_tr, 12);
  gr_ccremap_init(&cc);
  cc.map[1] = 74;
  gr_spsc_init(&q, qbuf, 256, sizeof(gr_qevent));
  gr_sink_init(&sink, &q);
  int k = gr_graph_add(&g, &feed_desc, &keys);
  int p = gr_graph_add(&g, &feed_desc, &pads);
  int m = gr_graph_add(&g, &gr_merge2_desc, &mg);
  int c = gr_graph_add(&g, &gr_ccremap_desc, &cc);
  int s = gr_graph_add(&g, &gr_split_desc, &sp);
  int lm = gr_graph_add(&g, &gr_chanmap_desc, &lo_map);
  int ht = gr_graph_add(&g, &gr_transpose_desc, &hi_tr);
  int hm = gr_graph_add(&g, &gr_chanmap_desc, &hi_map);
  int om = gr_graph_add(&g, &gr_merge2_desc, &out_mg);
  int sk = gr_graph_add(&g, &gr_sink_desc, &sink);
  gr_graph_connect(&g, k, 0, m, 0);
  gr_graph_connect(&g, p, 0, m, 1);
  gr_graph_connect(&g, m, 0, c, 0);
  gr_graph_connect(&g, c, 0, s, 0);
  gr_graph_connect(&g, s, 0, lm, 0);
  gr_graph_connect(&g, s, 1, ht, 0);
  gr_graph_connect(&g, ht, 0, hm, 0);
  gr_graph_connect(&g, lm, 0, om, 0);
  gr_graph_connect(&g, hm, 0, om, 1);
  gr_graph_connect(&g, om, 0, sk, 0);
  EXPECT_EQ(gr_graph_compile(&g), GR_OK);
  const int kBlocks = 1000;
  int sent = 0;
  gr_qevent e;
  int got = 0, order_ok = 1, lo_ok = 1, hi_ok = 1, cc_ok = 1, cc_seen = 0;
  uint64_t last = 0;
  for (int b = 0; b < kBlocks; ++b) {
    uint8_t note = (uint8_t)(36 + (b % 48));
    m1(&keys.next, (uint32_t)(b % 64), 0x90, note, 100);
    m1(&pads.next, 63 - (uint32_t)(b % 64), 0x80, note, 0);
    sent += 2;
    if (b % 10 == 0) {
      m1(&keys.next, 32, 0xB0, 1, (uint8_t)(b & 0x7F));
      ++sent;
    }
    gr_graph_run(&g);
    // Core 0's side: drain the sink queue every block.
    while (gr_spsc_pop(&q, &e)) {
      ++got;
      order_ok &= e.abs_frame >= last;
      last = e.abs_frame;
      uint32_t w0 = e.ump[0];
      if (gr_ump_opcode(w0) == GR_OP_CC) {
        ++cc_seen;
        cc_ok &= gr_ump_index(w0) == 74 && gr_ump_channel(w0) == 1;
      } else {
        uint32_t n = gr_ump_index(w0);
        if (gr_ump_channel(w0) == 1) lo_ok &= n < 60;
        else if (gr_ump_channel(w0) == 2) hi_ok &= n >= 72;
        else lo_ok = hi_ok = 0;
      }
    }
  }
  EXPECT_EQ(got, sent);
  EXPECT(order_ok);
  EXPECT(lo_ok);
  EXPECT(hi_ok);
  EXPECT(cc_ok);
  EXPECT_EQ(cc_seen, kBlocks / 10);
  EXPECT_EQ(g.ev_dropped_total, 0);
  EXPECT_EQ(q.dropped, 0);
}

int main() {
  test_ump();
  test_events();
  test_spsc_basic();
  test_spsc_threads();
  test_graph_build();
  test_source();
  test_merge();
  test_split();
  test_chanmap();
  test_transpose();
  test_ccremap();
  test_hub_chain();
  printf("test_graph: %d checks, %d failures; sizeof(gr_graph) = %zu bytes, sizeof(gr_event) = %zu\n",
         checks, failures, sizeof(gr_graph), sizeof(gr_event));
  return failures ? 1 : 0;
}
