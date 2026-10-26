#include "bench.h"

#include <Arduino.h>
#include <EspUsbHost.h>
#include <esp_heap_caps.h>
#include <new>

#include "bench_stats.h"

using web_core::SampleSet;

constexpr size_t kSamples = 4096;

struct ModeResult {
  SampleSet<kSamples> forward, note_gaps, clock_gaps;
  uint32_t send_failures;
  uint32_t heap_min_internal, heap_min_block;  // bytes, lowest seen during the run
  uint32_t seconds;
  bool recorded;
};

static ModeResult *results;  // kBenchModes entries, in PSRAM
static uint32_t *scratch;    // kSamples, in PSRAM

static volatile BenchMode running = BenchMode::None;
static volatile uint32_t run_ms = 0;
static uint32_t started_ms = 0;
static volatile bool start_pending = false;
static uint32_t requested_ms = 0;

// USB client task only.
static int64_t last_note_us = 0, last_clock_us = 0;
static int64_t pending[64];  // arrival times of sends awaiting completion, FIFO
static uint8_t pending_head = 0, pending_tail = 0;

static ModeResult &current() { return results[static_cast<int>(running)]; }

static void on_send_complete(bool ok) {
  if (pending_head == pending_tail) return;
  const int64_t t0 = pending[pending_tail];
  pending_tail = (pending_tail + 1) % 64;
  if (running == BenchMode::None) return;
  if (ok) current().forward.add(static_cast<uint32_t>(esp_timer_get_time() - t0));
  else ++current().send_failures;
}

void bench_begin() {
  results = static_cast<ModeResult *>(heap_caps_calloc(kBenchModes, sizeof(ModeResult), MALLOC_CAP_SPIRAM));
  scratch = static_cast<uint32_t *>(heap_caps_malloc(kSamples * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
  if (!results || !scratch) return;
  for (int m = 0; m < kBenchModes; ++m) new (&results[m]) ModeResult{};
  EspUsbHost::midiSendCompleteHook = on_send_complete;
}

void bench_midi_in(int64_t t_us, uint8_t status, uint8_t data2) {
  if (running == BenchMode::None) return;
  if ((status & 0xF0) == 0x90 && data2 > 0) {
    if (last_note_us) current().note_gaps.add(static_cast<uint32_t>(t_us - last_note_us));
    last_note_us = t_us;
  } else if (status == 0xF8) {
    if (last_clock_us) current().clock_gaps.add(static_cast<uint32_t>(t_us - last_clock_us));
    last_clock_us = t_us;
  }
}

void bench_send_failed() {
  if (running != BenchMode::None) ++current().send_failures;
}

void bench_forwarded(int64_t t_us) {
  const uint8_t next = (pending_head + 1) % 64;
  if (next == pending_tail) return;  // can't happen at MIDI rates; drop the sample
  pending[pending_head] = t_us;
  pending_head = next;
}

static BenchMode requested = BenchMode::None;

bool bench_start(BenchMode mode, uint32_t seconds) {
  if (!results || mode == BenchMode::None || running != BenchMode::None || start_pending) return false;
  if (seconds < 5 || seconds > 600) return false;
  requested = mode;
  run_ms = seconds * 1000;
  requested_ms = millis();
  start_pending = true;  // the loop starts it, so all resets happen on one task
  return true;
}

void bench_loop(uint32_t now_ms) {
  if (start_pending && (requested != BenchMode::WifiOff || now_ms - requested_ms >= 2000)) {
    ModeResult &r = results[static_cast<int>(requested)];
    r.forward.clear();
    r.note_gaps.clear();
    r.clock_gaps.clear();
    r.send_failures = 0;
    r.heap_min_internal = r.heap_min_block = UINT32_MAX;
    r.seconds = run_ms / 1000;
    r.recorded = false;
    last_note_us = last_clock_us = 0;
    started_ms = now_ms;
    start_pending = false;
    running = requested;
  }
  if (running == BenchMode::None) return;
  ModeResult &r = current();
  const uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  const uint32_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (internal < r.heap_min_internal) r.heap_min_internal = internal;
  if (block < r.heap_min_block) r.heap_min_block = block;
  if (now_ms - started_ms >= run_ms) {
    r.recorded = true;
    running = BenchMode::None;
  }
}

BenchMode bench_running() { return start_pending ? requested : running; }

bool bench_wants_wifi_off(uint32_t now_ms) {
  if (running == BenchMode::WifiOff) return true;
  return start_pending && requested == BenchMode::WifiOff && now_ms - requested_ms >= 500;
}

static const char *const kNames[kBenchModes] = {"none", "wifi_off", "idle", "busy"};
const char *bench_mode_name(BenchMode m) { return kNames[static_cast<int>(m)]; }
BenchMode bench_mode_from(const char *name) {
  for (int m = 1; m < kBenchModes; ++m)
    if (strcmp(name, kNames[m]) == 0) return static_cast<BenchMode>(m);
  return BenchMode::None;
}

static size_t put_summary(char *out, size_t len, const char *name, const SampleSet<kSamples> &s) {
  const web_core::Summary m = web_core::summarize(s.data(), s.size(), scratch);
  return snprintf(out, len,
                  "\"%s\":{\"n\":%lu,\"min\":%lu,\"p50\":%lu,\"p90\":%lu,\"p99\":%lu,\"max\":%lu,\"overflow\":%lu}",
                  name, (unsigned long)m.count, (unsigned long)m.min, (unsigned long)m.p50, (unsigned long)m.p90,
                  (unsigned long)m.p99, (unsigned long)m.max, (unsigned long)s.overflow());
}

static size_t put_jitter(char *out, size_t len, const char *name, const SampleSet<kSamples> &s) {
  const web_core::Jitter j = web_core::jitter(s.data(), s.size(), scratch);
  return snprintf(out, len, "\"%s\":{\"n\":%lu,\"median\":%lu,\"p99_dev\":%lu,\"max_dev\":%lu}", name,
                  (unsigned long)j.count, (unsigned long)j.median_interval, (unsigned long)j.p99_dev,
                  (unsigned long)j.max_dev);
}

size_t bench_json(char *out, size_t len) {
  if (!results) return snprintf(out, len, "{\"error\":\"no PSRAM\"}");
  size_t n = snprintf(out, len, "{\"running\":\"%s\",\"units\":\"us\"", bench_mode_name(bench_running()));
  for (int m = 1; m < kBenchModes && n < len; ++m) {
    const ModeResult &r = results[m];
    if (!r.recorded) continue;
    n += snprintf(out + n, len - n, ",\"%s\":{\"seconds\":%lu,\"send_failures\":%lu,\"heap_min_internal\":%lu,\"heap_min_block\":%lu,",
                  kNames[m], (unsigned long)r.seconds, (unsigned long)r.send_failures,
                  (unsigned long)r.heap_min_internal, (unsigned long)r.heap_min_block);
    if (n < len) n += put_summary(out + n, len - n, "forward", r.forward);
    if (n < len) n += snprintf(out + n, len - n, ",");
    if (n < len) n += put_jitter(out + n, len - n, "note_gaps", r.note_gaps);
    if (n < len) n += snprintf(out + n, len - n, ",");
    if (n < len) n += put_jitter(out + n, len - n, "clock_gaps", r.clock_gaps);
    if (n < len) n += snprintf(out + n, len - n, "}");
  }
  if (n < len) n += snprintf(out + n, len - n, "}");
  return n < len ? n : len - 1;
}

size_t bench_raw_json(BenchMode mode, int kind, char *out, size_t len) {
  if (!results || mode == BenchMode::None || kind < 0 || kind > 2) return snprintf(out, len, "[]");
  const ModeResult &r = results[static_cast<int>(mode)];
  const SampleSet<kSamples> &s = kind == 0 ? r.forward : kind == 1 ? r.note_gaps : r.clock_gaps;
  size_t n = snprintf(out, len, "[");
  for (size_t i = 0; i < s.size() && n + 12 < len; ++i)
    n += snprintf(out + n, len - n, i ? ",%lu" : "%lu", (unsigned long)s.data()[i]);
  n += snprintf(out + n, len - n, "]");
  return n < len ? n : len - 1;
}
