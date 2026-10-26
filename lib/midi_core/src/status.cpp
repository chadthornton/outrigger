#include "status.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace midi_core {

// Grace periods so plugging things in doesn't flash a warning.
constexpr uint32_t kBootGraceMs = 4000;
constexpr uint32_t kStuckPortMs = 3000;
// A port that was already stuck this early has been stuck since power-up.
constexpr uint32_t kSinceBootMs = 5000;

static void set_problem(ChainView &v, const char *a, const char *b) {
  v.has_problem = true;
  snprintf(v.problem.line1, kProblemLen, "%s", a);
  snprintf(v.problem.line2, kProblemLen, "%s", b);
}

ChainView evaluate(const HostSnapshot &s) {
  ChainView v{};
  v.power = Level::Ok;  // if we're drawing, we have power

  const DeviceView *target = nullptr;
  const DeviceView *unopened = nullptr;
  int controllers = 0, open_controllers = 0;
  for (uint8_t i = 0; i < s.device_count && i < kMaxDevices; ++i) {
    const DeviceView &d = s.devices[i];
    if (!d.is_midi) continue;
    if (!d.midi_open && !unopened) unopened = &d;
    if (d.role == Role::Target) {
      if (d.midi_open) target = &d;
    } else {
      ++controllers;
      if (d.midi_open) ++open_controllers;
    }
  }
  const bool anything = s.hub_present || s.device_count > 0;

  v.hub = anything ? Level::Ok : (s.uptime_ms > kBootGraceMs ? Level::Bad : Level::Off);
  v.controllers = controllers == 0 ? Level::Off
                  : open_controllers < controllers ? Level::Bad
                                                   : Level::Ok;
  v.target = target ? Level::Ok : (anything ? Level::Warn : Level::Off);
  v.router = target && open_controllers > 0 ? Level::Ok : Level::Off;

  int stuck_port = -1;
  for (uint8_t p = 0; p < s.hub_ports && p < 4; ++p) {
    if (s.port_state[p] != 'C' || s.port_stuck_ms[p] < kStuckPortMs) continue;
    if (s.uptime_ms - s.port_stuck_ms[p] < kSinceBootMs) v.port_off_since_boot[p] = true;
    else stuck_port = p;
  }

  // The one problem worth explaining, most fundamental first.
  char buf[kProblemLen];
  if (v.hub == Level::Bad) {
    set_problem(v, "Nothing on my USB-C.", "Plug in the hub's HOST cable.");
  } else if (stuck_port >= 0) {
    v.hub = Level::Warn;
    snprintf(buf, sizeof buf, "Port %d: device won't start.", stuck_port + 1);
    set_problem(v, buf, "Move it to a USB-C port.");
  } else if (unopened) {
    snprintf(buf, sizeof buf, "%s: MIDI not opened.", unopened->name);
    set_problem(v, buf, s.channels_used >= 8 ? "USB full: FM-1 + 1 controller." : "It may not be class-compliant.");
    if (unopened->role == Role::Target) v.target = Level::Bad;
  } else if (anything && s.device_count == 0) {
    set_problem(v, "No devices on the hub.", "Use the hub's USB-C ports.");
  } else if (anything && !target) {
    set_problem(v, "No FM-1 found.", controllers ? "Controller notes go nowhere." : "Plug the FM-1 into USB-C.");
  }

  const Level chain[5] = {v.power, v.hub, v.controllers, v.router, v.target};
  v.first_broken = 5;
  for (uint8_t i = 0; i < 5; ++i)
    if (chain[i] != Level::Ok) { v.first_broken = i; break; }
  return v;
}

void make_tag(const char *name, char tag[4]) {
  int n = 0;
  for (const char *p = name; *p && n < 3; ++p)
    if (isalnum(static_cast<unsigned char>(*p))) tag[n++] = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
  if (n == 0) tag[n++] = '?';
  tag[n] = '\0';
}

}  // namespace midi_core
