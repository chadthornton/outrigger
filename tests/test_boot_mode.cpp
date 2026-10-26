// Host-side tests for lib/midi_core boot_mode.h: when "Connect to computer"
// keeps the USB host off, and when the request expires. Run with `make test`.
#include <stdio.h>

#include "boot_mode.h"

using namespace midi_core;

static int failures = 0;

#define EXPECT(cond, what)                                   \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s (line %d)\n", what, __LINE__);         \
      ++failures;                                            \
    }                                                        \
  } while (0)

static void test_no_request_is_host() {
  BootDecision d = decide_boot_mode(nullptr, "abc123");
  EXPECT(d.mode == BootMode::Host, "no request -> host");
  EXPECT(!d.clear_request, "nothing to clear");
  d = decide_boot_mode("", "abc123");
  EXPECT(d.mode == BootMode::Host && !d.clear_request, "empty request -> host");
}

static void test_request_from_this_build_survives_power_cycles() {
  // Same build boots again after the cable moved from the hub to the computer.
  BootDecision d = decide_boot_mode("abc123", "abc123");
  EXPECT(d.mode == BootMode::Computer, "same build -> computer");
  EXPECT(!d.clear_request, "kept until the user leaves or an update lands");
}

static void test_update_returns_to_host() {
  // The installer flashed a different build: the request is used up.
  BootDecision d = decide_boot_mode("abc123", "def456");
  EXPECT(d.mode == BootMode::Host, "new build -> host");
  EXPECT(d.clear_request, "stale request cleared");
}

static void test_prefix_is_not_a_match() {
  BootDecision d = decide_boot_mode("abc", "abc123");
  EXPECT(d.mode == BootMode::Host && d.clear_request, "prefix of build ID is a different build");
}

static void test_unknown_running_build_stays_host() {
  BootDecision d = decide_boot_mode("abc123", "");
  EXPECT(d.mode == BootMode::Host && d.clear_request, "running build has no ID -> host");
}

int main() {
  test_no_request_is_host();
  test_request_from_this_build_survives_power_cycles();
  test_update_returns_to_host();
  test_prefix_is_not_a_match();
  test_unknown_running_build_stays_host();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all boot mode tests passed\n");
  return 0;
}
