// Host-side tests for lib/web_core: bench statistics, pairing, the request
// guard and the MIDI monitor ring. Run with `make test`.
#include <stdio.h>
#include <string.h>

#include "bench_stats.h"
#include "json_field.h"
#include "monitor_ring.h"
#include "pairing.h"
#include "request_guard.h"

using namespace web_core;

static int failures = 0;

#define EXPECT(cond, what)                                   \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s (line %d)\n", what, __LINE__);         \
      ++failures;                                            \
    }                                                        \
  } while (0)

// ---------- bench_stats ----------
static void test_summary() {
  uint32_t s[100], scratch[100];
  for (uint32_t i = 0; i < 100; ++i) s[99 - i] = (i + 1) * 10;  // 10..1000, reversed
  const Summary m = summarize(s, 100, scratch);
  EXPECT(m.count == 100, "count");
  EXPECT(m.min == 10 && m.max == 1000, "min/max");
  EXPECT(m.p50 == 500, "p50 is nearest-rank");
  EXPECT(m.p90 == 900, "p90");
  EXPECT(m.p99 == 990, "p99");
  EXPECT(s[0] == 1000, "input left unsorted");

  const Summary empty = summarize(s, 0, scratch);
  EXPECT(empty.count == 0 && empty.max == 0, "empty summary is zero");

  uint32_t one[1] = {42};
  const Summary single = summarize(one, 1, scratch);
  EXPECT(single.p50 == 42 && single.p99 == 42, "single sample");
}

static void test_jitter() {
  // A steady 125 ms arp with one late note (+3 ms) and one early (-1 ms).
  uint32_t iv[50], scratch[50];
  for (int i = 0; i < 50; ++i) iv[i] = 125000;
  iv[10] = 128000;
  iv[20] = 124000;
  const Jitter j = jitter(iv, 50, scratch);
  EXPECT(j.count == 50, "jitter count");
  EXPECT(j.median_interval == 125000, "median interval");
  EXPECT(j.max_dev == 3000, "max deviation from the median");
  EXPECT(j.p99_dev == 3000, "p99 deviation");
}

static void test_sample_set() {
  SampleSet<4> set;
  for (uint32_t v = 1; v <= 6; ++v) set.add(v);
  EXPECT(set.size() == 4, "sample set stops at capacity");
  EXPECT(set.overflow() == 2, "overflow counted");
  EXPECT(set.data()[3] == 4, "keeps the first samples");
  set.clear();
  EXPECT(set.size() == 0 && set.overflow() == 0, "clear");
}

// ---------- pairing ----------
static void test_pairing() {
  Pairing p;
  EXPECT(p.pin(0)[0] == 0, "no pin before start");
  EXPECT(p.submit(0, "123456") == PairResult::NoAttempt, "submit without start");

  p.start(1000, 42);
  EXPECT(strcmp(p.pin(1000), "000042") == 0, "pin is six digits, zero padded");
  EXPECT(p.submit(2000, "000041") == PairResult::Wrong, "wrong pin");
  EXPECT(p.submit(3000, "000042") == PairResult::Ok, "right pin");
  EXPECT(p.pin(3000)[0] == 0, "pin cleared after success");
  EXPECT(p.submit(3000, "000042") == PairResult::NoAttempt, "pin is single use");

  p.start(10000, 987654321);
  EXPECT(strcmp(p.pin(10000), "654321") == 0, "pin from the low six digits");
  EXPECT(p.submit(10000 + Pairing::kLifetimeMs, "654321") == PairResult::Expired, "pin expires");
  EXPECT(p.pin(10000 + Pairing::kLifetimeMs)[0] == 0, "expired pin not shown");

  p.start(100000, 111111);
  EXPECT(p.submit(100001, "000000") == PairResult::Wrong, "try 1");
  EXPECT(p.submit(100002, "000000") == PairResult::Wrong, "try 2");
  EXPECT(p.submit(100003, "000000") == PairResult::Locked, "third wrong try ends the attempt");
  EXPECT(p.submit(100004, "111111") == PairResult::NoAttempt, "right pin after lockout refused");
  EXPECT(p.submit(100004, "11111") == PairResult::NoAttempt, "short pin refused");
}

static void test_tokens() {
  TokenStore store;
  uint8_t t[16];
  char hex[33];
  for (int i = 0; i < 16; ++i) t[i] = static_cast<uint8_t>(i * 17);
  store.add(t);
  token_to_hex(t, hex);
  EXPECT(strlen(hex) == 32, "hex length");
  EXPECT(store.check(hex), "added token accepted");
  hex[5] = hex[5] == 'a' ? 'b' : 'a';
  EXPECT(!store.check(hex), "changed token refused");
  EXPECT(!store.check(""), "empty token refused");
  EXPECT(!store.check(nullptr), "missing token refused");
  EXPECT(!store.check("00112233"), "short token refused");

  // A fifth browser evicts the oldest.
  uint8_t first[16];
  memcpy(first, t, 16);
  for (int k = 1; k <= TokenStore::kMax; ++k) {
    t[0] = static_cast<uint8_t>(0xF0 + k);
    store.add(t);
  }
  token_to_hex(first, hex);
  EXPECT(!store.check(hex), "oldest token evicted");
  token_to_hex(t, hex);
  EXPECT(store.check(hex), "newest token kept");
  for (char *ch = hex; *ch; ++ch) if (*ch >= 'a') *ch -= 32;
  EXPECT(store.check(hex), "upper-case hex accepted");
}

// ---------- request_guard ----------
static void test_host_guard() {
  const HostNames n = {"fm1hub.local", "192.168.1.42", "192.168.4.1"};
  EXPECT(host_allowed("fm1hub.local", n), "mdns name");
  EXPECT(host_allowed("FM1HUB.local", n), "case-insensitive");
  EXPECT(host_allowed("fm1hub.local.", n), "trailing dot");
  EXPECT(host_allowed("fm1hub.local:80", n), "port 80");
  EXPECT(host_allowed("192.168.1.42", n), "own ip");
  EXPECT(host_allowed("192.168.4.1", n), "setup hotspot ip");
  EXPECT(!host_allowed("evil.example", n), "foreign name (DNS rebinding)");
  EXPECT(!host_allowed("fm1hub.local.evil.example", n), "suffix trick");
  EXPECT(!host_allowed("192.168.1.420", n), "longer ip");
  EXPECT(!host_allowed("", n), "empty host");
  EXPECT(!host_allowed(nullptr, n), "missing host");

  const HostNames off = {"fm1hub.local", "", ""};
  EXPECT(!host_allowed("", off), "empty ip never matches empty host");
}

static void test_origin_guard() {
  const HostNames n = {"fm1hub.local", "192.168.1.42", "192.168.4.1"};
  EXPECT(origin_allowed("http://fm1hub.local", n), "own origin");
  EXPECT(origin_allowed("http://192.168.1.42", n), "ip origin");
  EXPECT(origin_allowed(nullptr, n), "no origin (not a browser)");
  EXPECT(!origin_allowed("https://evil.example", n), "foreign origin");
  EXPECT(!origin_allowed("http://fm1hub.local.evil.example", n), "suffix trick");
  EXPECT(!origin_allowed("null", n), "opaque origin");
  EXPECT(!origin_allowed("fm1hub.local", n), "origin without scheme");
}

// ---------- monitor_ring ----------
static void test_ring() {
  MonitorRing<4> ring;
  MonitorEvent e{};
  EXPECT(!ring.pop(e), "empty ring");
  for (uint8_t i = 0; i < 3; ++i) {
    e.data1 = i;
    EXPECT(ring.push(e), "push fits");
  }
  e.data1 = 9;
  EXPECT(!ring.push(e), "full ring refuses (capacity N-1)");
  EXPECT(ring.dropped() == 1, "drop counted");
  MonitorEvent out{};
  EXPECT(ring.pop(out) && out.data1 == 0, "fifo order");
  e.data1 = 3;
  EXPECT(ring.push(e), "room again after pop");
  int n = 0;
  while (ring.pop(out)) ++n;
  EXPECT(n == 3 && out.data1 == 3, "drains the rest in order");
}

// ---------- json_field ----------
static void test_json() {
  const char *body = "{\"ssid\": \"Home \\\"5G\\\"\", \"pass\":\"a\\\\b\", \"text_size\": 2, \"neg\":-3, \"pin\":\"012345\"}";
  char out[32];
  long v = 0;
  EXPECT(json_string(body, "ssid", out, sizeof out) && strcmp(out, "Home \"5G\"") == 0, "string with escaped quotes");
  EXPECT(json_string(body, "pass", out, sizeof out) && strcmp(out, "a\\b") == 0, "escaped backslash");
  EXPECT(json_string(body, "pin", out, sizeof out) && strcmp(out, "012345") == 0, "digits stay a string");
  EXPECT(json_int(body, "text_size", &v) && v == 2, "int");
  EXPECT(json_int(body, "neg", &v) && v == -3, "negative int");
  EXPECT(!json_int(body, "ssid", &v), "string is not an int");
  EXPECT(!json_string(body, "missing", out, sizeof out), "missing key");
  EXPECT(!json_string(body, "size", out, sizeof out), "key must match whole (text_size is not size)");
  char tiny[4];
  EXPECT(!json_string(body, "ssid", tiny, sizeof tiny), "too long for buffer refused");
  EXPECT(!json_string(nullptr, "ssid", out, sizeof out), "null body");

  char esc[32];
  json_escape("a\"b\\c\n", esc, sizeof esc);
  EXPECT(strcmp(esc, "a\\\"b\\\\c\\u000a") == 0, "escape quote, backslash, control");
  json_escape("abcdefghij", esc, 6);
  EXPECT(strcmp(esc, "abcde") == 0, "escape truncates to fit");
}

int main() {
  test_json();
  test_summary();
  test_jitter();
  test_sample_set();
  test_pairing();
  test_tokens();
  test_host_guard();
  test_origin_guard();
  test_ring();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("web_core: all tests passed\n");
  return 0;
}
