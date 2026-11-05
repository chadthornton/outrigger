// What the FM-1 pages say, from what the Loader knows: the plan before a
// switch, a running job's steps, and the result or error after it
// (docs/ui-redesign.md, "Results and errors"). Plain C++ with no Arduino or
// FreeRTOS, so tests/test_loader_messages.cpp runs it on the Mac; loader_app.cpp
// gathers the inputs and calls it.
#pragma once

#include <stdint.h>

#include "host_ui.h"
#include "installer.h"
#include "softkey.h"
#include "switch_chain.h"

namespace loader_messages {

// Why the ROM-boot key didn't go out (the Host's checks before it).
enum class KeyStop : uint8_t {
  None,
  AlreadyInRom,  // a ROM device was already on the hub
  NoFm1,         // no FM-1 MIDI device on the hub
  NotReady,      // the FM-1 or the broker wasn't ready for the key
  Unsupported,   // an FM-1 the key hasn't been tried on (M-VAVE Open firmware)
  Answer,        // the identity handshake: see KeyResult
};

// "b50" from a backup id ("0123456789abcdef-b50"); "" for none.
void backup_name(const char *backup_id, char *out, size_t len);

// The plan page: what holding "switch to X" would do, from the preview.
struct PlanInput {
  const char *target;   // "Felucca 1.6.1"
  const char *running;  // what the FM-1 runs ("Skiff 1.4"), or "" when unknown
  const char *backup;   // the latest backup's name ("b50"), or ""
  bool has_unit;        // a backup of some FM-1 exists
  bool ready;           // the store and the loader file are fine
  bool writes_enabled;  // FM1_LOADER_WRITES
  bool unfinished;      // an install stopped part-way: only the retry runs
  fm1_loader::InstallPreview preview;
};
void describe_plan(const PlanInput &in, UiPlan &out);

// A running job's page.
struct JobInput {
  JobKind kind;
  fm1_loader::Step step;  // Idle: the hold just ran, the board restarts for the job
  const char *firmware;   // the switch's or the retry's target ("Felucca 1.6.1")
  const char *backup;     // the backup the switch checks against ("b50")
  uint32_t done, total;   // the step's sectors
  bool danger;
  Chain chain = Chain::First;     // a switch that found the FM-1 changed: its backup, then its install
  const char *stale_backup = "";  // the backup the FM-1 had changed since ("b50")
};
void describe_job(const JobInput &in, UiJob &out);

// How a job ended: the result page.
struct JobOutcome {
  JobKind kind;
  bool done;  // Step::Done
  fm1_loader::Failure failure;
  KeyStop key_stop;
  fm1_loader::KeyResult key;  // KeyStop::Answer
  bool danger;                // Installer::danger(): the app area may be incomplete
  bool wrote;                 // a write step was reached
  bool pending;               // the unit has an unfinished install recorded
  const char *firmware;       // the switch's or the retry's target ("Felucca 1.6.1"), or ""
  const char *backup;         // the latest backup's name after the job ("b51"), or ""
  const char *detail;         // for us: where it failed and what USB said, or ""
  uint32_t sectors;           // written, for a finished switch
  fm1_loader::DataAction data;
  Chain chain = Chain::First;     // where a switch was in its chain (switch_chain.h)
  const char *stale_backup = "";  // the backup the FM-1 had changed since
};
void describe_result(const JobOutcome &in, UiResult &out);

// The FM-1 page when the store or the loader file is bad.
void describe_not_ready(bool store_ok, bool blob_ok, bool blob_missing, const char *detail, UiResult &out);
// Which job a completed hold starts, or JobKind::None to refuse it (why says
// why). A hold runs only what its label named and the last preview showed.
struct HoldInput {
  UiAct act;
  int index;                // the row a switch's hold was for
  int selected;             // the row the Loader last previewed, -1 none
  bool selected_previewed;  // that row's package is the one previewed
  bool pending;             // resume_pending() now
  bool previewed_retry;     // the last preview found the install unfinished
};
JobKind job_for_hold(const HoldInput &in, const char **why);

// A hold that no longer matched what the Loader previewed: nothing ran.
void describe_refused(UiResult &out);
// The FM-1 page when an install stopped part-way (resume_pending()).
void describe_unfinished(const char *firmware, const char *backup, UiResult &out);

}  // namespace loader_messages
