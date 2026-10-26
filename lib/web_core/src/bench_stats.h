// Statistics for the routing latency bench: percentiles of forwarding times and
// jitter of a steady note stream. Plain C++ so tests/ can run it on the Mac.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace web_core {

struct Summary {
  uint32_t count, min, p50, p90, p99, max;
};

// Nearest-rank percentiles. scratch holds n values; samples stay unsorted.
Summary summarize(const uint32_t *samples, size_t n, uint32_t *scratch);

// For intervals between events that should be evenly spaced (an arpeggiator,
// MIDI clock): how far each interval strays from the median one.
struct Jitter {
  uint32_t count, median_interval, p99_dev, max_dev;
};
Jitter jitter(const uint32_t *intervals, size_t n, uint32_t *scratch);

// Fixed-capacity sample buffer that keeps the first N samples and counts the
// rest. Written by one task while recording, read by another after it stops.
template <size_t N>
class SampleSet {
 public:
  void add(uint32_t v) {
    if (size_ < N) data_[size_++] = v;
    else ++overflow_;
  }
  void clear() { size_ = overflow_ = 0; }
  size_t size() const { return size_; }
  uint32_t overflow() const { return overflow_; }
  const uint32_t *data() const { return data_; }

 private:
  uint32_t data_[N];
  volatile size_t size_ = 0;
  volatile uint32_t overflow_ = 0;
};

}  // namespace web_core
