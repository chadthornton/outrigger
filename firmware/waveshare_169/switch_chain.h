// A switch backs up only when the FM-1 changed (docs/ui-redesign.md, "What
// the hold runs"). Its install checks the FM-1 against the latest backup
// before any erase; if it changed (or there's no backup yet), the job restarts
// the board into a backup, then into the install again against that backup.
// Each stage is its own boot (loader_app.cpp's JobBoot). This decides what
// follows a stage; header-only and plain C++, so the fm1_loader tests drive
// the real chain on the mock FM-1.
#pragma once

#include <stdint.h>

#include "installer.h"

namespace loader_messages {

// What a job boot runs (loader_app.cpp's JobBoot).
enum class JobKind : uint8_t { None, Backup, Install, Resume };

// Where a switch is in its chain: its first try, the backup it needed, the
// install after that backup.
enum class Chain : uint8_t { First, Backup, Install };

struct NextJob {
  bool restart;  // false: the chain ends here, with this stage's result
  JobKind kind;
  Chain chain;
};

// What follows a stage that ended (done, or failed with `failure`).
// Only a first try that stopped before any erase backs up, so a chain has at
// most three stages and never loops.
inline NextJob next_after(JobKind kind, Chain chain, bool done, fm1_loader::Failure failure, bool danger) {
  using fm1_loader::Failure;
  const NextJob end = {false, JobKind::None, chain};
  if (kind == JobKind::Install && chain == Chain::First && !done && !danger &&
      (failure == Failure::BackupStale || failure == Failure::NoBackupForInstall))
    return {true, JobKind::Backup, Chain::Backup};
  if (kind == JobKind::Backup && chain == Chain::Backup && done) return {true, JobKind::Install, Chain::Install};
  return end;
}

}  // namespace loader_messages
