#include "boot_mode.h"

#include <string.h>

namespace midi_core {

BootDecision decide_boot_mode(const char *requested_by, const char *this_build) {
  if (!requested_by || !requested_by[0]) return {BootMode::Host, false};
  if (this_build && this_build[0] && strcmp(requested_by, this_build) == 0) return {BootMode::Computer, false};
  return {BootMode::Host, true};
}

}  // namespace midi_core
