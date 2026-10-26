// Hands MIDI events from the USB task to the web server without locks: one
// producer (on_midi), one consumer (the server's drain). When full, push()
// refuses the new event and counts it, so the USB task never waits.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

namespace web_core {

struct MonitorEvent {
  uint32_t at_ms;
  uint8_t address;  // source device
  char mark;        // '>' sent to the FM-1, '!' send failed, a Router drop letter, ' ' from the FM-1
  uint8_t status, data1, data2;
};

// Holds N - 1 events.
template <size_t N>
class MonitorRing {
 public:
  bool push(const MonitorEvent &e) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next = (head + 1) % N;
    if (next == tail_.load(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    events_[head] = e;
    head_.store(next, std::memory_order_release);
    return true;
  }
  bool pop(MonitorEvent &out) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    out = events_[tail];
    tail_.store((tail + 1) % N, std::memory_order_release);
    return true;
  }
  uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  MonitorEvent events_[N];
  std::atomic<size_t> head_{0}, tail_{0};
  std::atomic<uint32_t> dropped_{0};
};

}  // namespace web_core
