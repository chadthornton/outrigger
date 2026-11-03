#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace web_core {

namespace {

// Chad's names for his boards, by the MACs on his labelled boards (2026-11-03;
// the Mac's registry is tools/devices.py).
struct Waveshare {
  const char *name;
  uint8_t mac[6];
};
constexpr Waveshare kWaveshares[] = {
    {"small-v1", {0xA0, 0x85, 0xE3, 0xF0, 0xA2, 0x3C}},
    {"small-v2", {0xD8, 0x3B, 0xDA, 0xA0, 0x67, 0xD0}},
    {"big-s3", {0x44, 0xBD, 0x8D, 0x61, 0x2B, 0x28}},
};

// The value of key in a query string ("a=1&b=2"), copied into out.
bool query_value(const char *query, const char *key, char *out, size_t len) {
  const size_t klen = strlen(key);
  for (const char *p = query; p && *p;) {
    const char *end = strchr(p, '&');
    const size_t n = end ? size_t(end - p) : strlen(p);
    if (n > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const size_t v = n - klen - 1;
      if (v >= len) return false;
      memcpy(out, p + klen + 1, v);
      out[v] = 0;
      return true;
    }
    p = end ? end + 1 : nullptr;
  }
  return false;
}

bool parse_index(const char *query, int &out) {
  char buf[8];
  if (!query_value(query, "index", buf, sizeof buf) || !buf[0]) return false;
  char *end = nullptr;
  const long v = strtol(buf, &end, 10);
  if (*end || v < 0 || v > 63) return false;
  out = int(v);
  return true;
}

}  // namespace

const char *waveshare_name(const uint8_t mac[6]) {
  for (const Waveshare &w : kWaveshares)
    if (memcmp(mac, w.mac, 6) == 0) return w.name;
  return nullptr;
}

void web_host_name(const uint8_t mac[6], bool bench_build, char *out, size_t len) {
  const char *name = waveshare_name(mac);
  snprintf(out, len, "%s", name ? name : bench_build ? "fm1bench" : "fm1hub");
}

const char *bench_kind_name(BenchKind k) {
  switch (k) {
    case BenchKind::Select: return "select";
    case BenchKind::Switch: return "switch";
    case BenchKind::Backup: return "backup";
    case BenchKind::Retry: return "retry";
    default: return "none";
  }
}

bool parse_bench_command(const char *path, const char *query, BenchCommand &out, const char **why) {
  const char *dummy;
  if (!why) why = &dummy;
  out = BenchCommand{};
  if (!query) query = "";
  if (strcmp(path, "/bench/select") == 0) {
    if (!parse_index(query, out.index)) return *why = "select needs index=0..63", false;
    out.kind = BenchKind::Select;
    return true;
  }
  if (strcmp(path, "/bench/act") != 0) return *why = "not a bench command", false;
  char kind[8];
  if (!query_value(query, "kind", kind, sizeof kind)) return *why = "act needs kind=switch|backup|retry", false;
  if (strcmp(kind, "switch") == 0) {
    if (!parse_index(query, out.index)) return *why = "switch needs index=0..63", false;
    out.kind = BenchKind::Switch;
  } else if (strcmp(kind, "backup") == 0) {
    out.kind = BenchKind::Backup;
  } else if (strcmp(kind, "retry") == 0) {
    out.kind = BenchKind::Retry;
  } else {
    return *why = "kind must be switch, backup or retry", false;
  }
  return true;
}

}  // namespace web_core
