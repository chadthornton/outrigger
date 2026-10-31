// Host-side tests for firmware/waveshare_169/loader_messages: what the FM-1
// pages say for each way a job ends (docs/ui-redesign.md's mapping table),
// the plan before a switch and a job's steps. Run with `make test`.
#include <stdio.h>
#include <string.h>

#include "loader_messages.h"

using namespace fm1_loader;
using namespace loader_messages;

static int failures = 0;

#define EXPECT(cond, what)                                   \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s (line %d)\n", what, __LINE__);         \
      ++failures;                                            \
    }                                                        \
  } while (0)

static bool has(const char *s, const char *part) { return strstr(s, part) != nullptr; }

static JobOutcome switch_failed(Failure f) {
  JobOutcome o{};
  o.kind = JobKind::Install;
  o.failure = f;
  o.firmware = "Felucca 1.6.1";
  o.backup = "b50";
  o.detail = "check read at 0x5A000";
  return o;
}

static UiResult result(const JobOutcome &o) {
  UiResult r{};
  r.seq = 7;
  describe_result(o, r);
  return r;
}

// The safety card follows what was written, not !danger() alone.
static void test_danger_retries() {
  for (Failure f : {Failure::DeviceLost, Failure::WriteVerifyFailed}) {
    JobOutcome o = switch_failed(f);
    o.danger = o.wrote = true;
    o.pending = true;
    UiResult r = result(o);
    EXPECT(r.seq == 7, "keeps the result's number");
    EXPECT(r.safety == UiSafety::Danger && has(r.safety_text, "Don't unplug"), "danger card");
    EXPECT(strcmp(r.headline, "Switch stopped") == 0, "headline");
    EXPECT(has(r.why, "half of Felucca 1.6.1 written"), "why names the firmware");
    EXPECT(has(r.todo, "Hold to retry"), "to do: the retry");
    EXPECT(has(r.detail, "check read at 0x5A000") && has(r.detail, "Transporter + b50"), "detail adds the last resort");
    EXPECT(r.fix == UiAct::Retry && strcmp(r.fix_label, "retry Felucca 1.6.1") == 0, "the hold retries it");
    EXPECT(!r.ok, "not ok");
  }
}

static void test_key_not_sent() {
  JobOutcome o = switch_failed(Failure::KeyNotSent);
  o.key_stop = KeyStop::AlreadyInRom;
  o.detail = "FM-1 already in ROM boot: power-cycle it";
  UiResult r = result(o);
  EXPECT(r.safety == UiSafety::NothingWritten && strcmp(r.safety_text, "Nothing was written.") == 0, "nothing written");
  EXPECT(strcmp(r.headline, "Switch didn't start") == 0, "didn't start");
  EXPECT(has(r.why, "already in ROM boot"), "why");
  EXPECT(has(r.todo, "off and on"), "power-cycle it");
  EXPECT(r.fix == UiAct::Switch && strcmp(r.fix_label, "switch to Felucca 1.6.1") == 0, "the hold switches again");

  // The same, with an install recorded as pending: never advise a power cycle.
  o.pending = true;
  r = result(o);
  EXPECT(r.safety == UiSafety::Danger, "pending: danger");
  EXPECT(strcmp(r.headline, "Install unfinished") == 0, "routes to Install unfinished");
  EXPECT(!has(r.todo, "off and on") && !has(r.why, "off and on") && !has(r.detail, "power-cycle"),
         "no power cycle anywhere on the page");
  EXPECT(r.fix == UiAct::Retry && strcmp(r.fix_label, "retry Felucca 1.6.1") == 0, "the hold retries");

  o = switch_failed(Failure::KeyNotSent);
  o.key_stop = KeyStop::NoFm1;
  r = result(o);
  EXPECT(strcmp(r.why, "No FM-1 on the hub.") == 0 && has(r.todo, "hub port 2"), "no FM-1");
  EXPECT(r.fix == UiAct::None, "no FM-1: OK, nothing to run");

  o.key_stop = KeyStop::Answer;
  o.key = KeyResult::UpdateMode;
  EXPECT(has(result(o).why, "update"), "an FM-1 mid-update says so");
  o.key = KeyResult::NoAnswer;
  EXPECT(has(result(o).why, "didn't answer"), "no answer");
  o.key_stop = KeyStop::NotReady;
  EXPECT(result(o).fix == UiAct::Switch && has(result(o).todo, "Try again"), "not ready: try again");
}

static void test_nothing_written_rows() {
  UiResult r = result(switch_failed(Failure::NoRomDevice));
  EXPECT(r.safety == UiSafety::NothingWritten && has(r.why, "didn't show up in ROM boot"), "no ROM device");
  EXPECT(has(r.todo, "off and on") && r.fix == UiAct::Switch, "no ROM device: power-cycle, switch again");

  // A first stale check chains to a backup and never ends a switch; one right
  // after that backup does.
  JobOutcome again = switch_failed(Failure::BackupStale);
  again.chain = Chain::Install;
  r = result(again);
  EXPECT(strcmp(r.headline, "Switch didn't start") == 0 && r.safety == UiSafety::NothingWritten, "changed again");
  EXPECT(has(r.why, "changed again right after its backup") && has(r.todo, "untouched"), "changed again: why, to do");
  EXPECT(r.fix == UiAct::Switch && strcmp(r.fix_label, "switch to Felucca 1.6.1") == 0, "changed again: switch again");

  JobOutcome b{};
  b.kind = JobKind::Backup;
  b.failure = Failure::BackupMismatch;
  r = result(b);
  EXPECT(strcmp(r.headline, "Backup didn't finish") == 0 && has(r.why, "two reads differed"), "mismatch");
  EXPECT(has(r.todo, "cable and hub") && r.fix == UiAct::Backup, "mismatch: the same job");

  r = result(switch_failed(Failure::LoaderTooSlow));
  EXPECT(has(r.why, "too slow") && r.fix == UiAct::Switch, "too slow: the same job");

  JobOutcome retry = switch_failed(Failure::NoRomDevice);
  retry.kind = JobKind::Resume;
  r = result(retry);
  EXPECT(!has(r.why, "key") && !has(r.todo, "off and on") && r.fix == UiAct::Retry, "a retry sends no key");

  r = result(switch_failed(Failure::IdentityMismatch));
  EXPECT(has(r.why, "Chip key") && strcmp(r.todo, "Try again.") == 0, "anything else: failure_text, try again");
  EXPECT(r.safety == UiSafety::NothingWritten && r.fix == UiAct::Switch, "anything else: the same job");
}

// The backup a switch needed didn't finish: the switch didn't start.
static void test_chained_backup_failed() {
  JobOutcome o{};
  o.kind = JobKind::Backup;
  o.chain = Chain::Backup;
  o.failure = Failure::BackupMismatch;
  o.firmware = "Felucca 1.6.1";
  o.stale_backup = "b50";
  UiResult r = result(o);
  EXPECT(strcmp(r.headline, "Switch didn't start") == 0 && r.safety == UiSafety::NothingWritten, "headline");
  EXPECT(has(r.why, "changed since b50") && has(r.why, "two reads differed"), "why: changed, then the backup's reason");
  EXPECT(r.fix == UiAct::Switch && strcmp(r.fix_label, "switch to Felucca 1.6.1") == 0, "the hold switches again");
  EXPECT(!has(r.headline, "unfinished"), "never Install unfinished");

  // The fresh backup changed the plan (a fresh start the user didn't agree to).
  JobOutcome p{};
  p.kind = JobKind::Install;
  p.chain = Chain::Install;
  p.failure = Failure::NeedsFreshStartOk;
  p.firmware = "Felucca 1.6.1";
  r = result(p);
  EXPECT(r.safety == UiSafety::NothingWritten && has(r.why, "changes the plan"), "plan changed");
  EXPECT(has(r.todo, "Open Felucca 1.6.1") && r.fix == UiAct::None, "plan changed: look first, no hold");

  // A switch that backed up first says so.
  JobOutcome ok{};
  ok.kind = JobKind::Install;
  ok.chain = Chain::Install;
  ok.done = true;
  ok.firmware = "Felucca 1.6.1";
  ok.backup = "b51";
  ok.sectors = 149;
  r = result(ok);
  EXPECT(r.ok && has(r.detail, "Backed up b51 first") && has(r.detail, "149 sectors"), "switched after a backup");
}

static void test_midi_not_back() {
  JobOutcome o = switch_failed(Failure::MidiNotBack);
  o.wrote = true;
  UiResult r = result(o);
  EXPECT(r.safety == UiSafety::WrittenChecked && strcmp(r.safety_text, "Installed and checked.") == 0, "written");
  EXPECT(has(r.why, "didn't come back as MIDI") && has(r.todo, "off and on"), "why and to do");
  EXPECT(r.fix == UiAct::None, "OK");

  JobOutcome b{};
  b.kind = JobKind::Backup;
  b.failure = Failure::MidiNotBack;
  r = result(b);
  EXPECT(r.safety == UiSafety::NothingWritten && has(r.why, "backup is stored"), "a backup writes nothing");
}

// A failure after a write step but without danger: written, not "nothing".
static void test_written_without_danger() {
  JobOutcome o = switch_failed(Failure::StorageFailed);
  o.wrote = true;
  UiResult r = result(o);
  EXPECT(r.safety == UiSafety::WrittenChecked, "wrote: not 'nothing written'");
  EXPECT(strcmp(r.headline, "Switch stopped") == 0, "stopped, not 'didn't start'");
}

static void test_retry_and_success() {
  JobOutcome o = switch_failed(Failure::DeviceLost);
  o.kind = JobKind::Resume;
  o.danger = o.wrote = true;
  UiResult r = result(o);
  EXPECT(strcmp(r.headline, "Retry stopped") == 0 && r.fix == UiAct::Retry, "a retry that stops");

  JobOutcome ok{};
  ok.kind = JobKind::Install;
  ok.done = true;
  ok.firmware = "Felucca 1.6.1";
  ok.sectors = 149;
  ok.data = DataAction::Restore;
  r = result(ok);
  EXPECT(r.ok && strcmp(r.headline, "Switched") == 0, "switched");
  EXPECT(r.safety == UiSafety::WrittenChecked && has(r.why, "Felucca 1.6.1 is running"), "running");
  EXPECT(has(r.todo, "songs are back") && has(r.detail, "149 sectors"), "songs and sectors");
  EXPECT(r.fix == UiAct::None, "OK");
  ok.sectors = 0;
  r = result(ok);
  EXPECT(r.safety == UiSafety::NothingWritten && strcmp(r.headline, "Already running") == 0, "nothing to write");

  JobOutcome b{};
  b.kind = JobKind::Backup;
  b.done = true;
  b.backup = "b51";
  r = result(b);
  EXPECT(r.ok && r.safety == UiSafety::NothingWritten && has(r.why, "b51"), "backed up");
}

static void test_not_ready_and_unfinished() {
  UiResult r{};
  describe_not_ready(false, false, true, "Store unreadable: make flash-store", r);
  EXPECT(has(r.why, "store can't be read") && has(r.todo, "MIDI still works"), "store");
  EXPECT(r.fix == UiAct::None && r.safety == UiSafety::NothingWritten, "OK only");
  describe_not_ready(true, false, true, "", r);
  EXPECT(has(r.why, "loader file is missing"), "blob missing");
  describe_not_ready(true, false, false, "", r);
  EXPECT(has(r.why, "damaged"), "blob damaged");

  describe_refused(r);
  EXPECT(r.safety == UiSafety::NothingWritten && r.fix == UiAct::None && has(r.todo, "hold again"), "refused");

  describe_unfinished("Felucca 1.6.1", "b50", r);
  EXPECT(strcmp(r.headline, "Install unfinished") == 0 && r.safety == UiSafety::Danger, "unfinished");
  EXPECT(r.fix == UiAct::Retry && strcmp(r.fix_label, "retry Felucca 1.6.1") == 0, "the hold names the retry");
  EXPECT(has(r.detail, "Transporter + b50"), "last resort");
}

static PlanInput plan_input() {
  PlanInput in{};
  in.target = "Felucca 1.6.1";
  in.running = "Skiff 1.4";
  in.backup = "b50";
  in.has_unit = in.ready = in.writes_enabled = true;
  in.preview.ok = true;
  in.preview.current.identity = "FM-1_714";
  in.preview.current.family = Family::Felucca;
  in.preview.app_sectors = 143;
  in.preview.decision.action = DataAction::Restore;
  return in;
}

static bool plan_has(const UiPlan &p, const char *part) {
  for (int i = 0; i < p.line_count; ++i)
    if (has(p.lines[i], part)) return true;
  return false;
}

static void test_plan() {
  UiPlan p{};
  p.item = 2;
  describe_plan(plan_input(), p);
  EXPECT(p.item == 2, "keeps its row");
  EXPECT(strcmp(p.title, "Felucca 1.6.1") == 0 && strcmp(p.hold, "switch to Felucca 1.6.1") == 0, "title and hold");
  EXPECT(strcmp(p.lines[0], "Switches from Skiff 1.4.") == 0, "from");
  EXPECT(plan_has(p, "Backs up only if the FM-1 changed."), "backs up only when needed");
  EXPECT(plan_has(p, "Saves Skiff's songs."), "saves the running firmware's songs");
  EXPECT(plan_has(p, "Writes Felucca 1.6.1: 143 sectors."), "writes");
  EXPECT(plan_has(p, "Brings back Felucca's songs."), "restores");
  EXPECT(plan_has(p, "MIDI stops"), "MIDI stops");
  EXPECT(p.can_act && p.warn_from == p.line_count, "can act, no warnings");

  PlanInput in = plan_input();
  in.preview.decision.action = DataAction::FreshStart;
  describe_plan(in, p);
  EXPECT(plan_has(p, "Starts Felucca fresh: no songs saved yet."), "fresh start");

  in = plan_input();
  in.preview.decision.action = DataAction::Keep;
  in.preview.decision.stock_unmapped = true;
  describe_plan(in, p);
  EXPECT(p.warn_from < p.line_count && has(p.lines[p.warn_from], "may confuse stock"), "stock warning kept");

  in = plan_input();
  in.preview.app_sectors = 0;
  describe_plan(in, p);
  EXPECT(plan_has(p, "already running"), "already installed");

  in = plan_input();
  in.preview.ok = false;
  in.preview.problem = "package missing or damaged in the library";
  describe_plan(in, p);
  EXPECT(!p.can_act && p.warn_from == 0 && has(p.lines[0], "Package missing"), "a preview problem disables the hold");

  in = plan_input();
  in.ready = false;
  describe_plan(in, p);
  EXPECT(!p.can_act, "not ready: no hold");
  in = plan_input();
  in.writes_enabled = false;
  describe_plan(in, p);
  EXPECT(!p.can_act && strcmp(p.hold, "read-only build") == 0, "read-only build");
  in = plan_input();
  in.unfinished = true;
  describe_plan(in, p);
  EXPECT(!p.can_act && has(p.lines[0], "unfinished"), "unfinished: only the retry runs");
  in = plan_input();
  in.has_unit = false;
  describe_plan(in, p);
  EXPECT(p.can_act && plan_has(p, "backs it up first"), "no backup yet: the switch backs up first");
  in.ready = false;
  describe_plan(in, p);
  EXPECT(!p.can_act, "no backup and not ready: no hold");
}

static void test_job_steps() {
  UiJob j{};
  describe_job({JobKind::Install, Step::Idle, "Felucca 1.6.1", "b50", 0, 0, false}, j);
  EXPECT(j.active && strcmp(j.title, "Switching to Felucca 1.6.1") == 0, "title");
  EXPECT(j.step_count == 4 && j.steps[0].state == UiStepState::Now, "starts on the first step");
  EXPECT(has(j.steps[0].text, "Restarting this board"), "the restart before the job");

  describe_job({JobKind::Install, Step::Check, "Felucca 1.6.1", "b50", 128, 256, false}, j);
  EXPECT(j.steps[0].state == UiStepState::Done && strcmp(j.steps[0].text, "FM-1 in ROM boot") == 0, "rom done");
  EXPECT(j.steps[1].state == UiStepState::Now && strcmp(j.steps[1].text, "Checking: 50%") == 0, "checking");
  EXPECT(j.steps[2].state == UiStepState::Next && j.steps[3].state == UiStepState::Next, "to come");
  EXPECT(!j.danger, "no danger before the erase");

  describe_job({JobKind::Install, Step::WriteApp, "Felucca 1.6.1", "b50", 88, 149, true}, j);
  EXPECT(strcmp(j.steps[1].text, "Checked: same as b50") == 0, "checked");
  EXPECT(strcmp(j.steps[2].text, "Writing 88 of 149") == 0 && j.danger, "writing, danger");
  EXPECT(j.percent > 50 && j.percent < 75, "the bar counts the steps before");

  describe_job({JobKind::Backup, Step::Backup, "", "", 10, 20, false}, j);
  EXPECT(strcmp(j.title, "Backing up") == 0 && j.step_count == 3, "backup");
  EXPECT(strcmp(j.steps[1].text, "Reading twice: 50%") == 0, "reading");
  EXPECT(has(j.note, "Writes nothing"), "a backup writes nothing");

  // A switch that found the FM-1 changed: the backup it runs, then the install.
  JobInput chained = {JobKind::Backup, Step::Backup, "Felucca 1.6.1", "", 42, 100, false};
  chained.chain = Chain::Backup;
  chained.stale_backup = "b50";
  describe_job(chained, j);
  EXPECT(strcmp(j.title, "Switching to Felucca 1.6.1") == 0, "the backup is the switch's");
  EXPECT(j.steps[0].state == UiStepState::Done && strcmp(j.steps[0].text, "Changed since b50: nothing written") == 0,
         "says why it backs up");
  EXPECT(j.steps[1].state == UiStepState::Now && has(j.steps[1].text, "Backing up: 42%"), "backing up");
  EXPECT(j.steps[2].state == UiStepState::Next && has(j.steps[2].text, "Felucca"), "the write to come");
  chained = {JobKind::Install, Step::WriteApp, "Felucca 1.6.1", "b51", 88, 149, true};
  chained.chain = Chain::Install;
  chained.stale_backup = "b50";
  describe_job(chained, j);
  EXPECT(j.step_count == 5 && strcmp(j.steps[1].text, "Backed up: b51") == 0, "the backup done");
  EXPECT(strcmp(j.steps[2].text, "Checked: same as b51") == 0 && strcmp(j.steps[3].text, "Writing 88 of 149") == 0,
         "then checked and writing");

  describe_job({JobKind::Resume, Step::WaitMidi, "Felucca 1.6.1", "", 0, 0, false}, j);
  EXPECT(has(j.title, "Retrying") && j.steps[3].state == UiStepState::Now, "a retry restarting the FM-1");
  EXPECT(j.steps[2].state == UiStepState::Done, "written before the restart");
}

// A hold runs only what its label named and the last preview showed.
static void test_hold_gate() {
  const char *why;
  const HoldInput ok_switch = {UiAct::Switch, 2, 2, true, false, false};
  EXPECT(job_for_hold(ok_switch, &why) == JobKind::Install && !why[0], "a switch that matches its preview");
  HoldInput h = ok_switch;
  h.index = 1;
  EXPECT(job_for_hold(h, &why) == JobKind::None && has(why, "selection"), "a switch for another row");
  h = ok_switch;
  h.selected_previewed = false;
  EXPECT(job_for_hold(h, &why) == JobKind::None, "a switch whose package isn't the previewed one");
  h = ok_switch;
  h.selected = -1;
  h.index = -1;
  EXPECT(job_for_hold(h, &why) == JobKind::None, "a switch with nothing selected");
  h = ok_switch;
  h.pending = true;
  EXPECT(job_for_hold(h, &why) == JobKind::None && has(why, "unfinished"), "no switch over an unfinished install");
  h = ok_switch;
  h.previewed_retry = true;
  EXPECT(job_for_hold(h, &why) == JobKind::None, "nor while the preview showed one");

  EXPECT(job_for_hold({UiAct::Backup, -1, -1, false, false, false}, &why) == JobKind::Backup, "a backup");
  EXPECT(job_for_hold({UiAct::Backup, -1, -1, false, true, false}, &why) == JobKind::None, "no backup: unfinished");
  EXPECT(job_for_hold({UiAct::Backup, -1, -1, false, false, true}, &why) == JobKind::None, "no backup: previewed retry");

  EXPECT(job_for_hold({UiAct::Retry, -1, -1, false, true, true}, &why) == JobKind::Resume, "a retry");
  EXPECT(job_for_hold({UiAct::Retry, -1, -1, false, false, true}, &why) == JobKind::None,
         "no retry once the FM-1 is back (pending gone)");
  EXPECT(job_for_hold({UiAct::Retry, -1, -1, false, true, false}, &why) == JobKind::None,
         "no retry the preview didn't show");
  EXPECT(job_for_hold({UiAct::None, -1, -1, false, false, false}, &why) == JobKind::None, "nothing to run");
}

static void test_backup_name() {
  char b[12];
  backup_name("69bb070aaa87bd10-b50", b, sizeof b);
  EXPECT(strcmp(b, "b50") == 0, "b50");
  backup_name("", b, sizeof b);
  EXPECT(strcmp(b, "") == 0, "none");
}

int main() {
  test_danger_retries();
  test_key_not_sent();
  test_nothing_written_rows();
  test_chained_backup_failed();
  test_midi_not_back();
  test_written_without_danger();
  test_retry_and_success();
  test_not_ready_and_unfinished();
  test_plan();
  test_job_steps();
  test_hold_gate();
  test_backup_name();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all loader message tests passed\n");
  return 0;
}
