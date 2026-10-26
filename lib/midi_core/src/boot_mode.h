// Which way the board's one USB port runs at boot: as the MIDI host (normal),
// or left as the ESP32-S3's built-in USB-Serial-JTAG device so a computer can
// flash it without BOOT+RST ("Connect to computer" on the settings page).
//
// The request lives in NVS because moving the cable from the hub to a computer
// usually cuts power. It records the build that made it, so the first boot of
// a different build (an update just landed) returns to host mode by itself.
// Plain C++ so tests/ can run it on the Mac.
#pragma once

#include <stddef.h>

namespace midi_core {

enum class BootMode { Host, Computer };

struct BootDecision {
  BootMode mode;
  bool clear_request;  // forget the stored request (it is stale)
};

// requested_by: the build ID stored with the request, or nullptr/"" if none.
// this_build: the running build's ID.
BootDecision decide_boot_mode(const char *requested_by, const char *this_build);

}  // namespace midi_core
