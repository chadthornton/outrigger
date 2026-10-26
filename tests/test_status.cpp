// Host-side tests for lib/midi_core status.h: which chain node is healthy and
// which problem the screen explains. Run with `make test`.
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "status.h"

using namespace midi_core;

static int failures = 0;

#define EXPECT(cond, what)                                   \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s (line %d)\n", what, __LINE__);         \
      ++failures;                                            \
    }                                                        \
  } while (0)

static DeviceView device(uint8_t address, const char *name, Role role, uint8_t port, bool midi_open) {
  DeviceView d{};
  d.address = address;
  strncpy(d.name, name, sizeof d.name - 1);
  d.role = role;
  d.port_id = port;
  d.midi_open = midi_open;
  d.is_midi = true;
  return d;
}

static HostSnapshot hub_with(std::initializer_list<DeviceView> devices) {
  HostSnapshot s{};
  s.uptime_ms = 10000;
  s.hub_present = true;
  s.hub_ports = 4;
  memcpy(s.port_state, "EEpC", 4);
  s.channels_used = 8;
  for (const DeviceView &d : devices) s.devices[s.device_count++] = d;
  return s;
}

static void test_playing_is_all_ok() {
  ChainView v = evaluate(hub_with({device(2, "Felucca", Role::Target, 0x11, true),
                                   device(3, "Launchkey Mini MK4 37", Role::Controller, 0x12, true)}));
  EXPECT(v.power == Level::Ok && v.hub == Level::Ok && v.controllers == Level::Ok &&
             v.router == Level::Ok && v.target == Level::Ok,
         "every node ok while playing");
  EXPECT(!v.has_problem, "no problem while playing");
  EXPECT(v.first_broken == 5, "spine fully live");
}

static void test_nothing_answering() {
  HostSnapshot s{};
  s.uptime_ms = 1000;
  EXPECT(!evaluate(s).has_problem, "no complaint right after boot");
  s.uptime_ms = 6000;
  ChainView v = evaluate(s);
  EXPECT(v.has_problem && v.hub == Level::Bad, "nothing on USB is a hub problem");
  EXPECT(strstr(v.problem.line2, "HOST") != nullptr, "tells you to use the HOST cable");
  EXPECT(v.first_broken == 1, "spine stops at the hub");
}

static void test_port_that_wont_start() {
  HostSnapshot s = hub_with({device(2, "Felucca", Role::Target, 0x11, true),
                             device(3, "Launchkey", Role::Controller, 0x12, true)});
  s.port_state[3] = 'C';
  s.port_stuck_ms[3] = 1000;
  EXPECT(strstr(evaluate(s).problem.line1, "Port 4") == nullptr, "short connect blip is not a problem");
  s.port_stuck_ms[3] = 5000;
  ChainView v = evaluate(s);
  EXPECT(v.has_problem && v.hub == Level::Warn, "stuck port warns on the hub node");
  EXPECT(strstr(v.problem.line1, "Port 4") != nullptr, "names the stuck port");
  EXPECT(strstr(v.problem.line2, "USB-C") != nullptr, "says to use a USB-C port");
  // This hub's own inner hub chip sits on port 4 and never starts: stuck since
  // boot is a fact about the hub, not something the user just plugged in.
  s.port_stuck_ms[3] = 9000;
  v = evaluate(s);
  EXPECT(!v.has_problem && v.hub == Level::Ok, "port stuck since boot is not a problem");
  EXPECT(v.port_off_since_boot[3], "but it is reported as off");
}

static void test_out_of_channels() {
  ChainView v = evaluate(hub_with({device(2, "Felucca", Role::Target, 0x11, true),
                                   device(3, "Launchkey", Role::Controller, 0x12, true),
                                   device(4, "MPK mini", Role::Controller, 0x13, false)}));
  EXPECT(v.has_problem && v.controllers == Level::Bad, "unopened controller is bad");
  EXPECT(strstr(v.problem.line1, "MPK mini") != nullptr, "names the device");
  EXPECT(strstr(v.problem.line2, "1 controller") != nullptr, "states the real limit");
  EXPECT(v.router == Level::Ok && v.target == Level::Ok, "the working pair still routes");
}

static void test_no_fm1() {
  ChainView v = evaluate(hub_with({device(3, "Launchkey", Role::Controller, 0x12, true)}));
  EXPECT(v.has_problem && v.target == Level::Warn, "missing FM-1 warns");
  EXPECT(v.router == Level::Off, "nothing to route to");
  EXPECT(strstr(v.problem.line2, "nowhere") != nullptr, "explains notes go nowhere");
  EXPECT(v.first_broken == 3, "spine stops at the router");
}

static void test_no_controller_is_not_an_error() {
  ChainView v = evaluate(hub_with({device(2, "Felucca", Role::Target, 0x11, true)}));
  EXPECT(v.controllers == Level::Off, "no controller shows grey");
  EXPECT(!v.has_problem, "FM-1 alone is fine");
}

static void test_empty_hub() {
  ChainView v = evaluate(hub_with({}));
  EXPECT(v.has_problem && v.hub == Level::Ok, "hub itself is fine");
  EXPECT(strstr(v.problem.line1, "No devices") != nullptr, "says the hub is empty");
}

static void test_tags() {
  char tag[4];
  make_tag("Launchkey Mini MK4 37", tag);
  EXPECT(strcmp(tag, "LAU") == 0, "tag from name");
  make_tag("MPK mini", tag);
  EXPECT(strcmp(tag, "MPK") == 0, "tag skips spaces");
  make_tag("", tag);
  EXPECT(strcmp(tag, "?") == 0, "empty name");
}

int main() {
  test_playing_is_all_ok();
  test_nothing_answering();
  test_port_that_wont_start();
  test_out_of_channels();
  test_no_fm1();
  test_no_controller_is_not_an_error();
  test_empty_hub();
  test_tags();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all status tests passed\n");
  return 0;
}
