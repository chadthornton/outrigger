// Host-side tests for lib/web_core: the diagnostics log, the route table (read
// only), the request guard and the setup page's JSON fields. Run with `make test`.
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "hotspot_channel.h"
#include "json_field.h"
#include "log_ring.h"
#include "request_guard.h"
#include "routes.h"

using namespace web_core;

static int failures = 0;

#define EXPECT(cond, what)                                   \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s (line %d)\n", what, __LINE__);         \
      ++failures;                                            \
    }                                                        \
  } while (0)

// ---------- LineRing: one producer, one consumer ----------
static void test_ring_order_and_drop() {
  LineRing<4> ring;
  LogLine l;
  EXPECT(!ring.peek(l), "empty ring has nothing");
  for (uint32_t i = 0; i < 6; ++i) {
    char text[8];
    snprintf(text, sizeof text, "n%u", i);
    ring.push(100 + i, text);
  }
  // Full at 4: the newest two are refused, never the oldest overwritten (the
  // producer must not touch a slot the consumer may be reading).
  EXPECT(ring.dropped() == 2, "two refused when full");
  EXPECT(ring.peek(l) && l.ms == 100 && strcmp(l.text, "n0") == 0, "oldest first");
  int n = 0;
  while (ring.pop(l)) ++n;
  EXPECT(n == 4, "four kept");
  EXPECT(strcmp(l.text, "n3") == 0, "last kept is n3");
  ring.push(200, "after");
  EXPECT(ring.pop(l) && l.ms == 200, "room again after draining");
}

static void test_ring_truncates() {
  LineRing<2> ring;
  std::string long_text(500, 'x');
  ring.push(1, long_text.c_str());
  LogLine l;
  EXPECT(ring.pop(l), "long line kept");
  EXPECT(strlen(l.text) == kLogText - 1, "cut to the slot");
}

static void test_ring_threads() {
  // A real producer thread against a consumer: every line arrives once, in
  // order, or is counted as dropped.
  static LineRing<16> ring;
  constexpr uint32_t kCount = 200000;
  std::atomic<bool> done{false};
  std::thread producer([&] {
    char text[16];
    for (uint32_t i = 0; i < kCount; ++i) {
      snprintf(text, sizeof text, "%u", i);
      ring.push(i, text);
    }
    done = true;
  });
  uint32_t got = 0, last = 0;
  bool ordered = true, matches = true, first = true;
  LogLine l;
  while (!done.load() || ring.peek(l)) {
    if (!ring.pop(l)) continue;
    if (!first && l.ms <= last) ordered = false;
    if (strtoul(l.text, nullptr, 10) != l.ms) matches = false;
    first = false;
    last = l.ms;
    ++got;
  }
  producer.join();
  EXPECT(ordered, "threaded: in order");
  EXPECT(matches, "threaded: no torn lines");
  EXPECT(got + ring.dropped() == kCount, "threaded: kept + dropped = pushed");
}

// ---------- LogHistory: what /log serves ----------
static void test_history_since() {
  LogHistory<4> h;
  EXPECT(h.next() == 1, "first sequence number is 1");
  for (uint32_t i = 0; i < 3; ++i) h.add({i, 'L', "x"});
  std::vector<uint32_t> seqs;
  uint32_t lost = 99;
  h.read(0, &lost, [&](uint32_t seq, const LogLine &) { seqs.push_back(seq); });
  EXPECT(seqs.size() == 3 && seqs[0] == 1 && seqs[2] == 3, "since 0 returns all");
  EXPECT(lost == 0, "nothing lost");
  seqs.clear();
  h.read(2, &lost, [&](uint32_t seq, const LogLine &) { seqs.push_back(seq); });
  EXPECT(seqs.size() == 1 && seqs[0] == 3, "since 2 returns 3");
  seqs.clear();
  h.read(3, &lost, [&](uint32_t seq, const LogLine &) { seqs.push_back(seq); });
  EXPECT(seqs.empty(), "caught up");
}

static void test_history_evicts() {
  LogHistory<4> h;
  for (uint32_t i = 0; i < 10; ++i) h.add({i, 'U', "x"});  // seq 1..10, keeps 7..10
  std::vector<uint32_t> seqs;
  uint32_t lost = 0;
  h.read(2, &lost, [&](uint32_t seq, const LogLine &l) {
    seqs.push_back(seq);
    EXPECT(l.ms == seq - 1, "line matches its sequence");
  });
  EXPECT(seqs.size() == 4 && seqs[0] == 7, "a reader that fell behind gets the oldest kept");
  EXPECT(lost == 4, "and is told 3..6 are gone");
  seqs.clear();
  h.read(50, &lost, [&](uint32_t seq, const LogLine &) { seqs.push_back(seq); });
  EXPECT(seqs.size() == 4 && lost == 0, "a cursor from a previous boot starts over");
}

// Lines from several rings come out in time order.
static void test_drain_merges() {
  LineRing<8> usb, loader;
  LogHistory<16> h;
  usb.push(10, "u10");
  usb.push(30, "u30");
  loader.push(20, "l20");
  loader.push(40, "l40");
  LineRingRef rings[] = {{&usb, 'U'}, {&loader, 'L'}};
  drain(rings, 2, h);
  std::string order;
  uint32_t lost;
  h.read(0, &lost, [&](uint32_t, const LogLine &l) { order += l.text; order += l.source; order += ' '; });
  EXPECT(order == "u10U l20L u30U l40L ", "merged by time, source kept");
}

// ---------- Routes: nothing writes ----------
static void test_routes_read_only() {
  int posts = 0;
  for (const Route &r : kRoutes) {
    if (r.method == Method::Get) continue;
    ++posts;
    EXPECT(r.hotspot_only, "a non-GET route answers only on the setup hotspot");
    EXPECT(strcmp(r.uri, "/setup/join") == 0, "the only non-GET route is the hotspot's join");
  }
  EXPECT(posts == 1, "exactly one non-GET route");
  const char *const kWanted[] = {"/", "/status", "/log", "/crash", "/coredump", "/store"};
  for (const char *w : kWanted) {
    bool found = false;
    for (const Route &r : kRoutes) found = found || (strcmp(r.uri, w) == 0 && r.method == Method::Get);
    EXPECT(found, w);
  }
}

// ---------- Request guard (from the spike) ----------
static void test_host_guard() {
  const HostNames names = {"fm1hub.local", "192.168.1.20", ""};
  EXPECT(host_allowed("fm1hub.local", names), "mdns name");
  EXPECT(host_allowed("FM1HUB.local.:80", names), "case, dot, port");
  EXPECT(host_allowed("192.168.1.20", names), "own ip");
  EXPECT(!host_allowed("evil.example", names), "foreign host");
  EXPECT(!host_allowed("", names), "empty host");
  EXPECT(!host_allowed("192.168.1.2", names), "prefix of the ip");
}

static void test_json_fields() {
  char s[16];
  EXPECT(json_string("{\"ssid\":\"a\\\"b\",\"pass\":\"x\"}", "ssid", s, sizeof s) && strcmp(s, "a\"b") == 0,
         "escaped string");
  EXPECT(!json_string("{\"ssid\":\"0123456789abcdefg\"}", "ssid", s, sizeof s), "too long refused");
  char e[16];
  json_escape("a\"b\n", e, sizeof e);
  EXPECT(strcmp(e, "a\\\"b\\u000a") == 0, "escape");
}

static void test_hotspot_channel() {
  const ScanEntry nets[] = {{"home", -60, 11}, {"loud", -40, 1}};
  EXPECT(pick_hotspot_channel(nets, 2, "home") == 11, "saved network's channel");
  EXPECT(pick_hotspot_channel(nets, 2, "") == 1, "else the strongest");
  EXPECT(pick_hotspot_channel(nets, 0, "") == kDefaultHotspotChannel, "else the default");
}

int main() {
  test_ring_order_and_drop();
  test_ring_truncates();
  test_ring_threads();
  test_history_since();
  test_history_evicts();
  test_drain_merges();
  test_routes_read_only();
  test_host_guard();
  test_json_fields();
  test_hotspot_channel();
  if (failures) {
    printf("web_core: %d failure(s)\n", failures);
    return 1;
  }
  printf("web_core: all passed\n");
  return 0;
}
