// Outrigger's side of its own updates (lib/ota, docs/ota-updates.md): at boot,
// what the update record says about this image; in the loop, the confirm that
// keeps a newly copied image, or the restart that hands it back to the
// recovery app. Every app set has it, so any build an update brings can keep
// itself, and none is kept that never reached its first healthy loop.
#pragma once

#include <stdint.h>

struct UpdateStatus {
  const char *state = "idle";   // ota::state_name
  const char *result = "none";  // ota::result_name of the last update
  uint8_t attempts = 0;         // boots the last update took
  char label[16] = {};          // its commit
  bool pending_verify = false;  // this image hasn't confirmed itself yet
  char recovery[17] = "none";   // the recovery app's image SHA-256, 16 hex digits
};

// Early in setup(): reads the record and settles what it says about this boot.
void update_boot();
// Every loop(). healthy: setup() reached Play or Computer mode (not safe mode).
// A new image confirms itself kConfirmAfterMs into a healthy boot; one that
// can't by kConfirmDeadlineMs restarts, and the bootloader goes back to the
// recovery app.
void update_loop(uint32_t now_ms, bool healthy);
// For GET /status. Set by update_boot and update_loop on the loop task.
const UpdateStatus &update_status();
// The recovery app's NVS log, newest last (empty if none): for GET /crash.
const char *update_recovery_log();
