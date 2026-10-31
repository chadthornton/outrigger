// The diagnostics log behind GET /log. Each task that logs (the USB client
// task, the loader task, the loop) owns one LineRing: a lock-free queue with
// one producer and one consumer, whose push never blocks and never allocates,
// so the USB task can log from a callback. A full ring refuses the new line and
// counts it. The web server's task is the only consumer: it drains every ring,
// oldest line first, into a LogHistory that numbers the lines, so any number of
// browsers can poll with ?since=N without taking lines from each other.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

namespace web_core {

constexpr size_t kLogText = 96;

struct LogLine {
  uint32_t ms;     // millis() when it was logged
  char source;     // 'U' USB task, 'L' loader task, 'M' main loop
  char text[kLogText];
};

class LineQueue {
 public:
  virtual bool peek(LogLine &out) const = 0;
  virtual bool pop(LogLine &out) = 0;
  virtual uint32_t dropped() const = 0;

 protected:
  ~LineQueue() = default;
};

template <size_t N>
class LineRing final : public LineQueue {
 public:
  // Producer only. False (and counted) when the ring is full.
  bool push(uint32_t ms, const char *text) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= N) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    LogLine &slot = lines_[h % N];
    slot.ms = ms;
    slot.source = 0;
    strncpy(slot.text, text ? text : "", kLogText - 1);
    slot.text[kLogText - 1] = 0;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  // Consumer only.
  bool peek(LogLine &out) const override {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    out = lines_[t % N];
    return true;
  }
  bool pop(LogLine &out) override {
    if (!peek(out)) return false;
    tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    return true;
  }
  uint32_t dropped() const override { return dropped_.load(std::memory_order_relaxed); }

 private:
  LogLine lines_[N] = {};
  std::atomic<uint32_t> head_{0}, tail_{0}, dropped_{0};
};

// The consumer's side: numbered lines, the newest N kept. Not thread-safe; only
// the web server's task uses it.
template <size_t N>
class LogHistory {
 public:
  void add(const LogLine &line) {
    lines_[(next_ - 1) % N] = line;
    ++next_;
  }
  // The sequence number the next line will get (lines are numbered from 1).
  uint32_t next() const { return next_; }
  // Calls fn(seq, line) for each kept line after `since`, oldest first. *lost
  // gets how many lines after `since` were already evicted. A cursor at or past
  // next() (a page left open across a reboot) starts again from the oldest.
  template <class Fn>
  void read(uint32_t since, uint32_t *lost, Fn fn) const {
    const uint32_t oldest = next_ > N ? next_ - N : 1;
    uint32_t start = since + 1;
    if (since >= next_) start = oldest;
    *lost = start < oldest ? oldest - start : 0;
    if (start < oldest) start = oldest;
    for (uint32_t seq = start; seq < next_; ++seq) fn(seq, lines_[(seq - 1) % N]);
  }

 private:
  LogLine lines_[N] = {};
  uint32_t next_ = 1;
};

struct LineRingRef {
  LineQueue *ring;
  char source;
};

// Moves every line out of the rings into the history, oldest first across
// rings. Stops after what the rings could hold at the start, so a busy
// producer can't keep the consumer here.
template <size_t N>
void drain(const LineRingRef *rings, size_t count, LogHistory<N> &history, size_t limit = 1024) {
  for (size_t moved = 0; moved < limit; ++moved) {
    int best = -1;
    LogLine line, best_line;
    for (size_t i = 0; i < count; ++i) {
      if (!rings[i].ring || !rings[i].ring->peek(line)) continue;
      if (best < 0 || int32_t(line.ms - best_line.ms) < 0) {
        best = int(i);
        best_line = line;
      }
    }
    if (best < 0) return;
    rings[best].ring->pop(line);
    line.source = rings[best].source;
    history.add(line);
  }
}

}  // namespace web_core
