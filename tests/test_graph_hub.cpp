// Host tests for lib/graph's hub runtime (gr_hub): the C1-2 use cases as
// hub apps, the microsecond <-> frame clock, the rings at the edges and the
// stats snapshot. Run with `make test` (or `make test-graph`).
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "gr_hub.h"

static int failures = 0;
static int checks = 0;

#define EXPECT(cond)                                         \
  do {                                                       \
    ++checks;                                                \
    if (!(cond)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                            \
    }                                                        \
  } while (0)
#define EXPECT_EQ(got, want)                                                          \
  do {                                                                                \
    ++checks;                                                                         \
    long long g_ = (long long)(got), w_ = (long long)(want);                          \
    if (g_ != w_) {                                                                   \
      printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #got, g_, w_); \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

static gr_hub hub;  // static: ~110 KB

static uint32_t rnd_state = 1;
static uint32_t rnd() {
  rnd_state = rnd_state * 1664525u + 1013904223u;
  return rnd_state >> 8;
}

struct Got {
  uint32_t t_us;
  uint8_t msg[3];
  int len;
};

// A simulated night: core 1 wakes every ~1 ms (with jitter) and runs what is
// due; core 0 pushes `script` inputs as their time comes and drains outputs.
struct Sim {
  uint32_t now;
  Got out[2][20000];
  int n_out[2];
};
static Sim sim;

static void drain(uint32_t now) {
  for (int o = 0; o < hub.n_out; ++o) {
    uint8_t m[3];
    int len;
    while (gr_hub_out_midi1(&hub, o, now, m, &len)) {
      Got &g = sim.out[o][sim.n_out[o]++ % 20000];
      g.t_us = now;
      memcpy(g.msg, m, 3);
      g.len = len;
    }
  }
}

// Advances the simulation to `until`, in steps of 1 ms +- jitter_us (core 1)
// and 50 us (core 0's drain).
static void run_until(uint32_t until, uint32_t jitter_us) {
  uint32_t next_block = sim.now;
  while ((int32_t)(until - sim.now) > 0) {
    sim.now += 50;
    if (gr_us_reached(sim.now, next_block)) {
      uint32_t f;
      while ((f = gr_hub_frames_due(&hub, sim.now)) != 0) gr_hub_run(&hub, f, sim.now);
      next_block = sim.now + 1000 - jitter_us / 2 + (jitter_us ? rnd() % jitter_us : 0);
    }
    drain(sim.now);
  }
}

static void start(gr_hub_app app, uint32_t t0) {
  gr_hub_config c;
  gr_hub_config_default(&c, app);
  EXPECT_EQ(gr_hub_init(&hub, &c), GR_OK);
  memset(&sim, 0, sizeof(sim));
  sim.now = t0;
  gr_hub_run(&hub, gr_hub_frames_due(&hub, t0), t0);  // anchors the clock
}

// THRU: every message arrives, in order, a constant latency after it went in
// (one block in, 48 frames = 1 ms, plus 2 ms out; within one frame, 21 us).
static void test_thru(uint32_t t0, const char *label) {
  start(GR_HUB_APP_THRU, t0);
  const int N = 5000;
  uint32_t sent_at[N];
  int sent = 0;
  for (int i = 0; i < N; ++i) {
    run_until(sim.now + 100 + rnd() % 900, 400);
    uint8_t m[3] = {(uint8_t)(i & 1 ? 0x80 : 0x90), (uint8_t)(36 + (i / 2) % 48), 100};
    sent_at[i] = sim.now;
    sent += gr_hub_in_midi1(&hub, 0, sim.now, m, 3);
  }
  run_until(sim.now + 10000, 400);
  EXPECT_EQ(sent, N);
  EXPECT_EQ(sim.n_out[0], N);
  int order = 1;
  int32_t lat_min = 1 << 30, lat_max = -(1 << 30);
  for (int i = 0; i < N && i < sim.n_out[0]; ++i) {
    uint8_t want = (uint8_t)(36 + (i / 2) % 48);
    order &= sim.out[0][i].msg[1] == want;
    int32_t lat = (int32_t)(sim.out[0][i].t_us - sent_at[i]);
    if (lat < lat_min) lat_min = lat;
    if (lat > lat_max) lat_max = lat;
  }
  EXPECT(order);
  // 1 ms in + 2 ms out. Due times are exact to a frame (21 us); the drain
  // polls every 50 us, so the wire sees +0..50.
  EXPECT(lat_min >= 3000 - 21);
  EXPECT(lat_max <= 3000 + 50);
  gr_hub_stats s;
  EXPECT(gr_hub_stats_take(&hub, &s));
  EXPECT_EQ(s.late, 0);
  EXPECT_EQ(s.in_dropped + s.out_dropped + s.ev_dropped, 0);
  EXPECT_EQ(s.resyncs, 0);
  printf("hub thru (%s): %d of %d through, latency %d..%d us (3000 set), late %u\n", label,
         sim.n_out[0], N, lat_min, lat_max, s.late);
}

static void test_merge() {
  start(GR_HUB_APP_MERGE, 1000);
  int sent = 0;
  for (int i = 0; i < 2000; ++i) {
    run_until(sim.now + 200 + rnd() % 300, 400);
    uint8_t a[3] = {0xB0, 1, (uint8_t)(i & 0x7F)}, b[3] = {0xB1, 2, (uint8_t)(i & 0x7F)};
    sent += gr_hub_in_midi1(&hub, 0, sim.now, a, 3);
    sent += gr_hub_in_midi1(&hub, 1, sim.now + 10, b, 3);
  }
  run_until(sim.now + 10000, 400);
  EXPECT_EQ(sent, 4000);
  EXPECT_EQ(sim.n_out[0], 4000);
  int order = 1, na = 0, nb = 0;
  for (int i = 0; i < sim.n_out[0]; ++i) {
    // same time order as sent: a then b, pair by pair
    if (sim.out[0][i].msg[0] == 0xB0) order &= sim.out[0][i].msg[2] == (uint8_t)(na++ & 0x7F);
    else order &= sim.out[0][i].msg[2] == (uint8_t)(nb++ & 0x7F) && nb <= na;
  }
  EXPECT(order);
  EXPECT_EQ(na, 2000);
}

static void test_split_layer() {
  start(GR_HUB_APP_SPLIT, 5000);
  for (int n = 40; n < 80; ++n) {
    run_until(sim.now + 700, 400);
    uint8_t on[3] = {0x90, (uint8_t)n, 90}, off[3] = {0x80, (uint8_t)n, 0};
    gr_hub_in_midi1(&hub, 0, sim.now, on, 3);
    gr_hub_in_midi1(&hub, 0, sim.now + 300, off, 3);
  }
  run_until(sim.now + 10000, 400);
  int lo_ok = 1, hi_ok = 1;
  for (int i = 0; i < sim.n_out[0]; ++i) lo_ok &= sim.out[0][i].msg[1] < 60 && (sim.out[0][i].msg[0] & 15) == 0;
  for (int i = 0; i < sim.n_out[1]; ++i) hi_ok &= sim.out[1][i].msg[1] >= 60 && (sim.out[1][i].msg[0] & 15) == 1;
  EXPECT_EQ(sim.n_out[0], 40);
  EXPECT_EQ(sim.n_out[1], 40);
  EXPECT(lo_ok);
  EXPECT(hi_ok);

  start(GR_HUB_APP_LAYER, 5000);
  uint8_t on[3] = {0x90, 60, 90}, off[3] = {0x80, 60, 0};
  gr_hub_in_midi1(&hub, 0, sim.now + 100, on, 3);
  gr_hub_in_midi1(&hub, 0, sim.now + 900, off, 3);
  run_until(sim.now + 10000, 400);
  EXPECT_EQ(sim.n_out[0], 2);
  EXPECT_EQ(sim.n_out[1], 2);
  EXPECT_EQ(sim.out[0][0].msg[1], 60);
  EXPECT_EQ(sim.out[1][0].msg[1], 72);
  EXPECT_EQ(sim.out[1][0].msg[0], 0x91);
  EXPECT_EQ(sim.out[1][1].msg[0], 0x81);
}

// The box keeps time: 120 bpm is a clock every 20833.3 us; over 60 s the
// clocks leave within one frame of that grid, with block jitter of +-400 us.
static void test_clock() {
  start(GR_HUB_APP_CLOCK, 0xFFFF0000u);  // and across the 32-bit us wrap
  const uint32_t t0 = sim.now;
  run_until(t0 + 60u * 1000000u, 800);
  int clocks = 0, starts = 0;
  double worst = 0;
  uint32_t first = 0;
  for (int i = 0; i < sim.n_out[1] && i < 20000; ++i) {
    if (sim.out[1][i].msg[0] == 0xFA) ++starts;
    if (sim.out[1][i].msg[0] != 0xF8) continue;
    if (!clocks) first = sim.out[1][i].t_us;
    double ideal = clocks * (1000000.0 / 48.0);  // 48 clocks a second at 120 bpm
    double err = (double)(int32_t)(sim.out[1][i].t_us - first) - ideal;
    if (err < 0) err = -err;
    if (err > worst) worst = err;
    ++clocks;
  }
  EXPECT_EQ(starts, 1);
  EXPECT(clocks >= 2879 && clocks <= 2881);
  EXPECT(worst <= 21.0 + 50.0);  // one frame, plus the drain's 50 us poll
  gr_hub_stats s;
  EXPECT(gr_hub_stats_take(&hub, &s));
  EXPECT_EQ(s.resyncs, 0);
  EXPECT_EQ(s.out_dropped, 0);
  printf("hub clock: %d clocks in 60 s across the us wrap, worst %.1f us off the 120 bpm grid\n", clocks,
         worst);
}

// A 300 ms stall on core 1: the clock re-anchors once; nothing is lost, the
// stalled inputs count as late.
static void test_stall_and_full() {
  start(GR_HUB_APP_THRU, 0);
  run_until(sim.now + 5000, 0);
  for (int i = 0; i < 10; ++i) {
    uint8_t m[3] = {0x90, 60, 100};
    gr_hub_in_midi1(&hub, 0, sim.now + (uint32_t)i * 1000, m, 3);
  }
  sim.now += 300000;  // core 1 asleep
  run_until(sim.now + 200000, 0);
  gr_hub_stats s;
  EXPECT(gr_hub_stats_take(&hub, &s));
  EXPECT_EQ(sim.n_out[0], 10);
  EXPECT_EQ(s.resyncs, 1);
  EXPECT_EQ(s.late, 10);

  // In-ring full: core 0 never waits, the push is refused and counted.
  start(GR_HUB_APP_THRU, 0);
  int ok = 0;
  for (int i = 0; i < GR_HUB_RING + 44; ++i) {
    uint8_t m[3] = {0xB0, 7, 1};
    ok += gr_hub_in_midi1(&hub, 0, 0, m, 3);
  }
  EXPECT_EQ(ok, GR_HUB_RING);
  EXPECT_EQ(hub.in_q[0].dropped, 44);
  run_until(sim.now + 200000, 0);
  EXPECT(gr_hub_stats_take(&hub, &s));
  EXPECT_EQ(s.in_dropped, 44);
  EXPECT_EQ(s.n_nodes, 2);
  EXPECT(strcmp(s.node[0].name, "tsource") == 0);
  EXPECT(s.blocks > 0);
}

// Three real threads, as on the board: core 0's producer, core 1's graph and
// core 0's drain, on the host's microsecond clock. Run under TSan by
// `make graph-tsan`.
static void test_threads() {
  gr_hub_config c;
  gr_hub_config_default(&c, GR_HUB_APP_THRU);
  c.latency_frames = 480;  // 10 ms: a desktop scheduler is no RTOS
  EXPECT_EQ(gr_hub_init(&hub, &c), GR_OK);
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  auto now_us = [&]() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t0).count();
  };
  const int N = 20000;
  std::atomic<int> done{0};
  std::atomic<int> got{0};
  std::thread core1([&]() {
    while (done.load(std::memory_order_acquire) < 2) {
      uint32_t t = now_us(), f;
      while ((f = gr_hub_frames_due(&hub, t)) != 0) gr_hub_run(&hub, f, t);
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  });
  std::thread drain_t([&]() {
    int n = 0, order = 1, expect = 0;
    while (n < N && now_us() < 20000000u) {  // 20 s: a lost event fails, never hangs
      uint8_t m[3];
      int len;
      while (gr_hub_out_midi1(&hub, 0, now_us(), m, &len)) {
        order &= m[2] == (uint8_t)(1 + expect++ % 127);
        ++n;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      if (n >= N) break;
    }
    got.store(order ? n : -n);
  });
  int sent = 0;
  for (int i = 0; i < N; ++i) {
    uint8_t m[3] = {0xB0, 1, (uint8_t)(1 + i % 127)};
    while (!gr_hub_in_midi1(&hub, 0, now_us(), m, 3)) std::this_thread::yield();  // test only
    ++sent;
    if (i % 16 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  done.store(1, std::memory_order_release);
  drain_t.join();
  done.store(2, std::memory_order_release);
  core1.join();
  EXPECT_EQ(sent, N);
  EXPECT_EQ(got.load(), N);
  printf("hub threads: %d of %d through three threads, in order\n", got.load(), N);
}

int main() {
  test_thru(1000, "from 0");
  test_thru(0xFFFFFFFFu - 2000000u, "across the us wrap");
  test_merge();
  test_split_layer();
  test_clock();
  test_stall_and_full();
  test_threads();
  printf("test_graph_hub: %d checks, %d failures; sizeof(gr_hub) = %zu bytes\n", checks, failures,
         sizeof(gr_hub));
  return failures ? 1 : 0;
}
