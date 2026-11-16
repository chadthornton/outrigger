// Host tests for the bench robot's line protocol and schedule
// (firmware/bench_robot/robot_core.cpp). Run with `make test`.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "robot_core.h"

using namespace robot;

static int failures = 0;

static void expect(bool ok, const char *what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++failures;
  }
}

static void expect_eq(const std::string &got, const std::string &want, const char *what) {
  if (got != want) {
    printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
    ++failures;
  }
}

struct FakePort : Port {
  bool is_mounted = true, connected = true;
  int full_for = 0;  // refuse this many sends, as a full TX FIFO does
  std::vector<std::string> sent;
  std::vector<bool> connects;
  bool send(const uint8_t p[4]) override {
    if (full_for > 0) {
      --full_for;
      return false;
    }
    char b[16];
    snprintf(b, sizeof b, "%02X%02X%02X%02X", p[0], p[1], p[2], p[3]);
    sent.push_back(b);
    return true;
  }
  bool mounted() override { return is_mounted && connected; }
  void set_connected(bool on) override {
    connected = on;
    connects.push_back(on);
  }
  void describe(char *out, size_t n) override { snprintf(out, n, "mac=02:00:00:00:00:09 fw=abc1234"); }
};

struct Lines : Out {
  std::vector<std::string> v;
  bool writable = true;
  void line(const char *s) override { v.push_back(s); }
  bool can_write() override { return writable; }
  std::string last() const { return v.empty() ? "" : v.back(); }
};

struct Rig {
  FakePort port;
  Event sched[8];
  Record tx[16], rx[16];
  Core core{port, sched, 8, tx, 16, rx, 16};
  Lines out;
  std::string cmd(const char *line, int64_t now = 0) {
    out.v.clear();
    for (const char *c = line; *c; ++c) core.feed(*c, now, out);
    core.feed('\n', now, out);
    core.poll(now, out);
    return out.last();
  }
};

static void test_pack() {
  uint8_t p[4], m[3];
  m[0] = 0x90, m[1] = 0x3C, m[2] = 0x64;
  expect(pack(1, m, 3, p) && p[0] == 0x19 && p[1] == 0x90 && p[3] == 0x64, "note on, cable 1");
  m[0] = 0xC2, m[1] = 5;
  expect(pack(0, m, 2, p) && p[0] == 0x0C && p[3] == 0, "program change is 2 bytes");
  expect(!pack(0, m, 3, p), "program change with 3 bytes refused");
  m[0] = 0xF8;
  expect(pack(0, m, 1, p) && p[0] == 0x0F, "clock is CIN F");
  m[0] = 0xF2, m[1] = 1, m[2] = 2;
  expect(pack(0, m, 3, p) && p[0] == 0x03, "song position is CIN 3");
  m[0] = 0xF0;
  expect(!pack(0, m, 1, p), "SysEx refused");
  m[0] = 0x3C;
  expect(!pack(0, m, 1, p), "running status refused");
  m[0] = 0x90, m[1] = 0x80, m[2] = 1;
  expect(!pack(0, m, 3, p), "a status byte as data refused");
  m[0] = 0x90, m[1] = 1, m[2] = 1;
  expect(!pack(2, m, 3, p), "cable 2 refused");
}

static void test_hex() {
  uint8_t b[4];
  size_t n;
  expect(parse_hex("903c64", b, 4, &n) && n == 3 && b[1] == 0x3C, "lower-case hex");
  expect(!parse_hex("903", b, 4, &n), "odd length");
  expect(!parse_hex("90zz", b, 4, &n), "non-hex digit");
  expect(!parse_hex("", b, 4, &n), "empty");
  expect(!parse_hex("0102030405", b, 4, &n), "too long for the buffer");
}

static void test_basic_commands() {
  Rig r;
  expect_eq(r.cmd("ping"), "ok pong", "ping");
  expect_eq(r.cmd("id"), "ok id mac=02:00:00:00:00:09 fw=abc1234 cables=2", "id");
  expect_eq(r.cmd("time", 123456789012LL), "ok time 123456789012", "time is 64-bit");
  expect_eq(r.cmd("bogus"), "err unknown", "unknown command");
  expect_eq(r.cmd("  ping  "), "ok pong", "spaces around a command");
  r.out.v.clear();
  r.core.feed('\n', 0, r.out);
  r.core.feed('\r', 0, r.out);
  expect(r.out.v.empty(), "blank lines get no reply");
  expect_eq(r.cmd("send 0 1 2 3 4 5 6 7 8"), "err args", "too many tokens");
}

static void test_long_line() {
  Rig r;
  std::string longl(kLineMax + 5, 'x');
  expect_eq(r.cmd(longl.c_str()), "err too-long", "over-long line refused whole");
  expect_eq(r.cmd("ping"), "ok pong", "the next line is fine");
}

static void test_at_errors() {
  Rig r;
  expect_eq(r.cmd("at"), "err args", "at without args");
  expect_eq(r.cmd("at x 0 903C64"), "err args", "bad time");
  expect_eq(r.cmd("at -5 0 903C64"), "err args", "negative time");
  expect_eq(r.cmd("at 0 2 903C64"), "err cable", "bad cable");
  expect_eq(r.cmd("at 0 0 90XX64"), "err hex", "bad hex");
  expect_eq(r.cmd("at 0 0 F0"), "err msg", "SysEx");
  expect_eq(r.cmd("at 0 0 903C"), "err msg", "short note");
  expect_eq(r.cmd("at 100 0 903C64"), "ok at", "good at");
  expect_eq(r.cmd("at 50 0 803C00"), "err order", "time going back");
  expect_eq(r.cmd("at 100 unplug x"), "err args", "unplug ms not a number");
  expect_eq(r.cmd("at 100 plug 5"), "err args", "plug takes nothing");
  for (int i = 0; i < 7; ++i) r.cmd("at 200 0 F8");
  expect_eq(r.cmd("at 300 0 F8"), "err full", "schedule full at 8");
}

static void test_schedule_runs_on_time() {
  Rig r;
  r.cmd("at 0 0 903C64");
  r.cmd("at 1000 1 803C00");
  r.cmd("at 1000 0 F8");
  expect_eq(r.cmd("go 500", 10000), "ok go t0=10500", "go sets t0");
  expect_eq(r.cmd("go", 10000), "err running", "go twice");
  r.core.poll(10499, r.out);
  expect(r.port.sent.empty(), "nothing before t0");
  expect(r.core.next_due() == 10500, "next due is t0");
  r.core.poll(10500, r.out);
  expect(r.port.sent.size() == 1 && r.port.sent[0] == "09903C64", "first at t0");
  r.core.poll(11499, r.out);
  expect(r.port.sent.size() == 1, "second not early");
  r.core.poll(11500, r.out);
  expect(r.port.sent.size() == 3 && r.port.sent[1] == "18803C00" && r.port.sent[2] == "0FF80000",
         "both at 1000, in order, cable 1 in the high nibble");
  expect(r.core.counters().sent == 3, "sent counted");
  expect_eq(r.cmd("at 1000 0 F8", 20000), "err late", "an at in the past while running");
}

static void test_log_drain_in_time_order() {
  Rig r;
  r.cmd("at 0 0 903C64");
  r.cmd("at 2000 0 803C00");
  r.cmd("go 0", 1000);
  r.core.poll(1000, r.out);
  uint8_t in[4] = {0x19, 0x90, 0x3C, 0x64};
  r.core.received(in, 1500);
  r.core.poll(3000, r.out);
  r.core.mount_changed(false, 4000);
  r.cmd("log", 5000);
  std::vector<std::string> want = {"s 1000 09903C64", "r 1500 19903C64", "s 3000 08803C00", "U 4000 00000000",
                                   "ok log n=4 more=0"};
  expect(r.out.v == want, "log merges sent and received by time");
  if (r.out.v != want)
    for (auto &l : r.out.v) printf("  got: %s\n", l.c_str());
  expect_eq(r.cmd("log", 6000), "ok log n=0 more=0", "log drains: second read is empty");
}

static void test_log_max_and_backpressure() {
  Rig r;
  uint8_t in[4] = {0x09, 0xF8, 0, 0};
  for (int i = 0; i < 12; ++i) r.core.received(in, 100 + i);
  r.cmd("log 5");
  expect(r.out.v.size() == 6 && r.out.last() == "ok log n=5 more=7", "log 5 sends 5 and says 7 left");
  r.out.v.clear();
  r.out.writable = false;
  r.core.command("log", 0, r.out);
  r.core.poll(0, r.out);
  expect(r.out.v.empty(), "a full UART buffer holds the drain");
  expect(r.core.next_due() == INT64_MIN, "a drain in progress wants polling");
  r.out.writable = true;
  r.core.poll(0, r.out);
  expect(r.out.v.size() == 8, "the 7 left and the ok");
  expect_eq(r.out.last(), "ok log n=7 more=0", "the drain ends with its ok");
  for (int i = 0; i < 10; ++i) r.core.received(in, 150 + i);
  r.out.v.clear();
  r.core.command("log", 0, r.out);
  r.core.poll(0, r.out);
  expect(r.out.v.size() == 8, "at most 8 records per poll");
  r.core.poll(0, r.out);
  expect(r.out.v.size() == 11 && r.out.last() == "ok log n=10 more=0", "the rest on the next poll");
  for (int i = 0; i < 20; ++i) r.core.received(in, 200 + i);
  expect(r.core.counters().log_lost_rx == 4, "a full receive log counts what it lost");
  expect(r.core.counters().recv == 42, "received counts every packet, logged or not");
}

static void test_tx_full_retry_then_drop() {
  Rig r;
  r.cmd("at 0 0 F8");
  r.cmd("at 0 0 FA");
  r.port.full_for = 3;
  r.cmd("go 0");  // polls at 0
  r.core.poll(10, r.out);
  r.core.poll(20, r.out);
  expect(r.port.sent.empty() && r.core.counters().tx_retries == 3, "a full FIFO retries");
  r.core.poll(30, r.out);
  expect(r.port.sent.size() == 2, "then sends both");
  r.cmd("log");
  expect(r.out.v.size() == 3 && r.out.v[0] == "s 30 0FF80000", "logged at the time it went, not its slot");
  Rig q;
  q.cmd("at 0 0 F8");
  q.port.full_for = 1000;
  q.cmd("go 0");
  q.core.poll(0, q.out);
  q.core.poll(kTxRetryUs + 1, q.out);
  expect(q.core.counters().not_sent == 1 && q.core.counters().sent == 0, "dropped after the retry window");
  q.cmd("log");
  expect_eq(q.out.v.front(), "ok log n=0 more=0", "a dropped send isn't in the log");
}

static void test_unplug() {
  Rig r;
  expect_eq(r.cmd("unplug 250", 1000), "ok unplug", "unplug");
  expect(!r.port.connected, "disconnected");
  expect(r.core.next_due() == 251000, "reconnect due");
  expect_eq(r.cmd("send 0 F8", 2000), "err not-sent", "nothing goes while unplugged");
  r.core.poll(251000, r.out);
  expect(r.port.connected, "reconnected on time");
  r.cmd("unplug", 300000);
  expect(r.core.next_due() == INT64_MAX, "unplug with no time stays out");
  r.cmd("plug", 400000);
  expect(r.port.connected, "plug");
  r.cmd("log");
  std::vector<std::string> want = {"u 1000 00000000", "p 251000 00000000", "u 300000 00000000",
                                   "p 400000 00000000", "ok log n=4 more=0"};
  expect(r.out.v == want, "unplugs and plugs are in the log");
  expect(r.core.counters().unplugs == 2, "unplugs counted");
}

static void test_scheduled_unplug_skips_sends() {
  Rig r;
  r.cmd("at 0 unplug 10");
  r.cmd("at 5000 0 F8");
  r.cmd("at 20000 0 FC");
  r.cmd("go 0");
  r.core.poll(0, r.out);
  r.port.is_mounted = false;  // the host takes a while to see it back
  r.core.poll(5000, r.out);
  expect(r.core.counters().not_sent == 1, "a send while unplugged is counted, not queued");
  r.core.poll(10000, r.out);
  r.port.is_mounted = true;
  r.core.poll(20000, r.out);
  expect(r.port.sent.size() == 1 && r.port.sent[0] == "0FFC0000", "the send after the replug goes");
}

static void test_repeater_clock() {
  Rig r;
  // 120 BPM clock: 20833.333 us apart, 4 ticks.
  expect_eq(r.cmd("rep 0 20833333 4 1 F8"), "ok rep 0", "rep");
  expect_eq(r.cmd("rep 0 999 4 1 F8"), "err args", "an interval under 1 us");
  r.cmd("go 0", 0);  // polls at 0: the first tick
  std::vector<int64_t> at;
  if (r.port.sent.size() == 1) at.push_back(0);
  for (int64_t t = 1; t <= 70000; ++t) {
    size_t before = r.port.sent.size();
    r.core.poll(t, r.out);
    if (r.port.sent.size() != before) at.push_back(t);
  }
  expect(at.size() == 4 && at[0] == 0 && at[1] == 20833 && at[2] == 41666 && at[3] == 62499,
         "ticks on the nanosecond grid, no drift");
  expect(r.port.sent[0] == "1FF80000", "on cable 1");
  for (int i = 0; i < 4; ++i) r.cmd("rep 10000000 1000000 1 0 F8");
  expect_eq(r.cmd("rep 10000000 1000000 1 0 F8"), "err busy", "four repeaters at most");
}

static void test_clear_and_status() {
  Rig r;
  r.cmd("at 0 0 F8");
  r.cmd("go 0");
  r.core.poll(0, r.out);
  std::string s = r.cmd("status", 7);
  expect(s.rfind("ok status t=7 running=1 mounted=1 sent=1 recv=0 not_sent=0", 0) == 0, "status");
  expect(s.find(" log=1 ") != std::string::npos, "status counts the log");
  expect_eq(r.cmd("clear"), "ok clear", "clear");
  s = r.cmd("status", 8);
  expect(s.rfind("ok status t=8 running=0 mounted=1 sent=0", 0) == 0, "clear resets");
  expect(s.find("sched=0 sched_free=8") != std::string::npos, "clear empties the schedule");
  expect_eq(r.cmd("at 0 0 F8"), "ok at", "clear resets the at order");
}

int main() {
  test_pack();
  test_hex();
  test_basic_commands();
  test_long_line();
  test_at_errors();
  test_schedule_runs_on_time();
  test_log_drain_in_time_order();
  test_log_max_and_backpressure();
  test_tx_full_retry_then_drop();
  test_unplug();
  test_scheduled_unplug_skips_sends();
  test_repeater_clock();
  test_clear_and_status();
  if (failures) {
    printf("test_robot_proto: %d failure(s)\n", failures);
    return 1;
  }
  printf("test_robot_proto: all passed\n");
  return 0;
}
