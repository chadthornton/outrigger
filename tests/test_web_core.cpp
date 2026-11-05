// Host-side tests for lib/web_core: the diagnostics log, the route table (read
// only), the request guard, the setup page's JSON fields, the Wi-Fi fallback and the hotspot's QR code. Run with `make test`.
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "bench.h"
#include "hotspot_channel.h"
#include "json_field.h"
#include "log_ring.h"
#include "request_guard.h"
#include "routes.h"
#include "wifi_fallback.h"
#include "wifi_qr.h"

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

// ---------- Bench API (BENCH=1 builds only) ----------
static void test_bench_routes_apart() {
  // The release table never gains a bench route; the bench table holds only /bench/.
  for (const Route &r : kRoutes) EXPECT(strncmp(r.uri, "/bench", 6) != 0, "no bench route in the release table");
  for (const Route &r : kBenchRoutes) {
    EXPECT(strncmp(r.uri, "/bench/", 7) == 0, "bench routes live under /bench/");
    EXPECT(!r.hotspot_only, "bench routes answer on the LAN");
  }
}

static void test_bench_mac() {
  // Built against tools/devices.example.json's made-up MACs (Makefile, build/example-boards).
  const uint8_t v1[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  const uint8_t v2[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
  const uint8_t s3[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
  const uint8_t other[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x04};
  EXPECT(waveshare_name(v1) && strcmp(waveshare_name(v1), "small-v1") == 0, "Small-V1 by MAC");
  EXPECT(waveshare_name(v2) && strcmp(waveshare_name(v2), "small-v2") == 0, "Small-V2 by MAC");
  EXPECT(waveshare_name(s3) && strcmp(waveshare_name(s3), "big-s3") == 0, "Big-S3 by MAC");
  EXPECT(!waveshare_name(other), "an unlisted board gets no bench API");
  char name[16];
  web_host_name(v1, false, name, sizeof name);
  EXPECT(strcmp(name, "small-v1") == 0, "Small-V1 answers at small-v1.local");
  web_host_name(s3, true, name, sizeof name);
  EXPECT(strcmp(name, "big-s3") == 0, "a listed board keeps its own name in a bench build");
  web_host_name(other, false, name, sizeof name);
  EXPECT(strcmp(name, "fm1hub") == 0, "anyone else's board is fm1hub");
  web_host_name(other, true, name, sizeof name);
  EXPECT(strcmp(name, "fm1bench") == 0, "an unlisted bench build is fm1bench, never fm1hub");
}

static void test_bench_update() {
  const char *why = "";
  BenchUpdate u;
  const std::string sha(64, 'a');
  const std::string q = "sha256=" + sha + "&label=abc1234";
  EXPECT(parse_bench_update(q.c_str(), 1700000, 3145728, 786432, u, &why), "an app image");
  EXPECT(u.slot == UpdateSlot::App && u.size == 1700000 && u.sha256[0] == 0xaa && u.sha256[31] == 0xaa, "its fields");
  EXPECT(strcmp(u.label, "abc1234") == 0, "its label");
  EXPECT(parse_bench_update((q + "&slot=recovery").c_str(), 343000, 3145728, 786432, u, &why) &&
             u.slot == UpdateSlot::Recovery,
         "a recovery image");
  EXPECT(!parse_bench_update((q + "&slot=recovery").c_str(), 800000, 3145728, 786432, u, &why), "too big for recovery");
  EXPECT(!parse_bench_update(q.c_str(), 3145729, 3145728, 786432, u, &why), "too big for app0");
  EXPECT(!parse_bench_update(q.c_str(), 100, 3145728, 786432, u, &why), "too small to be an image");
  EXPECT(!parse_bench_update((q + "&slot=boot").c_str(), 343000, 3145728, 786432, u, &why), "an unknown slot");
  EXPECT(!parse_bench_update(("sha256=" + sha.substr(1) + "&label=x").c_str(), 1000, 3145728, 786432, u, &why),
         "a short hash");
  EXPECT(!parse_bench_update(("sha256=" + std::string(63, 'a') + "g&label=x").c_str(), 1000, 3145728, 786432, u, &why),
         "a hash that isn't hex");
  EXPECT(!parse_bench_update(("sha256=" + sha).c_str(), 1000, 3145728, 786432, u, &why), "no label");
  EXPECT(!parse_bench_update(("sha256=" + sha + "&label=a%22b").c_str(), 1000, 3145728, 786432, u, &why),
         "a label that would need escaping");
  EXPECT(!parse_bench_update(("sha256=" + sha + "&label=0123456789abcdefg").c_str(), 1000, 3145728, 786432, u, &why),
         "a label too long");
}

static void test_bench_commands() {
  BenchCommand c;
  const char *why = nullptr;
  EXPECT(parse_bench_command("/bench/select", "index=3", c, &why) && c.kind == BenchKind::Select && c.index == 3,
         "select a row");
  EXPECT(parse_bench_command("/bench/act", "kind=switch&index=2", c, &why) && c.kind == BenchKind::Switch &&
             c.index == 2, "switch names its row");
  EXPECT(parse_bench_command("/bench/act", "index=2&kind=switch", c, &why) && c.kind == BenchKind::Switch,
         "any order");
  EXPECT(parse_bench_command("/bench/act", "kind=backup", c, &why) && c.kind == BenchKind::Backup, "backup");
  EXPECT(parse_bench_command("/bench/act", "kind=retry", c, &why) && c.kind == BenchKind::Retry, "retry");
  EXPECT(!parse_bench_command("/bench/act", "kind=switch", c, &why) && why, "switch without a row");
  EXPECT(!parse_bench_command("/bench/act", "kind=erase", c, &why), "unknown kind");
  EXPECT(!parse_bench_command("/bench/select", "index=-1", c, &why), "negative row");
  EXPECT(!parse_bench_command("/bench/select", "index=3x", c, &why), "junk after the row");
  EXPECT(!parse_bench_command("/bench/select", "", c, &why), "no row");
  EXPECT(!parse_bench_command("/bench/select", nullptr, c, &why), "no query");
  EXPECT(!parse_bench_command("/bench/act", "kindx=backup", c, &why), "a longer key isn't kind");
  EXPECT(!parse_bench_command("/status", "kind=backup", c, &why), "not a bench path");
  EXPECT(c.kind == BenchKind::None, "a refused command names nothing");
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

// ---------- The setup hotspot's Wi-Fi QR code ----------
static void test_wifi_qr() {
  char out[64];
  EXPECT(wifi_qr_payload("small-v1-setup", "abcd2345", out, sizeof out) &&
             strcmp(out, "WIFI:T:WPA;S:small-v1-setup;P:abcd2345;;") == 0,
         "the board's hotspot, as is");
  EXPECT(wifi_qr_payload("a;b,c", "p:q\\r\"s", out, sizeof out) &&
             strcmp(out, "WIFI:T:WPA;S:a\\;b\\,c;P:p\\:q\\\\r\\\"s;;") == 0,
         "\\ ; , : \" escaped in name and password");
  const char *exact = "WIFI:T:WPA;S:ab;P:12345678;;";  // 28 characters
  EXPECT(wifi_qr_payload("ab", "12345678", out, strlen(exact) + 1) && strcmp(out, exact) == 0, "fits exactly");
  EXPECT(!wifi_qr_payload("ab", "12345678", out, strlen(exact)) && out[0] == 0, "one byte short: empty");
  EXPECT(!wifi_qr_payload("a;", "12345678", out, strlen(exact) + 1) && out[0] == 0, "an escape that doesn't fit");
}

// ---------- WifiFallback: the saved network after a boot ----------
using WF = WifiFallback;

struct Action {
  uint32_t ms;
  WF::Do what;
};

// Ticks every 100 ms over [from, to) with the same view of the station.
static std::vector<Action> run(WF &f, uint32_t from, uint32_t to, const WF::Seen &s, bool nudge_ok = true) {
  std::vector<Action> out;
  for (uint32_t t = from; t < to; t += 100) {
    const WF::Do d = f.tick(t, s);
    if (d == WF::Do::Nudge) f.nudge_result(t, nudge_ok);
    if (d != WF::Do::Nothing) out.push_back({t, d});
  }
  return out;
}

static const WF::Seen kStalled = {true, false, true, false, 0, false};
static const WF::Seen kAssociated = {true, false, false, false, 0, false};
static const WF::Seen kOnline = {true, true, false, false, 0, false};

static void test_fallback_stalled_boot() {
  WF f;
  f.boot_join(1000);
  auto a = run(f, 1000, 61000, kStalled);
  EXPECT(a.size() == 2, "two nudges before the hotspot");
  EXPECT(a.size() > 0 && a[0].ms == 21000 && a[0].what == WF::Do::Nudge, "first nudge 20 s after the join");
  EXPECT(a.size() > 1 && a[1].ms == 41000 && a[1].what == WF::Do::Nudge, "second nudge at 40 s");
  a = run(f, 61000, 61100, kStalled);
  EXPECT(a.size() == 1 && a[0].ms == 61000 && a[0].what == WF::Do::OpenHotspot, "hotspot at 60 s");
}

static void test_fallback_no_nudge_while_associated() {
  WF f;
  f.boot_join(0);
  auto a = run(f, 0, 30000, kAssociated);
  EXPECT(a.empty(), "no nudge while associated (DHCP pending)");
  a = run(f, 30000, 30200, kStalled);
  EXPECT(a.size() == 1 && a[0].ms == 30000 && a[0].what == WF::Do::Nudge, "nudge once it stalls");
  a = run(f, 30200, 40000, kStalled);
  EXPECT(a.empty(), "second nudge not before 40 s");
  a = run(f, 40000, 59900, kAssociated);
  EXPECT(a.empty(), "nor while associated again");
  a = run(f, 59900, 60000, kStalled);
  EXPECT(a.size() == 1 && a[0].what == WF::Do::Nudge, "second nudge when it stalls again");
}

static void test_fallback_never_after_online() {
  WF f;
  f.boot_join(0);
  run(f, 0, 5000, kOnline);
  auto a = run(f, 5000, 300000, kStalled);
  EXPECT(a.empty(), "a network that worked since boot: no nudge, no hotspot");
}

static void test_fallback_online_in_time() {
  WF f;
  f.boot_join(0);
  auto a = run(f, 0, 21000, kStalled);
  EXPECT(a.size() == 1, "nudged once");
  a = run(f, 21000, 200000, kOnline);
  EXPECT(a.empty(), "joined after the nudge: nothing more");
}

static void test_fallback_retries_with_hotspot() {
  WF f;
  f.boot_join(0);
  run(f, 0, 60100, kStalled);  // nudges, then the hotspot at 60 s
  WF::Seen ap = kStalled;
  ap.hotspot_on = true;
  auto a = run(f, 60100, 90000, ap);
  EXPECT(a.empty(), "no nudges or retries for 30 s with the hotspot up");
  a = run(f, 90000, 90100, ap);
  EXPECT(a.size() == 1 && a[0].what == WF::Do::Join, "retry 30 s after the hotspot opened");
  WF::Seen trying = ap;
  trying.stalled = false;
  a = run(f, 90100, 105100, trying);
  EXPECT(a.size() == 1 && a[0].ms == 105000 && a[0].what == WF::Do::StopJoin, "a retry stops after 15 s");
  a = run(f, 105100, 120100, ap);
  EXPECT(a.size() == 1 && a[0].ms == 120000 && a[0].what == WF::Do::Join, "next retry 30 s after the last");
  WF::Seen joined = ap;
  joined.online = true;
  joined.stalled = false;
  a = run(f, 120100, 200000, joined);
  EXPECT(a.empty(), "joined: no stop, no more retries");
}

static void test_fallback_waits_for_phone() {
  WF f;
  f.boot_join(0);
  run(f, 0, 60100, kStalled);
  WF::Seen phone = kStalled;
  phone.hotspot_on = true;
  phone.phones = 1;
  auto a = run(f, 60100, 200000, phone);
  EXPECT(a.empty(), "never retry while a phone is on the hotspot");
  WF::Seen ap = phone;
  ap.phones = 0;
  a = run(f, 200000, 200100, ap);
  EXPECT(a.size() == 1 && a[0].what == WF::Do::Join, "retry once the phone leaves");
}

static void test_fallback_setup_join_holds_retries() {
  WF f;
  f.boot_join(0);
  run(f, 0, 60100, kStalled);
  WF::Seen setup = kStalled;
  setup.hotspot_on = true;
  setup.setup_join = true;
  auto a = run(f, 60100, 150000, setup);
  EXPECT(a.empty(), "nothing while the setup page's join runs");
  WF::Seen ap = setup;
  ap.setup_join = false;
  a = run(f, 150000, 179900, ap);  // the setup join's last tick was 149900
  EXPECT(a.empty(), "no retry within 30 s of the setup join");
  a = run(f, 179900, 180000, ap);
  EXPECT(a.size() == 1 && a[0].what == WF::Do::Join, "then the saved network again");
}

// 2026-10-31, on the board: AUTH_EXPIRE storms kept the library's own connect
// going (status 6), so both nudges were refused ("reconnect failed") and none
// was left when the router answered AUTH_FAIL at 44.5 s (status 4): the board
// came up only through the hotspot's retry, at 93 s.
static void test_fallback_refused_nudge_not_spent() {
  WF f;
  f.boot_join(0);
  auto a = run(f, 0, 44500, kStalled, false);
  EXPECT(a.size() == 5, "a refused nudge is retried every 5 s");
  EXPECT(a.size() > 1 && a[0].ms == 20000 && a[1].ms == 25000, "first at 20 s, again 5 s later");
  a = run(f, 44500, 45100, kStalled);
  EXPECT(a.size() == 1 && a[0].ms == 45000 && a[0].what == WF::Do::Nudge, "a nudge is still left for the AUTH_FAIL");
  a = run(f, 45100, 60000, kStalled);
  EXPECT(a.empty(), "the second nudge waits 20 s after the first that took");
  a = run(f, 60000, 60100, kStalled);
  EXPECT(a.size() == 1 && a[0].what == WF::Do::OpenHotspot, "hotspot still at 60 s");
}

static void test_fallback_no_saved_network() {
  WF f;  // no boot join: web.cpp opens the hotspot itself
  WF::Seen none = {false, false, true, true, 0, false};
  auto a = run(f, 0, 300000, none);
  EXPECT(a.empty(), "no saved network: nothing to nudge or retry");
}

int main() {
  test_ring_order_and_drop();
  test_ring_truncates();
  test_ring_threads();
  test_history_since();
  test_history_evicts();
  test_drain_merges();
  test_routes_read_only();
  test_bench_routes_apart();
  test_bench_mac();
  test_bench_update();
  test_bench_commands();
  test_host_guard();
  test_json_fields();
  test_hotspot_channel();
  test_wifi_qr();
  test_fallback_stalled_boot();
  test_fallback_no_nudge_while_associated();
  test_fallback_never_after_online();
  test_fallback_online_in_time();
  test_fallback_retries_with_hotspot();
  test_fallback_waits_for_phone();
  test_fallback_setup_join_holds_retries();
  test_fallback_no_saved_network();
  test_fallback_refused_nudge_not_spent();
  if (failures) {
    printf("web_core: %d failure(s)\n", failures);
    return 1;
  }
  printf("web_core: all passed\n");
  return 0;
}
