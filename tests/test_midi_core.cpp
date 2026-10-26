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

int main() {
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
