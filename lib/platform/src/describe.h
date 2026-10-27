// Turns a USB configuration descriptor into the broker's DeviceDesc. Plain C++
// so tests/ can run it on the Mac.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "broker.h"

namespace platform {

// One entry per interface number, in descriptor order. Alternate settings are
// merged: class triple from the first one seen, endpoints from the largest
// (the one an app would start). Interfaces past kMaxInterfaces are left out.
// Returns false, with out holding what was read so far, when a descriptor's
// length runs past the end.
bool describe_device(uint8_t address, uint16_t vid, uint16_t pid, bool is_hub, const char tag[4],
                     const uint8_t *config, size_t length, DeviceDesc &out);

}  // namespace platform
