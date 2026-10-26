// Health of the signal chain the screen draws: Power -> Hub -> Controllers ->
// Router -> FM-1. The firmware fills a HostSnapshot from what the USB host
// knows; evaluate() decides each node's level and the one problem worth
// explaining. Plain C++ so tests/ can run it on the Mac.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "midi_core.h"

namespace midi_core {

enum class Level : uint8_t { Off, Ok, Warn, Bad };  // grey, green, orange, red

constexpr size_t kMaxDevices = 6;
constexpr size_t kNameLen = 22;
constexpr size_t kProblemLen = 34;

struct DeviceView {
  uint8_t address;
  char name[kNameLen];
  uint8_t port_id;  // EspUsbHost portId: 0x12 = hub 1, port 2
  Role role;
  bool is_midi;     // has a MIDI interface (claimed or not)
  bool midi_open;   // its MIDI interface got USB channels and works
};

struct HostSnapshot {
  uint32_t uptime_ms;
  bool hub_present;
  uint8_t hub_ports;
  char port_state[4];        // per hub port: '.' off, 'p' empty, 'C' connected, 'E' enabled
  uint32_t port_stuck_ms[4]; // how long a port has been 'C' without becoming 'E'
  uint8_t channels_used;     // USB host channels in use, out of 8
  DeviceView devices[kMaxDevices];
  uint8_t device_count;      // hubs excluded
};

struct Problem {
  char line1[kProblemLen];
  char line2[kProblemLen];
};

struct ChainView {
  Level power, hub, controllers, router, target;
  uint8_t first_broken;  // index of the first node not Ok (0-4), 5 when all are Ok
  // Ports stuck since boot, e.g. a hub's own inner hub chip that never starts.
  // Shown quietly in the hub row instead of as a problem.
  bool port_off_since_boot[4];
  bool has_problem;
  Problem problem;
};

ChainView evaluate(const HostSnapshot &snapshot);

// Short label for a device: the first three letters or digits of its name,
// upper-cased ("Launchkey Mini" -> "LAU"). "?" when the name is empty.
void make_tag(const char *name, char tag[4]);

}  // namespace midi_core
