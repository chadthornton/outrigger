// Host-side tests for lib/midi_core. Run with `make test`.
#include <stdio.h>
#include <string.h>

#include "midi_core.h"

using namespace midi_core;

static int failures = 0;

static void expect_eq(const char *got, const char *want, const char *what) {
  if (strcmp(got, want) != 0) {
    printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    ++failures;
  }
}

static const char *fmt(uint8_t s, uint8_t d1, uint8_t d2, const uint8_t *raw = nullptr,
                       size_t n = 0) {
  static char buf[kLineLen];
  format_message(s, d1, d2, raw, n, buf);
  return buf;
}

static void test_note_names() {
  char n[5];
  note_name(60, n); expect_eq(n, "C4", "note 60");
  note_name(61, n); expect_eq(n, "C#4", "note 61");
  note_name(0, n); expect_eq(n, "C-1", "note 0");
  note_name(127, n); expect_eq(n, "G9", "note 127");
}

static void test_channel_messages() {
  expect_eq(fmt(0x90, 60, 100), "01 ON  C4   v100", "note on ch1");
  expect_eq(fmt(0x9F, 61, 1), "16 ON  C#4  v1", "note on ch16");
  expect_eq(fmt(0x90, 60, 0), "01 OFF C4   v0", "note on vel 0 is off");
  expect_eq(fmt(0x80, 60, 64), "01 OFF C4   v64", "note off");
  expect_eq(fmt(0xA0, 60, 30), "01 AT  C4   30", "poly aftertouch");
  expect_eq(fmt(0xB0, 74, 63), "01 CC  74 = 63", "control change");
  expect_eq(fmt(0xC1, 10, 0), "02 PC  10", "program change");
  expect_eq(fmt(0xD0, 90, 0), "01 CP  90", "channel pressure");
  expect_eq(fmt(0xE0, 0x00, 0x40), "01 PB  +0", "pitch bend centre");
  expect_eq(fmt(0xE0, 0x7F, 0x7F), "01 PB  +8191", "pitch bend max");
  expect_eq(fmt(0xE0, 0x00, 0x00), "01 PB  -8192", "pitch bend min");
}

static void test_system_messages() {
  const uint8_t sysex[] = {0xF0, 0x22, 0x24};
  expect_eq(fmt(0xF0, 0x22, 0x24, sysex, 3), "SYS F0 22 24", "sysex chunk");
  const uint8_t clock[] = {0xF8};
  expect_eq(fmt(0xF8, 0, 0, clock, 1), "SYS F8", "clock with raw");
  expect_eq(fmt(0xFE, 0, 0), "SYS FE", "active sensing without raw");
  const uint8_t long_raw[] = {1, 2, 3, 4, 5, 6, 7, 8};
  const char *line = fmt(0xF0, 0, 0, long_raw, 8);
  if (strlen(line) >= kLineLen) {
    printf("FAIL long sysex overflows line: %s\n", line);
    ++failures;
  }
}

static void test_line_log() {
  LineLog<3> log;
  expect_eq(log.newest(0), "", "empty log");
  log.push("a");
  log.push("b");
  expect_eq(log.newest(0), "b", "newest");
  expect_eq(log.newest(1), "a", "second newest");
  log.push("c");
  log.push("d");  // drops "a"
  if (log.count() != 3 || log.total() != 4) {
    printf("FAIL log count %zu total %u\n", log.count(), log.total());
    ++failures;
  }
  expect_eq(log.newest(0), "d", "newest after wrap");
  expect_eq(log.newest(2), "b", "oldest after wrap");
  expect_eq(log.newest(3), "", "past the end");
  log.push("this line is much longer than twenty characters");
  if (strlen(log.newest(0)) != kLineLen - 1) {
    printf("FAIL long line not truncated: %s\n", log.newest(0));
    ++failures;
  }
}

static void expect_packet(bool ok, const uint8_t *got, bool want_ok,
                          const uint8_t (&want)[4], const char *what) {
  if (ok != want_ok || (want_ok && memcmp(got, want, 4) != 0)) {
    printf("FAIL %s: ok=%d packet=%02X %02X %02X %02X\n", what, ok, got[0], got[1],
           got[2], got[3]);
    ++failures;
  }
}

static bool route(uint8_t cable, uint8_t status, uint8_t d1, uint8_t d2, uint8_t out[4],
                  int cin = -1) {
  memset(out, 0xAA, 4);
  return route_to_target(cable, cin < 0 ? status >> 4 : cin, status, d1, d2, out);
}

static void test_route_channel_voice() {
  uint8_t p[4];
  expect_packet(route(0, 0x90, 60, 100, p), p, true, {0x09, 0x90, 60, 100}, "note on");
  expect_packet(route(0, 0x83, 60, 0, p), p, true, {0x08, 0x83, 60, 0}, "note off ch4");
  expect_packet(route(0, 0xA0, 60, 30, p), p, true, {0x0A, 0xA0, 60, 30}, "poly pressure");
  expect_packet(route(0, 0xB0, 74, 63, p), p, true, {0x0B, 0xB0, 74, 63}, "cc");
  expect_packet(route(0, 0xC0, 10, 99, p), p, true, {0x0C, 0xC0, 10, 0}, "program change, 2 bytes");
  expect_packet(route(0, 0xD0, 90, 99, p), p, true, {0x0D, 0xD0, 90, 0}, "channel pressure, 2 bytes");
  expect_packet(route(0, 0xE0, 0, 64, p), p, true, {0x0E, 0xE0, 0, 64}, "pitch bend");
  expect_packet(route(0, 0x90, 0xBC, 0xFF, p), p, true, {0x09, 0x90, 0x3C, 0x7F},
                "data bytes masked to 7 bits");
}

// The FM-1 must never receive SysEx (the stock ROM-boot key and Skiff's
// installer both arrive that way), so every system message is dropped.
static void test_route_blocks_system_messages() {
  uint8_t p[4];
  const uint8_t none[4] = {};
  for (int status = 0xF0; status <= 0xFF; ++status)
    expect_packet(route(0, status, 0x22, 0x24, p), p, false, none, "system message dropped");
  expect_packet(route(0, 0xF0, 0x22, 0x24, p), p, false, none, "UBOOT key start");
  expect_packet(route(0, 0xF7, 0, 0, p), p, false, none, "sysex end");
}

static void test_route_drops_other_input() {
  uint8_t p[4];
  const uint8_t none[4] = {};
  expect_packet(route(1, 0x90, 60, 100, p), p, false, none, "cable 1 (DAW port) dropped");
  expect_packet(route(0, 0x3C, 100, 0, p), p, false, none, "data byte as status dropped");
  expect_packet(route(0, 0x00, 0, 0, p), p, false, none, "zero status dropped");
  expect_packet(route(0, 0x90, 0, 0, p, 0xF), p, false, none, "single-byte packet dropped");
  expect_packet(route(0, 0x90, 60, 100, p, 0x4), p, false, none, "sysex-continue CIN dropped");
  expect_packet(route(0, 0x90, 60, 100, p, 0x0), p, false, none, "padding CIN dropped");
}

static void test_router() {
  Router r;
  uint8_t p[4];
  const uint8_t kLaunchkey = 3, kFm1 = 2;
  if (r.route(kLaunchkey, 0, 0x9, 0x90, 60, 100, p) != 0) {
    printf("FAIL router sends before anything is known\n"); ++failures;
  }
  r.device_connected(kLaunchkey, 0x1235, 0x0142);
  if (r.route(kLaunchkey, 0, 0x9, 0x90, 60, 100, p) != 0) {
    printf("FAIL router sends with no FM-1 connected\n"); ++failures;
  }
  r.device_connected(kFm1, 0x1209, 0x0001);
  if (r.route(kLaunchkey, 0, 0x9, 0x90, 60, 100, p) != kFm1) {
    printf("FAIL controller note not routed to the FM-1\n"); ++failures;
  }
  if (r.route(kFm1, 0, 0x9, 0x90, 60, 100, p) != 0) {
    printf("FAIL FM-1 routed back to itself\n"); ++failures;
  }
  if (r.route(9, 0, 0x9, 0x90, 60, 100, p) != 0) {
    printf("FAIL unknown source address routed\n"); ++failures;
  }
  if (r.route(kLaunchkey, 0, 0x4, 0xF0, 0x22, 0x24, p) != 0) {
    printf("FAIL SysEx routed through the router\n"); ++failures;
  }
  char why = 0;
  struct { uint8_t src, cable, cin, status; char want; } cases[] = {
      {9, 0, 0x9, 0x90, 'u'},          {kFm1, 0, 0x9, 0x90, 't'},
      {kLaunchkey, 1, 0x9, 0x90, 'c'}, {kLaunchkey, 0, 0x8, 0x90, 'k'},
      {kLaunchkey, 0, 0xF, 0xF8, 's'}, {kLaunchkey, 0, 0x4, 0xF0, 's'},
  };
  for (auto &k : cases) {
    why = 0;
    if (r.route(k.src, k.cable, k.cin, k.status, 60, 100, p, &why) != 0 || why != k.want) {
      printf("FAIL drop reason: want '%c' got '%c'\n", k.want, why ? why : '0');
      ++failures;
    }
  }
  if (!r.is_target(kFm1) || r.is_target(kLaunchkey)) {
    printf("FAIL is_target\n"); ++failures;
  }
  r.device_disconnected(kFm1);
  why = 0;
  if (r.route(kLaunchkey, 0, 0x9, 0x90, 60, 100, p, &why) != 0 || why != 'n') {
    printf("FAIL router sends after the FM-1 left\n"); ++failures;
  }
  // What /status reports per device: known, and in which role.
  midi_core::Role role = midi_core::Role::Target;
  if (!r.knows(kLaunchkey, role) || role != midi_core::Role::Controller) {
    printf("FAIL router doesn't report the Launchkey as a known controller\n"); ++failures;
  }
  r.device_disconnected(kLaunchkey);
  if (r.knows(kLaunchkey, role) || r.knows(0, role) || r.knows(200, role)) {
    printf("FAIL router reports a gone or invalid address as known\n"); ++failures;
  }
  r.device_connected(kFm1, 0x1209, 0x0001);
  if (r.route(kLaunchkey, 0, 0x9, 0x90, 60, 100, p) != 0) {
    printf("FAIL departed controller still routed\n"); ++failures;
  }
  Router edge;
  edge.device_connected(0xFF, 0x1209, 0x0001);  // "any address" must never be a target
  edge.device_connected(200, 0x1209, 0x0001);
  edge.device_connected(4, 0x1235, 0x0142);
  if (edge.route(4, 0, 0x9, 0x90, 60, 100, p) != 0) {
    printf("FAIL out-of-range address became the target\n"); ++failures;
  }
}

// The synth (Outrigger Synth builds) hears what a controller would send the
// FM-1, by the same rules, with or without an FM-1 connected.
static void test_router_to_synth() {
  constexpr uint8_t kFm1 = 2, kLaunchkey = 3;
  Router r;
  r.device_connected(kLaunchkey, 0x1235, 0x0142);
  if (!r.to_synth(kLaunchkey, 0, 0x9, 0x90, 60, 100)) {
    printf("FAIL synth gets a controller's note with no FM-1\n"); ++failures;
  }
  r.device_connected(kFm1, 0x1209, 0x0001);
  if (!r.to_synth(kLaunchkey, 0, 0xB, 0xB3, 74, 10)) {
    printf("FAIL synth gets a controller's CC beside the FM-1\n"); ++failures;
  }
  struct { uint8_t src, cable, cin, status; } refused[] = {
      {9, 0, 0x9, 0x90}, {kFm1, 0, 0x9, 0x90}, {kLaunchkey, 1, 0x9, 0x90},
      {kLaunchkey, 0, 0x8, 0x90}, {kLaunchkey, 0, 0xF, 0xF8}, {kLaunchkey, 0, 0x4, 0xF0},
  };
  for (auto &k : refused) {
    if (r.to_synth(k.src, k.cable, k.cin, k.status, 60, 100)) {
      printf("FAIL synth got src %u cable %u cin %x status %02x\n", k.src, k.cable, k.cin, k.status);
      ++failures;
    }
  }
  r.device_disconnected(kLaunchkey);
  if (r.to_synth(kLaunchkey, 0, 0x9, 0x90, 60, 100)) {
    printf("FAIL departed controller still reaches the synth\n"); ++failures;
  }
}

static void test_device_roles() {
  if (device_role(0x4C4A, 0xC755) != Role::Target) {
    printf("FAIL stock FM-1 is the target\n"); ++failures;
  }
  if (device_role(0x1209, 0x0001) != Role::Target) {
    printf("FAIL Felucca/Skiff FM-1 is the target\n"); ++failures;
  }
  if (device_role(0x3654, 0x7756) != Role::Target) {
    printf("FAIL M-VAVE Open firmware FM-1 is the target\n"); ++failures;
  }
  if (device_role(0x1235, 0x0142) != Role::Controller) {
    printf("FAIL Launchkey is a controller\n"); ++failures;
  }
  // The Loader switches only from firmwares whose ROM-boot key has been tried.
  if (!can_switch_firmware(0x4C4A, 0xC755) || !can_switch_firmware(0x1209, 0x0001)) {
    printf("FAIL stock and Felucca/Skiff FM-1s can switch firmware\n"); ++failures;
  }
  if (can_switch_firmware(0x3654, 0x7756)) {
    printf("FAIL M-VAVE Open firmware can't switch yet (key untested)\n"); ++failures;
  }
  if (can_switch_firmware(0x1235, 0x0142)) {
    printf("FAIL a controller can't switch firmware\n"); ++failures;
  }
  if (device_role(0x05E3, 0x0610) != Role::Controller) {
    printf("FAIL unknown devices default to controller\n"); ++failures;
  }
}

int main() {
  test_route_channel_voice();
  test_route_blocks_system_messages();
  test_route_drops_other_input();
  test_router_to_synth();
  test_device_roles();
  test_router();
  test_note_names();
  test_channel_messages();
  test_system_messages();
  test_line_log();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all midi_core tests passed\n");
  return 0;
}
