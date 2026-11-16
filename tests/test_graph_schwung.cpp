// Host tests for the Schwung MIDI-effect shim (lib/graph/shim): two unmodified
// Schwung midi_fx modules, eucalypso (a Euclidean sequencer) and superarp (an
// arpeggiator), run as graph nodes under a MIDI clock.
//
//   ./build/test_graph_schwung tests/golden/graph            compare with the recordings
//   ./build/test_graph_schwung tests/golden/graph --record   rewrite them
//
// Three runs per module and script:
//  ref    the module driven directly through its own API the way Schwung's
//         chain host drives it (every message of a block, then one tick of
//         the block). This is the recording made on the Mac.
//  block  the same input through the graph, shim at tick_quantum = 64, every
//         event on frame 0 of its block. Must equal ref exactly.
//  exact  the input on its true frames, shim at tick_quantum = 1. Must be
//         repeatable, equal its own recording, and put clocked steps exactly
//         where the clock says.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

extern "C" {
#include "gr_graph.h"
#include "gr_nodes.h"
#include "gr_schwung.h"
#include "gr_spsc.h"
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"
midi_fx_api_v1_t *eucalypso_midi_fx_init(const host_api_v1_t *host);
midi_fx_api_v1_t *superarp_midi_fx_init(const host_api_v1_t *host);
extern unsigned long gr_alloc_calls;
}

static int failures = 0, checks = 0;
#define EXPECT(cond)                                         \
  do {                                                       \
    ++checks;                                                \
    if (!(cond)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                            \
    }                                                        \
  } while (0)

static const uint32_t kRate = 48000, kFrames = 64;
static const uint32_t kClockFrames = 1000;  // 24 ppqn at 120 BPM, 48 kHz

struct Msg {
  uint64_t frame;
  uint8_t b[3];
  int len;
};
typedef std::vector<Msg> Script;

// Start, hold C-E-G for 4 bars of MIDI clock, release, stop.
static Script clocked_chord(int bars) {
  Script s;
  const int clocks = bars * 4 * 24;
  s.push_back({0, {0xFA, 0, 0}, 1});
  s.push_back({0, {0x90, 60, 100}, 3});
  s.push_back({0, {0x90, 64, 100}, 3});
  s.push_back({0, {0x90, 67, 100}, 3});
  for (int c = 0; c < clocks; ++c) s.push_back({(uint64_t)c * kClockFrames, {0xF8, 0, 0}, 1});
  const uint64_t end = (uint64_t)clocks * kClockFrames;
  s.push_back({end - 500, {0x80, 60, 0}, 3});
  s.push_back({end - 500, {0x80, 64, 0}, 3});
  s.push_back({end - 500, {0x80, 67, 0}, 3});
  s.push_back({end, {0xFC, 0, 0}, 1});
  // Clocks were appended after the notes; the script is consumed in frame order.
  std::stable_sort(s.begin(), s.end(), [](const Msg &a, const Msg &b) { return a.frame < b.frame; });
  return s;
}

static std::string line(uint64_t frame, const uint8_t *b, int len) {
  char buf[64];
  if (len == 1) snprintf(buf, sizeof(buf), "%llu %02X\n", (unsigned long long)frame, b[0]);
  else if (len == 2) snprintf(buf, sizeof(buf), "%llu %02X %02X\n", (unsigned long long)frame, b[0], b[1]);
  else snprintf(buf, sizeof(buf), "%llu %02X %02X %02X\n", (unsigned long long)frame, b[0], b[1], b[2]);
  return buf;
}

typedef midi_fx_api_v1_t *(*InitFn)(const host_api_v1_t *);

struct Module {
  const char *name;
  InitFn init;
  std::vector<std::pair<const char *, const char *>> params;
};

static uint64_t script_end(const Script &s) { return s.back().frame + 48000; }

// ref: Schwung's chain host, by hand.
static std::string run_ref(const Module &m, const Script &s) {
  host_api_v1_t host;
  memset(&host, 0, sizeof(host));
  host.api_version = MOVE_PLUGIN_API_VERSION;
  static int status = MOVE_CLOCK_STATUS_RUNNING;
  host.get_clock_status = [] { return status; };
  midi_fx_api_v1_t *api = m.init(&host);
  void *inst = api->create_instance("", nullptr);
  for (auto &p : m.params) api->set_param(inst, p.first, p.second);
  std::string out;
  uint8_t msgs[MIDI_FX_MAX_OUT_MSGS][3];
  int lens[MIDI_FX_MAX_OUT_MSGS];
  size_t i = 0;
  const uint64_t end = script_end(s);
  for (uint64_t b0 = 0; b0 < end; b0 += kFrames) {
    for (; i < s.size() && s[i].frame < b0 + kFrames; ++i) {
      int c = api->process_midi(inst, s[i].b, s[i].len, msgs, lens, MIDI_FX_MAX_OUT_MSGS);
      for (int k = 0; k < c; ++k) out += line(b0, msgs[k], lens[k]);
    }
    int c = api->tick(inst, (int)kFrames, (int)kRate, msgs, lens, MIDI_FX_MAX_OUT_MSGS);
    for (int k = 0; k < c; ++k) out += line(b0, msgs[k], lens[k]);
  }
  api->destroy_instance(inst);
  return out;
}

static gr_graph g;

// The graph: source -> schwung -> sink, with the input sent as MIDI 2.0
// protocol UMPs (so the shim's conversions are on the path too).
static std::string run_graph(const Module &m, const Script &s, bool quantize, uint32_t quantum,
                             unsigned long *allocs_while_running, gr_timing *timing) {
  static gr_qevent in_buf[1 << 14], out_buf[1 << 14];
  static gr_spsc in_q, out_q;
  static gr_source src;
  static gr_schwung fx;
  static gr_sink sink;
  gr_spsc_init(&in_q, in_buf, 1 << 14, sizeof(gr_qevent));
  gr_spsc_init(&out_q, out_buf, 1 << 14, sizeof(gr_qevent));
  gr_graph_init(&g, kRate, kFrames);
  gr_source_init(&src, &in_q);
  gr_sink_init(&sink, &out_q);
  EXPECT(gr_schwung_init(&fx, (gr_midi_fx_init_fn)m.init, "", quantum, 0));
  for (auto &p : m.params) gr_schwung_set(&fx, p.first, p.second);
  int a = gr_graph_add(&g, &gr_source_desc, &src);
  int b = gr_graph_add(&g, &gr_schwung_desc, &fx);
  int c = gr_graph_add(&g, &gr_sink_desc, &sink);
  gr_graph_connect(&g, a, 0, b, 0);
  gr_graph_connect(&g, b, 0, c, 0);
  EXPECT(gr_graph_compile(&g) == GR_OK);

  std::string out;
  size_t i = 0;
  const uint64_t end = script_end(s);
  unsigned long allocs0 = gr_alloc_calls;
  while (g.block.frame_pos < end) {
    // Core 0's side: queue what is due by the end of the next block.
    uint64_t b0 = g.block.frame_pos;
    for (; i < s.size() && s[i].frame < b0 + kFrames; ++i) {
      gr_qevent e = {};
      e.abs_frame = quantize ? b0 : s[i].frame;
      gr_ump_from_midi1(s[i].b, s[i].len, 0, 1, e.ump);
      if (!gr_spsc_push(&in_q, &e)) EXPECT(!"input queue full");
    }
    gr_graph_run(&g);
    gr_qevent e;
    while (gr_spsc_pop(&out_q, &e)) {
      uint8_t m1[3];
      int len = gr_ump_to_midi1(e.ump, m1);
      out += line(e.abs_frame, m1, len);
    }
  }
  if (allocs_while_running) *allocs_while_running = gr_alloc_calls - allocs0;
  if (timing) *timing = *gr_graph_timing(&g, b);
  EXPECT(fx.truncated == 0);
  EXPECT(g.ev_dropped_total == 0);
  gr_schwung_destroy(&fx);
  return out;
}

static std::string read_file(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static void write_file(const std::string &path, const std::string &s) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) {
    printf("cannot write %s\n", path.c_str());
    exit(2);
  }
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}

struct Note {
  uint64_t frame;
  int note;
};
static std::vector<Note> note_ons(const std::string &rec) {
  std::vector<Note> v;
  const char *p = rec.c_str();
  while (*p) {
    unsigned long long fr;
    unsigned st = 0, d1 = 0, d2 = 0;
    int n = sscanf(p, "%llu %x %x %x", &fr, &st, &d1, &d2);
    if (n == 4 && (st & 0xF0) == 0x90 && d2 > 0) v.push_back({fr, (int)d1});
    const char *nl = strchr(p, '\n');
    if (!nl) break;
    p = nl + 1;
  }
  return v;
}

static int compare(const char *what, const std::string &got, const std::string &want) {
  ++checks;
  if (got == want) return 1;
  ++failures;
  size_t i = 0;
  while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
  size_t ls = want.rfind('\n', i);
  ls = ls == std::string::npos ? 0 : ls + 1;
  printf("FAIL %s: differs at byte %zu (got %zu bytes, want %zu)\n  got:  %.60s\n  want: %.60s\n", what,
         i, got.size(), want.size(), got.c_str() + (ls < got.size() ? ls : got.size()),
         want.c_str() + ls);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: %s <golden dir> [--record]\n", argv[0]);
    return 2;
  }
  std::string dir = argv[1];
  bool record = argc > 2 && strcmp(argv[2], "--record") == 0;

  std::vector<Module> mods = {
      {"eucalypso", eucalypso_midi_fx_init, {{"sync", "clock"}}},
      {"superarp", superarp_midi_fx_init, {{"sync", "clock"}}},
  };
  Script s = clocked_chord(4);

  for (auto &m : mods) {
    std::string ref = run_ref(m, s);
    unsigned long allocs = 99;
    gr_timing t;
    std::string block = run_graph(m, s, true, kFrames, &allocs, &t);
    std::string exact = run_graph(m, s, false, 1, nullptr, nullptr);
    std::string exact2 = run_graph(m, s, false, 1, nullptr, nullptr);
    std::string ref_path = dir + "/" + m.name + "_clock_ref.txt";
    std::string exact_path = dir + "/" + m.name + "_clock_exact.txt";
    if (record) {
      write_file(ref_path, ref);
      write_file(exact_path, exact);
      printf("recorded %s (%zu lines) and %s\n", ref_path.c_str(), note_ons(ref).size(), exact_path.c_str());
    }
    std::string what = std::string(m.name) + " ";
    compare((what + "ref vs recording").c_str(), ref, read_file(ref_path));
    compare((what + "graph (block) vs ref").c_str(), block, ref);
    compare((what + "graph (exact) vs recording").c_str(), exact, read_file(exact_path));
    compare((what + "graph (exact) repeatable").c_str(), exact, exact2);
    ++checks;
    if (allocs != 0) {
      ++failures;
      printf("FAIL %s allocated %lu times while running\n", m.name, allocs);
    }

    std::vector<Note> on_ref = note_ons(ref), on_exact = note_ons(exact);
    EXPECT(on_ref.size() > 8);
    EXPECT(on_ref.size() == on_exact.size());
    // Exact timing: every step on a clock tick (a multiple of 1,000 frames).
    int on_clock = 1, in_block = 1;
    for (auto &n : on_exact) on_clock &= n.frame % kClockFrames == 0;
    // Block timing: the same steps, never more than one block away.
    for (size_t k = 0; k < on_ref.size() && k < on_exact.size(); ++k) {
      long long d = (long long)on_exact[k].frame - (long long)on_ref[k].frame;
      in_block &= d >= 0 && d < (long long)kFrames && on_exact[k].note == on_ref[k].note;
    }
    EXPECT(on_clock);
    EXPECT(in_block);
    printf("%s: %zu note ons; first at frames %llu (exact) / %llu (block); graph node %.2f us/block mean, %.2f max (quantum 64)\n",
           m.name, on_exact.size(), on_exact.empty() ? 0ull : (unsigned long long)on_exact[0].frame,
           on_ref.empty() ? 0ull : (unsigned long long)on_ref[0].frame,
           t.blocks ? (double)t.total / t.blocks / 1000.0 : 0.0, t.max / 1000.0);

    if (strcmp(m.name, "eucalypso") == 0) {
      // Default lane: E(4,16) at 1/16 = one hit per quarter (24 clocks).
      // Upstream behaviour, pinned here so a change shows: eucalypso fires
      // step 0 on Start and counts the downbeat's own clock as tick 1, so
      // its second hit comes 23 clocks in, then every 24 (17 hits in 4 bars).
      // The ref run (no shim) shows the same, so it is the module, not the shim.
      int quarters = on_exact.size() > 2 && on_exact[1].frame - on_exact[0].frame == 23 * kClockFrames;
      for (size_t k = 2; k < on_exact.size(); ++k)
        quarters &= on_exact[k].frame - on_exact[k - 1].frame == 24 * kClockFrames;
      EXPECT(quarters);
      EXPECT(on_exact.size() == 17);
      int all_c = 1;
      for (auto &n : on_exact) all_c &= n.note == 60;  // lane note 1 = the lowest held note
      EXPECT(all_c);
    } else {
      // Up arp at 1/16 (6 clocks) over C-E-G.
      int sixteenths = 1, up = 1;
      const int want[3] = {60, 64, 67};
      for (size_t k = 1; k < on_exact.size(); ++k)
        sixteenths &= on_exact[k].frame - on_exact[k - 1].frame == 6 * kClockFrames;
      for (size_t k = 0; k < on_exact.size(); ++k) up &= on_exact[k].note == want[k % 3];
      EXPECT(sixteenths);
      EXPECT(up);
    }
  }
  // Positive control: the counter sees the modules' create_instance calloc.
  EXPECT(gr_alloc_calls > 0);

  // Internal sync at 120 BPM: no clock in, the module keeps its own time.
  // Exact mode (tick per frame) must land the arp's 1/16 steps 6,000 frames apart.
  {
    Module arp = {"superarp", superarp_midi_fx_init, {{"sync", "internal"}, {"bpm", "120"}}};
    Script in;
    in.push_back({0, {0xFA, 0, 0}, 1});
    in.push_back({0, {0x90, 60, 100}, 3});
    in.push_back({0, {0x90, 64, 100}, 3});
    in.push_back({191500, {0x80, 60, 0}, 3});
    in.push_back({191500, {0x80, 64, 0}, 3});
    in.push_back({192000, {0xFC, 0, 0}, 1});
    std::string a = run_graph(arp, in, false, 1, nullptr, nullptr);
    std::string b = run_graph(arp, in, false, 1, nullptr, nullptr);
    std::string q = run_graph(arp, in, false, 64, nullptr, nullptr);
    // Upstream behaviour, pinned: the first held note steps at once, and the
    // second (same frame) steps again, so the first two note ons share frame 0.
    // The ref run (no shim) does the same.
    std::string r = run_ref(arp, in);
    compare("superarp internal start vs ref", a.substr(0, 30), r.substr(0, 30));
    std::string path = dir + "/superarp_internal_exact.txt";
    if (record) write_file(path, a);
    compare("superarp internal repeatable", a, b);
    compare("superarp internal vs recording", a, read_file(path));
    std::vector<Note> on = note_ons(a), onq = note_ons(q);
    int steps = on.size() >= 16 && on[1].frame == 0;
    for (size_t k = 2; k < on.size(); ++k) {
      long long d = (long long)(on[k].frame - on[k - 1].frame);
      steps &= d >= 5999 && d <= 6001;  // 6,000 frames; the module's own rounding allows 1
    }
    EXPECT(steps);
    EXPECT(on.size() == onq.size());
    long long worst = 0;
    for (size_t k = 0; k < on.size() && k < onq.size(); ++k) {
      long long d = (long long)onq[k].frame - (long long)on[k].frame;
      if (d < 0) d = -d;
      if (d > worst) worst = d;
    }
    EXPECT(worst < 64);
    printf("superarp internal: %zu note ons, first gaps %lld %lld; block-tick worst offset %lld frames\n",
           on.size(), on.size() > 1 ? (long long)(on[1].frame - on[0].frame) : -1,
           on.size() > 2 ? (long long)(on[2].frame - on[1].frame) : -1, worst);
  }
  printf("test_graph_schwung: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
