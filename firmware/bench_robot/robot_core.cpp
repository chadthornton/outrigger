// The bench robot's line protocol and schedule (robot_core.h, docs/bench-robot.md).
#include "robot_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace robot {

bool Ring::push(const Record &r) {
  size_t h = head_.load(std::memory_order_relaxed);
  if (h - tail_.load(std::memory_order_acquire) >= cap_) return false;
  buf_[h % cap_] = r;
  head_.store(h + 1, std::memory_order_release);
  return true;
}

bool Ring::peek(Record *r) const {
  size_t t = tail_.load(std::memory_order_relaxed);
  if (t == head_.load(std::memory_order_acquire)) return false;
  *r = buf_[t % cap_];
  return true;
}

bool Ring::pop(Record *r) {
  if (!peek(r)) return false;
  tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  return true;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parse_hex(const char *s, uint8_t *out, size_t cap, size_t *len) {
  size_t n = strlen(s);
  if (n == 0 || n % 2 || n / 2 > cap) return false;
  for (size_t i = 0; i < n; i += 2) {
    int hi = hexval(s[i]), lo = hexval(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i / 2] = (uint8_t)(hi << 4 | lo);
  }
  *len = n / 2;
  return true;
}

// USB-MIDI 1.0, table 4-1: the Code Index Number, and how many bytes follow.
bool pack(uint8_t cable, const uint8_t *msg, size_t len, uint8_t pkt[4]) {
  if (cable >= kCables || len == 0 || !(msg[0] & 0x80)) return false;
  uint8_t st = msg[0], cin;
  size_t need;
  if (st < 0xF0) {
    cin = st >> 4;
    need = (cin == 0xC || cin == 0xD) ? 2 : 3;
  } else {
    switch (st) {
      case 0xF1: case 0xF3: cin = 0x2; need = 2; break;
      case 0xF2: cin = 0x3; need = 3; break;
      case 0xF6: cin = 0x5; need = 1; break;
      case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF:
        cin = 0xF; need = 1; break;
      default: return false;  // SysEx (F0/F7) and the undefined F4, F5, F9, FD
    }
  }
  if (len != need) return false;
  for (size_t i = 1; i < len; ++i)
    if (msg[i] & 0x80) return false;
  pkt[0] = (uint8_t)(cable << 4 | cin);
  pkt[1] = msg[0];
  pkt[2] = len > 1 ? msg[1] : 0;
  pkt[3] = len > 2 ? msg[2] : 0;
  return true;
}

static bool parse_i64(const char *s, int64_t *v) {
  if (!s || !*s) return false;
  char *end;
  long long x = strtoll(s, &end, 10);
  if (*end) return false;
  *v = x;
  return true;
}

Core::Core(Port &port, Event *sched, size_t sched_cap, Record *tx_log, size_t tx_cap,
           Record *rx_log, size_t rx_cap)
    : port_(port), sched_(sched), sched_cap_(sched_cap), tx_(tx_log, tx_cap), rx_(rx_log, rx_cap) {}

Counters Core::counters() const {
  Counters c = c_;
  c.recv = recv_.load();
  c.log_lost_rx = log_lost_rx_.load();
  c.mounts = mounts_.load();
  return c;
}

void Core::feed(char c, int64_t now, Out &out) {
  if (c == '\n' || c == '\r') {
    if (line_over_) {
      out.line("err too-long");
    } else if (line_len_) {
      line_[line_len_] = 0;
      command(line_, now, out);
    }
    line_len_ = 0;
    line_over_ = false;
    return;
  }
  if (line_len_ >= kLineMax) {
    line_over_ = true;
    return;
  }
  line_[line_len_++] = c;
}

void Core::log_tx(int64_t t, uint8_t kind, const uint8_t pkt[4]) {
  Record r = {t, kind, {0, 0, 0, 0}};
  if (pkt) memcpy(r.pkt, pkt, 4);
  if (!tx_.push(r)) ++c_.log_lost_tx;
}

void Core::received(const uint8_t pkt[4], int64_t now) {
  recv_.fetch_add(1);
  Record r = {now, 'r', {pkt[0], pkt[1], pkt[2], pkt[3]}};
  if (!rx_.push(r)) log_lost_rx_.fetch_add(1);
}

void Core::mount_changed(bool mounted, int64_t now) {
  if (mounted) mounts_.fetch_add(1);
  (mounted ? mount_at_ : unmount_at_).store(now);
}

void Core::unplug(int64_t now, uint32_t ms) {
  port_.set_connected(false);
  log_tx(now, 'u', nullptr);
  ++c_.unplugs;
  reconnect_at_ = ms ? now + (int64_t)ms * 1000 : -1;
}

void Core::plug(int64_t now) {
  port_.set_connected(true);
  log_tx(now, 'p', nullptr);
  reconnect_at_ = -1;
}

void Core::clear_all() {
  running_ = false;
  sched_head_ = sched_tail_ = 0;
  last_at_ = 0;
  for (auto &r : rep_) r = Repeater();
  tx_.clear();
  rx_.clear();
  c_ = Counters();
  recv_.store(0);
  log_lost_rx_.store(0);
  mounts_.store(0);
  mount_at_.store(-1);
  unmount_at_.store(-1);
  retry_since_ = -1;
  draining_ = false;
}

static int tokenize(char *s, char **tok, int max) {
  int n = 0;
  while (*s && n < max) {
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) break;
    tok[n++] = s;
    while (*s && *s != ' ' && *s != '\t') ++s;
    if (*s) *s++ = 0;
  }
  while (*s == ' ' || *s == '\t') ++s;
  return *s ? max + 1 : n;  // more than max tokens: refused
}

// at <t_rel_us> <cable> <hex>  |  at <t_rel_us> unplug <ms>  |  at <t_rel_us> plug
void Core::cmd_at(char **tok, int n, Out &out, int64_t now) {
  int64_t t;
  if (n < 3 || !parse_i64(tok[1], &t) || t < 0) return out.line("err args");
  if (t < last_at_) return out.line("err order");
  if (running_ && t0_ + t < now) return out.line("err late");
  if (sched_tail_ - sched_head_ >= sched_cap_) return out.line("err full");
  Event e = {t, kMidi, {0, 0, 0, 0}, 0};
  if (!strcmp(tok[2], "unplug")) {
    int64_t ms = 0;
    if (n > 4 || (n == 4 && (!parse_i64(tok[3], &ms) || ms < 0 || ms > 600000))) return out.line("err args");
    e.type = kUnplug;
    e.arg = (uint32_t)ms;
  } else if (!strcmp(tok[2], "plug")) {
    if (n != 3) return out.line("err args");
    e.type = kPlug;
  } else {
    int64_t cable;
    uint8_t msg[4];
    size_t len;
    if (n != 4 || !parse_i64(tok[2], &cable)) return out.line("err args");
    if (cable < 0 || cable >= kCables) return out.line("err cable");
    if (!parse_hex(tok[3], msg, sizeof msg, &len)) return out.line("err hex");
    if (!pack((uint8_t)cable, msg, len, e.pkt)) return out.line("err msg");
  }
  sched_[sched_tail_++ % sched_cap_] = e;
  last_at_ = t;
  out.line("ok at");
}

// rep <start_rel_us> <interval_ns> <count> <cable> <hex>: a MIDI clock, or any
// message on a fixed grid, without one `at` per tick.
void Core::cmd_rep(char **tok, int n, Out &out) {
  int64_t start, interval, count, cable;
  uint8_t msg[4];
  size_t len;
  if (n != 6 || !parse_i64(tok[1], &start) || !parse_i64(tok[2], &interval) ||
      !parse_i64(tok[3], &count) || !parse_i64(tok[4], &cable) || start < 0 ||
      interval < 1000 || count < 1 || count > 0x7fffffff)
    return out.line("err args");
  if (cable < 0 || cable >= kCables) return out.line("err cable");
  if (!parse_hex(tok[5], msg, sizeof msg, &len)) return out.line("err hex");
  Repeater r = {};
  if (!pack((uint8_t)cable, msg, len, r.pkt)) return out.line("err msg");
  for (int i = 0; i < kRepeaters; ++i) {
    if (rep_[i].on) continue;
    r.on = true;
    r.start_rel_us = start;
    r.interval_ns = interval;
    r.count = (uint32_t)count;
    r.retry_since = -1;
    rep_[i] = r;
    char b[24];
    snprintf(b, sizeof b, "ok rep %d", i);
    return out.line(b);
  }
  out.line("err busy");
}

void Core::cmd_status(int64_t now, Out &out) {
  Counters c = counters();
  int reps = 0;
  for (auto &r : rep_) reps += r.on;
  char b[320];
  snprintf(b, sizeof b,
           "ok status t=%lld running=%d mounted=%d sent=%u recv=%u not_sent=%u tx_retries=%u "
           "unplugs=%u mounts=%u sched=%u sched_free=%u reps=%d log=%u log_lost_tx=%u log_lost_rx=%u",
           (long long)now, running_, port_.mounted() ? 1 : 0, (unsigned)c.sent, (unsigned)c.recv,
           (unsigned)c.not_sent, (unsigned)c.tx_retries, (unsigned)c.unplugs, (unsigned)c.mounts,
           (unsigned)(sched_tail_ - sched_head_), (unsigned)(sched_cap_ - (sched_tail_ - sched_head_)), reps,
           (unsigned)(tx_.size() + rx_.size()), (unsigned)c.log_lost_tx, (unsigned)c.log_lost_rx);
  out.line(b);
}

// log [max]: up to max records (default 256), oldest first across sent and
// received, each removed as it goes out; then "ok log n=<k> more=<left>".
void Core::cmd_log(char **tok, int n, Out &out) {
  int64_t max = 256;
  if (n > 2 || (n == 2 && (!parse_i64(tok[1], &max) || max < 1))) return out.line("err args");
  if (draining_) return out.line("err busy");
  draining_ = true;
  drain_left_ = (size_t)max;
  drain_sent_ = 0;
  (void)out;
}

bool Core::emit_record(Out &out) {
  Record a, b, r;
  bool ha = tx_.peek(&a), hb = rx_.peek(&b);
  if (!ha && !hb) return false;
  if (ha && (!hb || a.t_us <= b.t_us)) tx_.pop(&r);
  else rx_.pop(&r);
  char line[48];
  snprintf(line, sizeof line, "%c %lld %02X%02X%02X%02X", r.kind, (long long)r.t_us, r.pkt[0], r.pkt[1],
           r.pkt[2], r.pkt[3]);
  out.line(line);
  return true;
}

void Core::command(const char *text, int64_t now, Out &out) {
  char buf[kLineMax + 1];
  strncpy(buf, text, kLineMax);
  buf[kLineMax] = 0;
  char *tok[8];
  int n = tokenize(buf, tok, 8);
  if (n == 0) return;
  if (n > 8) return out.line("err args");
  const char *cmd = tok[0];
  char b[192];  // id's reply carries the board's description (ip, host on Wi-Fi)
  if (!strcmp(cmd, "ping")) {
    out.line("ok pong");
  } else if (!strcmp(cmd, "id")) {
    char d[144];
    port_.describe(d, sizeof d);
    snprintf(b, sizeof b, "ok id %s cables=%d", d, kCables);
    out.line(b);
  } else if (!strcmp(cmd, "time")) {
    snprintf(b, sizeof b, "ok time %lld", (long long)now);
    out.line(b);
  } else if (!strcmp(cmd, "at")) {
    cmd_at(tok, n, out, now);
  } else if (!strcmp(cmd, "rep")) {
    cmd_rep(tok, n, out);
  } else if (!strcmp(cmd, "go")) {
    int64_t delay = 10000;
    if (n > 2 || (n == 2 && (!parse_i64(tok[1], &delay) || delay < 0))) return out.line("err args");
    if (running_) return out.line("err running");
    running_ = true;
    t0_ = now + delay;
    retry_since_ = -1;
    for (auto &r : rep_) r.sent = 0;
    snprintf(b, sizeof b, "ok go t0=%lld", (long long)t0_);
    out.line(b);
  } else if (!strcmp(cmd, "stop")) {
    running_ = false;
    out.line("ok stop");
  } else if (!strcmp(cmd, "clear")) {
    clear_all();
    out.line("ok clear");
  } else if (!strcmp(cmd, "send")) {
    int64_t cable;
    uint8_t msg[4], pkt[4];
    size_t len;
    if (n != 3 || !parse_i64(tok[1], &cable)) return out.line("err args");
    if (cable < 0 || cable >= kCables) return out.line("err cable");
    if (!parse_hex(tok[2], msg, sizeof msg, &len)) return out.line("err hex");
    if (!pack((uint8_t)cable, msg, len, pkt)) return out.line("err msg");
    if (!port_.mounted() || !port_.send(pkt)) {
      ++c_.not_sent;
      return out.line("err not-sent");
    }
    ++c_.sent;
    log_tx(now, 's', pkt);
    out.line("ok send");
  } else if (!strcmp(cmd, "unplug")) {
    int64_t ms = 0;
    if (n > 2 || (n == 2 && (!parse_i64(tok[1], &ms) || ms < 0 || ms > 600000))) return out.line("err args");
    unplug(now, (uint32_t)ms);
    out.line("ok unplug");
  } else if (!strcmp(cmd, "plug")) {
    plug(now);
    out.line("ok plug");
  } else if (!strcmp(cmd, "status")) {
    cmd_status(now, out);
  } else if (!strcmp(cmd, "log")) {
    cmd_log(tok, n, out);
  } else {
    out.line("err unknown");
  }
}

int64_t Core::next_due() const {
  int64_t due = INT64_MAX;
  if (draining_) return INT64_MIN;
  if (reconnect_at_ >= 0) due = reconnect_at_;
  if (!running_) return due;
  if (sched_head_ != sched_tail_) {
    int64_t t = t0_ + sched_[sched_head_ % sched_cap_].t_rel_us;
    if (t < due) due = t;
  }
  for (auto &r : rep_) {
    if (!r.on) continue;
    int64_t t = t0_ + r.start_rel_us + (int64_t)r.sent * r.interval_ns / 1000;
    if (t < due) due = t;
  }
  return due;
}

// One packet, or a retry: true when the event is done with (sent or given up).
bool Core::try_send(const uint8_t pkt[4], int64_t now, int64_t *retry_since) {
  if (!port_.mounted()) {
    ++c_.not_sent;
    *retry_since = -1;
    return true;
  }
  if (port_.send(pkt)) {
    ++c_.sent;
    log_tx(now, 's', pkt);
    *retry_since = -1;
    return true;
  }
  ++c_.tx_retries;
  if (*retry_since < 0) *retry_since = now;
  if (now - *retry_since > kTxRetryUs) {
    ++c_.not_sent;
    *retry_since = -1;
    return true;
  }
  return false;
}

void Core::poll(int64_t now, Out &out) {
  int64_t um = unmount_at_.exchange(-1), m = mount_at_.exchange(-1);
  if (um >= 0 && (m < 0 || um <= m)) log_tx(um, 'U', nullptr), um = -1;
  if (m >= 0) log_tx(m, 'M', nullptr);
  if (um >= 0) log_tx(um, 'U', nullptr);
  if (reconnect_at_ >= 0 && now >= reconnect_at_) plug(now);
  if (running_) {
    while (sched_head_ != sched_tail_) {
      Event &e = sched_[sched_head_ % sched_cap_];
      if (t0_ + e.t_rel_us > now) break;
      if (e.type == kMidi) {
        if (!try_send(e.pkt, now, &retry_since_)) break;
      } else if (e.type == kUnplug) {
        unplug(now, e.arg);
      } else {
        plug(now);
      }
      ++sched_head_;
    }
    for (auto &r : rep_) {
      while (r.on && t0_ + r.start_rel_us + (int64_t)r.sent * r.interval_ns / 1000 <= now) {
        if (!try_send(r.pkt, now, &r.retry_since)) break;
        ++r.sent;
        if (--r.count == 0) r.on = false;
      }
    }
  }
  if (draining_) {
    // A few lines per call, so a long log never holds up a due send.
    for (int i = 0; i < 8 && drain_left_ && out.can_write(); ++i) {
      if (!emit_record(out)) {
        drain_left_ = 0;
        break;
      }
      --drain_left_;
      ++drain_sent_;
    }
    if (!drain_left_ || (tx_.size() + rx_.size() == 0)) {
      if (!out.can_write()) return;
      char b[64];
      snprintf(b, sizeof b, "ok log n=%u more=%u", (unsigned)drain_sent_, (unsigned)(tx_.size() + rx_.size()));
      out.line(b);
      draining_ = false;
    }
  }
}

}  // namespace robot
