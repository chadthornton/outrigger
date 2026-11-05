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
};

// Early in setup(): reads the record and settles what it says about this boot.
// An unconfirmed image also gets a timer that restarts the board
// kConfirmDeadlineMs after boot, whatever the loop is doing, so one that hangs
// goes back to the recovery app like one that crashes.
void update_boot();
// Every loop(). healthy: setup() reached Play or Computer mode (not safe mode).
// A new image confirms itself kConfirmAfterMs into a healthy boot.
void update_loop(uint32_t now_ms, bool healthy);
// Before a restart the user asked for (Connect to computer, Back to MIDI,
// Remote diagnostics): a healthy unconfirmed image confirms itself first, so
// the restart doesn't count as a failed boot of it.
void update_confirm_now(bool healthy);
// For GET /status, from any task: a copy taken under a lock.
UpdateStatus update_status();
// The recovery app's image SHA-256 (16 hex digits), or "none": hashed the
// first time it's asked for, not at every boot.
const char *update_recovery_sha();
// The recovery app's NVS log, newest last (empty if none): for GET /crash.
const char *update_recovery_log();
