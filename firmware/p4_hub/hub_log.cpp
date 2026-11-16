#include "hub_log.h"

#include <Arduino.h>
#include <esp_log.h>
#include <stdarg.h>
#include <string.h>

#include "hub_crash.h"
#include "json_out.h"

namespace {

constexpr size_t kLines = 512;
constexpr size_t kText = 100;
struct Line {
  uint32_t seq, ms;
  char source;
  char text[kText];
};
Line *ring = nullptr;  // PSRAM
uint32_t next_seq = 1;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t idf_vprintf = nullptr;
HubLogCounters counters = {};
uint32_t truncated = 0;

void push(char source, const char *text) {
  if (!ring) return;
  char clean[kText];
  size_t n = 0;
  for (const char *p = text; *p && n < kText - 1; ++p) {
    if (*p == '\033') {  // drop ANSI colour codes
      while (*p && *p != 'm') ++p;
      if (!*p) break;
      continue;
    }
    if (*p == '\n' || *p == '\r') continue;
    clean[n++] = *p;
  }
  clean[n] = 0;
  if (!n) return;
  if (strlen(text) >= kText) ++truncated;
  const uint32_t ms = millis();
  portENTER_CRITICAL_SAFE(&mux);
  Line &l = ring[next_seq % kLines];
  l.seq = next_seq++;
  l.ms = ms;
  l.source = source;
  memcpy(l.text, clean, n + 1);
  portEXIT_CRITICAL_SAFE(&mux);
  hub_crash_tail(source, ms, clean);
}

int idf_hook(const char *fmt, va_list args) {
  char buf[160];
  va_list copy;
  va_copy(copy, args);
  vsnprintf(buf, sizeof buf, fmt, copy);
  va_end(copy);
  if (strstr(buf, "CHECK_SHORT_DEV_DESC")) ++counters.short_desc_failed;
  else if (strstr(buf, "Port disabled")) ++counters.port_disabled;
  else if (strstr(buf, "ENUM:") && buf[0] != 'I') ++counters.enum_failed_other;
  push('E', buf);
  return idf_vprintf ? idf_vprintf(fmt, args) : vprintf(fmt, args);
}

}  // namespace

void hub_log_begin() {
  if (ring) return;
  ring = static_cast<Line *>(heap_caps_calloc(kLines, sizeof(Line), MALLOC_CAP_SPIRAM));
  idf_vprintf = esp_log_set_vprintf(idf_hook);
}

void hub_log(char source, const char *fmt, ...) {
  char buf[kText];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof buf, fmt, a);
  va_end(a);
  push(source, buf);
  printf("%c %s\n", source, buf);
}

uint32_t hub_log_next() {
  portENTER_CRITICAL_SAFE(&mux);
  const uint32_t n = next_seq;
  portEXIT_CRITICAL_SAFE(&mux);
  return n;
}

uint32_t hub_log_dropped_long() { return truncated; }
HubLogCounters hub_log_counters() { return counters; }

size_t hub_log_json(uint32_t since, char *out, size_t cap) {
  JsonOut o{out, cap};
  const uint32_t next = hub_log_next();
  const uint32_t oldest = next > kLines ? next - kLines + 1 : 1;
  uint32_t start = since + 1;
  if (since >= next) start = oldest;  // a cursor from before a reboot
  const uint32_t lost = start < oldest ? oldest - start : 0;
  if (start < oldest) start = oldest;
  o.raw("{\"next\":%lu,\"lost\":%lu,\"lines\":[", (unsigned long)next, (unsigned long)lost);
  bool first = true;
  for (uint32_t s = start; s < next && o.room() > 200; ++s) {
    Line l;
    portENTER_CRITICAL_SAFE(&mux);
    l = ring[s % kLines];
    portEXIT_CRITICAL_SAFE(&mux);
    if (l.seq != s) continue;  // overwritten while we read
    o.raw(first ? "[" : ",[");
    first = false;
    o.raw("%lu,%lu,\"%c\",", (unsigned long)l.seq, (unsigned long)l.ms, l.source);
    o.str(l.text);
    o.raw("]");
  }
  o.raw("]}");
  return o.len;
}
