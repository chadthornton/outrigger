// The bench robot's core (docs/bench-robot.md): the line protocol, the send
// schedule, and the record of everything sent and received. No Arduino or
// TinyUSB here: the board (robot_main.cpp) and the Mac's simulator
// (tests/robot_sim.cpp) each supply a Port and the time, so the same code runs
// in both and the tests (tests/test_robot_proto.cpp) drive it directly.
//
// Times are microseconds on the robot's own clock (esp_timer on the board).
// MIDI travels as USB-MIDI event packets: byte 0 is cable << 4 | CIN.
#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace robot {

constexpr int kCables = 2;           // cable 0 and cable 1, each with an IN and an OUT jack
constexpr size_t kLineMax = 120;     // longer lines are refused whole
constexpr int kRepeaters = 4;        // `rep` slots (a clock, a CC sweep ...)
constexpr int64_t kTxRetryUs = 50000;  // a full TX FIFO for this long drops the event

// What the core needs from the board.
class Port {
 public:
  virtual ~Port() {}
  // Queue one USB-MIDI packet to the host. False when it can't (TX FIFO full,
  // or not mounted): the core retries a while, then counts it as not sent.
  virtual bool send(const uint8_t pkt[4]) = 0;
  virtual bool mounted() = 0;
  // The simulated unplug: drop off the bus (false) or come back (true).
  virtual void set_connected(bool on) = 0;
  // "mac=... fw=..." for the id reply.
  virtual void describe(char *out, size_t n) = 0;
};

// Where replies go. can_write() lets the log drain without blocking the
// schedule: the board says no while its UART TX buffer is nearly full.
class Out {
 public:
  virtual ~Out() {}
  virtual void line(const char *s) = 0;
  virtual bool can_write() { return true; }
};

// One line of the record. kind: 's' sent, 'r' received, 'u' unplugged,
// 'p' plugged back, 'M' mounted by the host, 'U' unmounted by the host.
struct Record {
  int64_t t_us;
  uint8_t kind;
  uint8_t pkt[4];
};

enum EventType : uint8_t { kMidi, kUnplug, kPlug };

struct Event {
  int64_t t_rel_us;  // from go's start time
  EventType type;
  uint8_t pkt[4];
  uint32_t arg;      // kUnplug: milliseconds down (0 = until plug)
};

struct Repeater {
  bool on;
  int64_t start_rel_us;
  int64_t interval_ns;
  uint32_t count;     // still to send
  uint32_t sent;
  int64_t retry_since;
  uint8_t pkt[4];
};

struct Counters {
  uint32_t sent, recv, not_sent, tx_retries, log_lost_tx, log_lost_rx, unplugs, mounts;
};

// A ring the USB task writes and the protocol task reads (one producer, one
// consumer), or one task does both (the send record).
class Ring {
 public:
  Ring(Record *buf, size_t cap) : buf_(buf), cap_(cap) {}
  bool push(const Record &r);
  bool pop(Record *r);
  bool peek(Record *r) const;
  size_t size() const { return head_.load() - tail_.load(); }
  void clear() { tail_.store(head_.load()); }

 private:
  Record *buf_;
  size_t cap_;
  std::atomic<size_t> head_{0}, tail_{0};
};

// The packet for one MIDI message (status byte and its data), or false for
// what the robot can't send: SysEx, a running-status byte, a short message.
bool pack(uint8_t cable, const uint8_t *msg, size_t len, uint8_t pkt[4]);
// Hex digits to bytes ("903C64" -> 90 3C 64). False on odd length or a non-hex digit.
bool parse_hex(const char *s, uint8_t *out, size_t cap, size_t *len);

class Core {
 public:
  Core(Port &port, Event *sched, size_t sched_cap, Record *tx_log, size_t tx_cap,
       Record *rx_log, size_t rx_cap);

  // Bytes from the control port. Each '\n' runs one line.
  void feed(char c, int64_t now, Out &out);
  void command(const char *line, int64_t now, Out &out);
  // Sends what's due and runs reconnects; then drains the log a line at a time
  // while out can take it. Call often: the board spins on it near a due time.
  void poll(int64_t now, Out &out);
  // The next time poll has work, or INT64_MAX.
  int64_t next_due() const;

  // From the USB task.
  void received(const uint8_t pkt[4], int64_t now);
  // From any task (the board's USB event loop): stored, and logged by the
  // next poll, so the receive ring keeps one producer (the USB task).
  void mount_changed(bool mounted, int64_t now);

  Counters counters() const;
  bool running() const { return running_; }

 private:
  void cmd_at(char **tok, int n, Out &out, int64_t now);
  void cmd_rep(char **tok, int n, Out &out);
  void cmd_status(int64_t now, Out &out);
  void cmd_log(char **tok, int n, Out &out);
  void unplug(int64_t now, uint32_t ms);
  void plug(int64_t now);
  bool try_send(const uint8_t pkt[4], int64_t now, int64_t *retry_since);
  void log_tx(int64_t t, uint8_t kind, const uint8_t pkt[4]);
  bool emit_record(Out &out);
  void clear_all();

  Port &port_;
  Event *sched_;
  size_t sched_cap_, sched_head_ = 0, sched_tail_ = 0;  // a FIFO: `at` times never go back
  int64_t last_at_ = 0;
  Ring tx_, rx_;
  Repeater rep_[kRepeaters] = {};
  Counters c_ = {};
  std::atomic<uint32_t> recv_{0}, log_lost_rx_{0}, mounts_{0};
  std::atomic<int64_t> mount_at_{-1}, unmount_at_{-1};  // waiting for poll to log them
  bool running_ = false;
  int64_t t0_ = 0;
  int64_t retry_since_ = -1;     // the head event's first failed send
  int64_t reconnect_at_ = -1;
  size_t drain_left_ = 0;        // records `log` still owes
  size_t drain_sent_ = 0;
  bool draining_ = false;
  char line_[kLineMax + 1];
  size_t line_len_ = 0;
  bool line_over_ = false;
};

}  // namespace robot
