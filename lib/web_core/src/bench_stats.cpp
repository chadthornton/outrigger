#include "bench_stats.h"

#include <algorithm>
#include <string.h>

namespace web_core {

static uint32_t rank(const uint32_t *sorted, size_t n, unsigned pct) {
  size_t idx = (n * pct + 99) / 100;  // ceil(n * pct / 100)
  if (idx == 0) idx = 1;
  return sorted[idx - 1];
}

Summary summarize(const uint32_t *samples, size_t n, uint32_t *scratch) {
  Summary s{};
  if (n == 0) return s;
  memcpy(scratch, samples, n * sizeof *samples);
  std::sort(scratch, scratch + n);
  s.count = static_cast<uint32_t>(n);
  s.min = scratch[0];
  s.max = scratch[n - 1];
  s.p50 = rank(scratch, n, 50);
  s.p90 = rank(scratch, n, 90);
  s.p99 = rank(scratch, n, 99);
  return s;
}

Jitter jitter(const uint32_t *intervals, size_t n, uint32_t *scratch) {
  Jitter j{};
  if (n == 0) return j;
  const Summary s = summarize(intervals, n, scratch);
  j.count = s.count;
  j.median_interval = s.p50;
  for (size_t i = 0; i < n; ++i)
    scratch[i] = intervals[i] > s.p50 ? intervals[i] - s.p50 : s.p50 - intervals[i];
  std::sort(scratch, scratch + n);
  j.p99_dev = rank(scratch, n, 99);
  j.max_dev = scratch[n - 1];
  return j;
}

}  // namespace web_core
