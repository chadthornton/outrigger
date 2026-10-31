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

// When the resource broker turned a device down, its reason is the problem.
static void test_broker_reason_wins() {
  DeviceView lk = device(3, "Launchkey", Role::Controller, 0x12, false);
  strcpy(lk.why1, "LAU off: room kept for FM-1");
  strcpy(lk.why2, "Plug in the FM-1 first");
  ChainView v = evaluate(hub_with({lk}));
  EXPECT(v.has_problem && strcmp(v.problem.line1, "LAU off: room kept for FM-1") == 0, "broker line 1");
  EXPECT(strcmp(v.problem.line2, "Plug in the FM-1 first") == 0, "broker line 2");
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

// Each problem belongs to the stage Play opens for it.
static void test_problem_stage() {
  HostSnapshot nothing{};
  nothing.uptime_ms = 6000;
  EXPECT(evaluate(nothing).problem_stage == kStageHub, "nothing on USB: the hub");
  EXPECT(evaluate(hub_with({})).problem_stage == kStageHub, "empty hub: the hub");
  HostSnapshot stuck = hub_with({device(2, "Felucca", Role::Target, 0x11, true)});
  stuck.port_state[3] = 'C';
  stuck.port_stuck_ms[3] = 5000;
  ChainView v = evaluate(stuck);
  EXPECT(v.problem_stage == kStageHub && v.stuck_port == 4, "stuck port: the hub, port 4");
  EXPECT(evaluate(hub_with({device(2, "Felucca", Role::Target, 0x11, true),
                            device(4, "MPK mini", Role::Controller, 0x13, false)}))
                 .problem_stage == kStageControllers,
         "a controller that didn't open: controllers");
  EXPECT(evaluate(hub_with({device(2, "Felucca", Role::Target, 0x11, false)})).problem_stage == kStageFm1,
         "an FM-1 that didn't open: the FM-1");
  EXPECT(evaluate(hub_with({device(3, "Launchkey", Role::Controller, 0x12, true)})).problem_stage == kStageFm1,
         "no FM-1: the FM-1");
  EXPECT(evaluate(hub_with({device(2, "Felucca", Role::Target, 0x11, true)})).problem_stage == kStages,
         "no problem: no stage");
}

static void statuses(const HostSnapshot &s, const Activity &a, StageStatus out[kStages]) {
  stage_status(s, evaluate(s), a, out);
}

static void test_stage_status_playing() {
  StageStatus st[kStages];
  HostSnapshot s = hub_with({device(2, "Felucca", Role::Target, 0x11, true),
                             device(3, "Launchkey Mini MK4 37", Role::Controller, 0x12, true)});
  statuses(s, {"LAU", 123456, "Skiff 1.4", false}, st);
  EXPECT(strcmp(st[kStageHub].text, "USB 8/8") == 0 && st[kStageHub].tone == Tone::Full, "hub: full is yellow");
  EXPECT(strcmp(st[kStageControllers].text, "LAU playing") == 0 && st[kStageControllers].tone == Tone::Live,
         "controllers: playing is green");
  EXPECT(strcmp(st[kStageRouter].text, "123k sent") == 0, "router: sent count");
  EXPECT(strcmp(st[kStageFm1].text, "Skiff 1.4") == 0 && st[kStageFm1].tone == Tone::Plain, "FM-1: its firmware");
  s.channels_used = 5;
  statuses(s, {nullptr, 0, nullptr, false}, st);
  EXPECT(strcmp(st[kStageHub].text, "USB 5/8") == 0 && st[kStageHub].tone == Tone::Plain, "hub: room left is grey");
  EXPECT(strcmp(st[kStageControllers].text, "LAU ready") == 0, "controllers: quiet");
  EXPECT(strcmp(st[kStageRouter].text, "0 sent") == 0, "router: nothing sent yet");
  EXPECT(strcmp(st[kStageFm1].text, "ready") == 0, "FM-1: no Loader, no name");
}

// The worst thing first: an issue beats activity, unfinished beats the name.
static void test_stage_status_worst_first() {
  StageStatus st[kStages];
  HostSnapshot six = hub_with({device(2, "Felucca", Role::Target, 0x11, true), device(3, "Launchkey", Role::Controller, 0x12, true),
                               device(4, "Keystep", Role::Controller, 0x13, true), device(5, "Arturia", Role::Controller, 0x14, false),
                               device(6, "BeatStep", Role::Controller, 0x21, true), device(7, "Control", Role::Controller, 0x22, true)});
  statuses(six, {"LAU", 10, "Skiff 1.4", false}, st);
  EXPECT(strcmp(st[kStageControllers].text, "5, 1 issue") == 0 && st[kStageControllers].tone == Tone::Warn,
         "an issue beats playing");
  HostSnapshot one = hub_with({device(2, "Felucca", Role::Target, 0x11, true), device(4, "MPK mini", Role::Controller, 0x13, false)});
  statuses(one, {nullptr, 0, nullptr, false}, st);
  EXPECT(strcmp(st[kStageControllers].text, "MPK not open") == 0, "one controller: names it");
  EXPECT(strcmp(st[kStageRouter].text, "idle") == 0, "router: nothing open to route");

  HostSnapshot fm1 = hub_with({device(2, "Felucca", Role::Target, 0x11, false)});
  statuses(fm1, {nullptr, 0, "Skiff 1.4", false}, st);
  EXPECT(strcmp(st[kStageFm1].text, "no answer") == 0 && st[kStageFm1].tone == Tone::Warn, "FM-1 didn't open");
  statuses(fm1, {nullptr, 0, "Skiff 1.4", true}, st);
  EXPECT(strcmp(st[kStageFm1].text, "unfinished") == 0 && st[kStageFm1].tone == Tone::Bad,
         "an unfinished install beats everything");
  statuses(hub_with({device(3, "Launchkey", Role::Controller, 0x12, true)}), {nullptr, 0, nullptr, false}, st);
  EXPECT(strcmp(st[kStageFm1].text, "not found") == 0 && st[kStageFm1].tone == Tone::Warn, "no FM-1");
  EXPECT(strcmp(st[kStageControllers].text, "LAU ready") == 0, "the controller is still fine");

  HostSnapshot nothing{};
  nothing.uptime_ms = 6000;
  statuses(nothing, {nullptr, 0, nullptr, false}, st);
  EXPECT(strcmp(st[kStageHub].text, "no USB") == 0 && st[kStageHub].tone == Tone::Bad, "nothing on USB");
  EXPECT(strcmp(st[kStageControllers].text, "none") == 0, "no controllers");
  nothing.uptime_ms = 1000;
  statuses(nothing, {nullptr, 0, nullptr, false}, st);
  EXPECT(strcmp(st[kStageHub].text, "starting") == 0, "right after boot: no complaint");

  HostSnapshot stuck = hub_with({device(2, "Felucca", Role::Target, 0x11, true)});
  stuck.port_state[3] = 'C';
  stuck.port_stuck_ms[3] = 5000;
  statuses(stuck, {nullptr, 0, nullptr, false}, st);
  EXPECT(strcmp(st[kStageHub].text, "port 4 stuck") == 0 && st[kStageHub].tone == Tone::Warn, "stuck port");
}

static void test_short_count() {
  char b[12];
  const struct { uint32_t n; const char *want; } cases[] = {
      {0, "0"}, {999, "999"}, {1000, "1k"}, {123456, "123k"}, {999999, "999k"},
      {1000000, "1.0M"}, {1250000, "1.2M"}, {9999999, "9.9M"}, {10000000, "10M"}, {45600000, "45M"}};
  for (const auto &c : cases) {
    short_count(c.n, b, sizeof b);
    if (strcmp(b, c.want) != 0) printf("  %u gave '%s', not '%s'\n", unsigned(c.n), b, c.want);
    EXPECT(strcmp(b, c.want) == 0, "short count");
  }
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
  test_broker_reason_wins();
  test_no_fm1();
  test_no_controller_is_not_an_error();
  test_empty_hub();
  test_tags();
  test_problem_stage();
  test_stage_status_playing();
  test_stage_status_worst_first();
  test_short_count();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all status tests passed\n");
  return 0;
}
