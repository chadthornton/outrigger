// The two-flow rules checked in code (CONTRACT.md, "Two flows"): built with
// -DGR_CHECK_FLOWS, so every ring knows its one writer and one reader.
// `make test` runs it; `make graph-tsan` runs it again under ThreadSanitizer,
// which proves the release/acquire handovers (and fails a relaxed one).
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <thread>

#include "gr_spsc.h"
#include "gr_swap.h"

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

struct Msg {
  uint32_t seq;
  uint32_t payload[7];  // seq, repeated: a torn element shows
};

// Rule 2: one writer, one reader, release/acquire. 0 lost, 0 torn, 0 violations.
static void test_one_writer() {
  static Msg buf[256];
  static gr_spsc q;
  gr_spsc_init(&q, buf, 256, sizeof(Msg));
  const uint32_t N = 500000;
  std::thread prod([&]() {
    for (uint32_t i = 0; i < N;) {
      Msg m;
      m.seq = i;
      for (int k = 0; k < 7; ++k) m.payload[k] = i;
      if (gr_spsc_push(&q, &m)) ++i;  // test only: a real producer never retries in a loop
    }
  });
  uint32_t next = 0, torn = 0;
  while (next < N) {
    Msg m;
    if (!gr_spsc_pop(&q, &m)) continue;
    if (m.seq != next) ++torn;
    for (int k = 0; k < 7; ++k) torn += m.payload[k] != m.seq;
    ++next;
  }
  prod.join();
  EXPECT_EQ(torn, 0);
  EXPECT_EQ(q.producer_violations, 0);
  EXPECT_EQ(q.consumer_violations, 0);
  printf("flows: %u messages, one writer and one reader: 0 torn, 0 owner violations\n", N);
}

// A second producer (or consumer) on one ring is caught.
static void test_second_writer_caught() {
  static Msg buf[64];
  static gr_spsc q;
  gr_spsc_init(&q, buf, 64, sizeof(Msg));
  Msg m = {};
  gr_spsc_push(&q, &m);                                // this thread owns the producer side
  std::thread other([&]() { gr_spsc_push(&q, &m); });  // a second writer
  other.join();
  EXPECT_EQ(q.producer_violations, 1);
  EXPECT_EQ(q.consumer_violations, 0);
  std::thread reader([&]() { gr_spsc_pop(&q, &m); });  // consumer: that thread
  reader.join();
  gr_spsc_pop(&q, &m);  // and now this one too
  EXPECT_EQ(q.consumer_violations, 1);
}

// The producer never waits: pushing into a full ring returns at once and counts.
static void test_full_returns() {
  static Msg buf[4];
  static gr_spsc q;
  gr_spsc_init(&q, buf, 4, sizeof(Msg));
  Msg m = {};
  int ok = 0;
  for (int i = 0; i < 10; ++i) ok += gr_spsc_push(&q, &m);
  EXPECT_EQ(ok, 4);
  EXPECT_EQ(q.dropped, 6);
}

// Rule 3: big state as a swap with acknowledge. The audio flow never sees a
// half-written table; the control flow never waits (it skips while busy).
struct Table {
  uint32_t gen;
  uint32_t v[256];
};
static void test_swap() {
  static Table a, b;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  for (int i = 0; i < 256; ++i) a.v[i] = (uint32_t)i;  // generation 0, in force from the start
  static gr_swap s;
  gr_swap_init(&s, &a, &b);
  std::atomic<int> stop{0};
  uint32_t published = 0, busy = 0;
  std::thread control([&]() {
    for (uint32_t gen = 1; gen <= 20000;) {
      Table *t = (Table *)gr_swap_back(&s);
      if (!t) {
        ++busy;
        std::this_thread::yield();
        continue;
      }
      t->gen = gen;
      for (int i = 0; i < 256; ++i) t->v[i] = gen * 7 + (uint32_t)i;
      published += (uint32_t)gr_swap_publish(&s);
      ++gen;
    }
    stop.store(1, std::memory_order_release);
  });
  uint32_t torn = 0, backwards = 0, last = 0, takes = 0;
  for (;;) {
    int done = stop.load(std::memory_order_acquire);
    const Table *t = (const Table *)gr_swap_take(&s);
    for (int i = 0; i < 256; ++i) torn += t->v[i] != t->gen * 7 + (uint32_t)i;
    if (t->gen < last) ++backwards;
    last = t->gen;
    ++takes;
    if (done) break;
  }
  control.join();
  EXPECT_EQ(published, 20000);
  EXPECT_EQ(torn, 0);
  EXPECT_EQ(backwards, 0);
  EXPECT_EQ(last, 20000);
  printf("flows: swap, %u tables published, %u takes, %u torn (control skipped %u times while busy)\n", published,
         takes, torn, busy);
}

int main() {
  test_one_writer();
  test_second_writer_caught();
  test_full_returns();
  test_swap();
  printf("test_graph_flows: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
