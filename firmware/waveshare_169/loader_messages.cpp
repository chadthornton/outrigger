#include "apps.h"
#if APP_LOADER

#include "loader_messages.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

using namespace fm1_loader;

namespace loader_messages {

namespace {

__attribute__((format(printf, 3, 4))) void put(char *out, size_t len, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(out, len, fmt, ap);
  va_end(ap);
}
#define PUT(field, ...) put(field, sizeof field, __VA_ARGS__)

const char *or_(const char *s, const char *fallback) { return s && s[0] ? s : fallback; }

// "Skiff's songs": the firmware's name without its version.
void name_of(const char *label, char *out, size_t len) {
  snprintf(out, len, "%s", label);
  if (char *space = strrchr(out, ' ')) {
    if (space != out && (space[1] >= '0' && space[1] <= '9')) *space = 0;
  }
}

}  // namespace

void backup_name(const char *id, char *out, size_t len) {
  const char *dash = id ? strrchr(id, '-') : nullptr;
  snprintf(out, len, "%s", dash ? dash + 1 : "");
}

// ---------- The plan ----------
void describe_plan(const PlanInput &in, UiPlan &out) {
  const int8_t item = out.item;
  out = UiPlan{};
  out.item = item;
  PUT(out.title, "%s", in.target);
  PUT(out.hold, "switch to %s", in.target);
  int n = 0, warn = -1;  // warn: the first warning line
  auto line = [&](const char *text) {
    if (n < kPlanLines) snprintf(out.lines[n++], sizeof out.lines[0], "%s", text);
  };
  char buf[64], running[28];
  name_of(or_(in.running, ""), running, sizeof running);
  const InstallPreview &p = in.preview;

  if (in.unfinished) {
    warn = 0;
    line("An install is unfinished.");
    line("Retry it from the FM-1 page first.");
  } else if (!in.has_unit) {
    // The switch backs it up first; the plan proper comes from that backup, and
    // a plan that erases data stops there for the user's OK (its own result).
    line("Backs up the FM-1 first.");
    snprintf(buf, sizeof buf, "Then writes %s.", in.target);
    line(buf);
    line("MIDI stops until it's done.");
  } else if (!p.ok) {
    warn = 0;
    snprintf(buf, sizeof buf, "%s", p.problem);
    if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] = char(buf[0] - 'a' + 'A');
    line(buf);
    if (p.problem2 && p.problem2[0]) line(p.problem2);
  } else if (p.app_sectors == 0) {
    line("It's already running.");
    line("Nothing to write.");
  } else {
    // A UC25HQ80 unit's first switch: the data line, then the risk, in 6 lines.
    const bool risk = p.decision.chip_risk;
    if (!risk) {
      if (in.running[0]) snprintf(buf, sizeof buf, "Switches from %s.", in.running);
      else snprintf(buf, sizeof buf, "Switches from a firmware this board doesn't know.");
      line(buf);
      // One read must match the latest backup; if the FM-1 changed, the switch
      // backs up first (switch_chain.h). Only the ROM-boot read can tell, so the
      // plan doesn't say: the job page shows the backup when it runs.
      if (p.current.known() && family_info(p.current.family).saves_enabled) {
        snprintf(buf, sizeof buf, "Saves %s's songs.", running);
        line(buf);
      }
      snprintf(buf, sizeof buf, "Writes %s: %u sectors.", in.target, unsigned(p.app_sectors));
      line(buf);
    }
    char target[28];
    name_of(in.target, target, sizeof target);
    // A clean slate (an untested switch) clears the old firmware's data first.
    const bool clean = p.decision.clean_slate;
    switch (p.decision.action) {
      case DataAction::Restore:
        snprintf(buf, sizeof buf, clean ? "Clears old data, brings back %s's songs." : "Brings back %s's songs.",
                 target);
        line(buf);
        break;
      case DataAction::FreshStart:
        snprintf(buf, sizeof buf, clean ? "Clears old data: %s starts fresh." : "Starts %s fresh: no songs saved yet.",
                 target);
        line(buf);
        break;
      case DataAction::Keep:
        if (p.decision.stock_unmapped || p.decision.layout_warning) {
          warn = n;
          line(p.decision.stock_unmapped ? "Keeps old data: it may confuse stock."
                                         : "Keeps old data: it may not suit it.");
        } else {
          line("Keeps songs and settings.");
        }
        break;
    }
    char no_saves[48];
    snprintf(no_saves, sizeof no_saves, "%s may not save on this FM-1.", in.target);
    if (risk) {
      if (warn < 0) warn = n;
      line("This FM-1 has a newer flash chip (UC25HQ80).");
      line("Installs may fail or not save your songs.");
      if (p.decision.chip_no_saves) line(no_saves);
      if (p.decision.stock_resources_warning) line("Returning to stock may lose stock resources.");
      line("Untested by Outrigger: you're taking a risk.");
    } else {
      // Skiff 2.x writes stock's resources: the warning takes the last line (the
      // job screen says MIDI stops too).
      if (p.decision.stock_resources_warning && warn < 0) {
        warn = n;
        line("Returning to stock may lose stock resources.");
      } else {
        line("MIDI stops until it's done.");
      }
      if (p.decision.chip_no_saves) {
        if (warn < 0) warn = n;
        line(no_saves);
      }
    }
  }
  out.line_count = uint8_t(n);
  out.warn_from = uint8_t(warn < 0 ? n : warn);
  if (!in.writes_enabled) PUT(out.hold, "read-only build");
  out.can_act = in.ready && in.writes_enabled && !in.unfinished && (!in.has_unit || p.ok);
}

// ---------- A running job ----------
void describe_job(const JobInput &in, UiJob &out) {
  out = UiJob{};
  out.active = true;
  out.danger = in.danger;
  const char *fw = or_(in.firmware, "the firmware");
  const char *backup = or_(in.backup, "its backup");
  const uint8_t pct = in.total ? uint8_t(in.done * 100 / in.total) : 0;
  const Step s = in.step;
  const bool rom_steps = s == Step::Idle || s == Step::Confirm || s == Step::SendKey || s == Step::WaitRom ||
                         s == Step::Identify || s == Step::UploadLoader || s == Step::ReadIds;
  const bool writing = s == Step::WriteApp || s == Step::WriteData, restarting = s == Step::Reset || s == Step::WaitMidi;
  // What the FM-1 is doing before the job's real work: its live text.
  char rom_live[40];
  put(rom_live, sizeof rom_live, "%s",
      s == Step::Idle                                  ? "Restarting this board"
      : s == Step::Confirm || s == Step::SendKey       ? "Asking the FM-1"
      : s == Step::WaitRom                             ? "Waiting for ROM boot"
                                                       : "Starting the loader");

  // Each step: its text once done, now (live), and still to come.
  struct Row {
    char done[40], now[40], next[40];
  } rows[kJobSteps] = {};
  int count = 0, now = 0;
  auto row = [&](const char *done, const char *live, const char *next, bool is_now) {
    Row &r = rows[count];
    snprintf(r.done, sizeof r.done, "%s", done);
    snprintf(r.now, sizeof r.now, "%s", live);
    snprintf(r.next, sizeof r.next, "%s", next);
    if (is_now) now = count;
    ++count;
  };
  char a[40], b[40], c[40];
  const bool chained = in.chain != Chain::First;
  if (chained) {
    // A switch that found the FM-1 changed since its backup.
    PUT(out.title, "Switching to %s", fw);
    if (in.stale_backup && in.stale_backup[0]) put(a, sizeof a, "Changed since %s: nothing written", in.stale_backup);
    else put(a, sizeof a, "No backup yet: nothing written");
    row(a, a, a, false);
  }
  if (in.kind == JobKind::Backup && chained) {
    if (s == Step::Backup) put(b, sizeof b, "Backing up: %u%%", unsigned(pct));
    else put(b, sizeof b, "Backing up: %s", restarting ? "restarting the FM-1" : "starting");
    row("Backed up", b, "Back up", true);
    put(c, sizeof c, "Write %s", fw);
    row(c, c, c, false);
    row("FM-1 restarted", "Restarting the FM-1", "Restart the FM-1", false);
  } else if (in.kind == JobKind::Backup) {
    PUT(out.title, "Backing up");
    row("FM-1 in ROM boot", rom_live, "Put the FM-1 in ROM boot", rom_steps);
    put(b, sizeof b, "Reading twice: %u%%", unsigned(pct));
    row("Read twice and stored", b, "Read it twice and store it", s == Step::Backup);
    row("FM-1 restarted", "Restarting the FM-1", "Restart the FM-1", restarting);
    PUT(out.note, "Writes nothing to the FM-1.");
  } else if (in.kind == JobKind::Resume) {
    PUT(out.title, "Retrying %s", fw);
    row("FM-1 in ROM boot", rom_live, "Put the FM-1 in ROM boot", rom_steps);
    put(b, sizeof b, "Reading the FM-1: %u%%", unsigned(pct));
    row("Read the FM-1", b, "Read the FM-1", s == Step::ResumeRead || s == Step::Plan);
    put(c, sizeof c, "Writing %u of %u", unsigned(in.done), unsigned(in.total));
    row("Written and checked", c, "Finish writing", writing);
    row("FM-1 restarted", "Restarting the FM-1", "Restart the FM-1", restarting);
  } else {
    PUT(out.title, "Switching to %s", fw);
    if (chained) {
      put(b, sizeof b, "Backed up: %s", backup);
      row(b, b, b, false);
    } else {
      row("FM-1 in ROM boot", rom_live, "Put the FM-1 in ROM boot", rom_steps);
    }
    // The install's one read, compared with the backup it planned from. After
    // a chained backup the ROM-boot steps fold into this one.
    char done[40], next[40], live[40];
    put(done, sizeof done, "Checked: same as %s", backup);
    put(next, sizeof next, "Check it matches %s", backup);
    if (s == Step::Check) put(live, sizeof live, "Checking: %u%%", unsigned(pct));
    else put(live, sizeof live, "%s", rom_live);
    row(done, live, next, s == Step::Check || (chained && rom_steps));
    put(c, sizeof c, "Writing %u of %u", unsigned(in.done), unsigned(in.total));
    row("Written and checked", c, "Write", writing);
    row("FM-1 restarted", "Restarting the FM-1", "Restart the FM-1", restarting);
  }
  // A chained stage's first rows are done before it starts.
  if (chained && now == 0) now = 1;
  out.step_count = uint8_t(count);
  for (int i = 0; i < count; ++i) {
    out.steps[i].state = i < now ? UiStepState::Done : i == now ? UiStepState::Now : UiStepState::Next;
    snprintf(out.steps[i].text, sizeof out.steps[i].text, "%s",
             i < now ? rows[i].done : i == now ? rows[i].now : rows[i].next);
  }
  // The bar: the steps before this one, plus this one's share.
  out.percent = uint8_t((now * 100 + (in.total ? in.done * 100 / in.total : 0)) / count);
}

// ---------- Results and errors ----------
namespace {

const char *kNothing = "Nothing was written.";
const char *kChecked = "Installed and checked.";
const char *kDanger = "Don't unplug the FM-1 or switch it off.";

void safety(UiResult &out, UiSafety s, const char *text = nullptr) {
  out.safety = s;
  PUT(out.safety_text, "%s",
      text ? text : s == UiSafety::Danger ? kDanger : s == UiSafety::WrittenChecked ? kChecked : kNothing);
}

// The hold that runs the same job again.
void same_job(const JobOutcome &in, UiResult &out) {
  switch (in.kind) {
    case JobKind::Backup:
      out.fix = UiAct::Backup;
      PUT(out.fix_label, "back up now");
      break;
    case JobKind::Resume:
      out.fix = UiAct::Retry;
      PUT(out.fix_label, "retry %s", or_(in.firmware, "the install"));
      break;
    default:
      out.fix = UiAct::Switch;
      PUT(out.fix_label, "switch to %s", or_(in.firmware, "it"));
      break;
  }
}

void back_up_first(UiResult &out) {
  out.fix = UiAct::Backup;
  PUT(out.fix_label, "back up now");
}

}  // namespace

static void describe_stage(const JobOutcome &in, UiResult &out);

void describe_result(const JobOutcome &in, UiResult &out) {
  describe_stage(in, out);
  // The backup a switch needed didn't finish: it's the switch that didn't start.
  if (in.kind == JobKind::Backup && in.chain == Chain::Backup && !in.done) {
    char why[72];
    snprintf(why, sizeof why, "%s", out.why);
    if (why[0] >= 'A' && why[0] <= 'Z') why[0] = char(why[0] - 'A' + 'a');
    PUT(out.headline, "Switch didn't start");
    if (in.stale_backup && in.stale_backup[0])
      PUT(out.why, "It changed since %s, and the backup stopped: %s", in.stale_backup, why);
    else
      PUT(out.why, "No backup yet, and the backup stopped: %s", why);
    if (out.fix == UiAct::Backup) {
      out.fix = UiAct::Switch;
      PUT(out.fix_label, "switch to %s", or_(in.firmware, "it"));
    }
  }
}

static void describe_stage(const JobOutcome &in, UiResult &out) {
  const uint32_t seq = out.seq;
  out = UiResult{};
  out.seq = seq;
  PUT(out.detail, "%s", or_(in.detail, ""));
  const char *fw = or_(in.firmware, "the firmware");
  const bool backup = in.kind == JobKind::Backup, resume = in.kind == JobKind::Resume;

  if (in.done) {
    out.ok = true;
    if (backup) {
      PUT(out.headline, "Backed up");
      safety(out, UiSafety::NothingWritten);
      PUT(out.why, "Backup %s is stored on this board.", or_(in.backup, ""));
    } else if (!resume && in.sectors == 0) {
      PUT(out.headline, "Already running");
      safety(out, UiSafety::NothingWritten);
      PUT(out.why, "%s was already on the FM-1.", fw);
    } else {
      PUT(out.headline, resume ? "Install finished" : "Switched");
      safety(out, UiSafety::WrittenChecked);
      PUT(out.why, "%s is running.", fw);
      if (!resume) {
        switch (in.data) {
          case DataAction::Restore: PUT(out.todo, "Its songs are back."); break;
          case DataAction::FreshStart: PUT(out.todo, "It starts fresh: no songs yet."); break;
          case DataAction::Keep: PUT(out.todo, "Songs and settings kept."); break;
        }
        if (in.chain == Chain::Install)
          PUT(out.detail, "Backed up %s first; %u sectors written", or_(in.backup, ""), unsigned(in.sectors));
        else
          PUT(out.detail, "%u sectors written", unsigned(in.sectors));
      }
    }
    return;
  }

  // An install the FM-1 is still in ROM boot for: retry, never a power cycle.
  if (in.danger || (in.pending && in.failure == Failure::KeyNotSent && in.key_stop == KeyStop::AlreadyInRom)) {
    const bool routed = !in.danger;  // the key was refused because an earlier install is unfinished
    PUT(out.headline, routed ? "Install unfinished" : resume ? "Retry stopped" : "Switch stopped");
    safety(out, UiSafety::Danger);
    if (routed) PUT(out.why, "It's in ROM boot with %s part-written.", fw);
    else PUT(out.why, "It's in ROM boot with half of %s written.", fw);
    PUT(out.todo, "Hold to retry: it carries on where it stopped.");
    char backup_line[48];
    snprintf(backup_line, sizeof backup_line, "If the retry fails: Transporter + %s.", or_(in.backup, "the backup"));
    if (routed) PUT(out.detail, "%s", backup_line);
    else PUT(out.detail, "%s%s%s", or_(in.detail, ""), in.detail && in.detail[0] ? ". " : "", backup_line);
    out.fix = UiAct::Retry;
    PUT(out.fix_label, "retry %s", or_(in.firmware, "the install"));
    return;
  }

  // Nothing was written unless a write step was reached.
  safety(out, in.wrote ? UiSafety::WrittenChecked : UiSafety::NothingWritten);
  PUT(out.headline, "%s", backup   ? "Backup didn't finish"
                          : in.wrote ? (resume ? "Retry stopped" : "Switch stopped")
                                     : (resume ? "Retry didn't start" : "Switch didn't start"));
  same_job(in, out);
  switch (in.failure) {
    case Failure::KeyNotSent:
      switch (in.key_stop) {
        case KeyStop::AlreadyInRom:
          PUT(out.why, "The FM-1 was already in ROM boot.");
          PUT(out.todo, "Switch the FM-1 off and on, then try again.");
          break;
        case KeyStop::NoFm1:
          PUT(out.why, "No FM-1 on the hub.");
          PUT(out.todo, "Plug it into hub port 2.");
          out.fix = UiAct::None;
          break;
        case KeyStop::Unsupported:
          PUT(out.why, "The FM-1 runs M-VAVE Open firmware: no switching or backups yet.");
          PUT(out.todo, "Put back V15 on M-VAVE's FM1 Workbench, then try again.");
          out.fix = UiAct::None;
          break;
        case KeyStop::Answer:
          if (in.key == KeyResult::UpdateMode) {
            PUT(out.why, "The FM-1 is in the middle of an update.");
            PUT(out.todo, "Finish or cancel it on the FM-1, then try again.");
          } else if (in.key == KeyResult::NotFm1) {
            PUT(out.why, "The device on the hub didn't answer as an FM-1.");
            PUT(out.todo, "Check it's the FM-1, then try again.");
          } else {
            PUT(out.why, "The FM-1 didn't answer.");
            PUT(out.todo, "Go to its main screen, then try again.");
          }
          break;
        default:
          PUT(out.why, "The FM-1 wasn't ready for ROM boot.");
          PUT(out.todo, "Try again.");
          break;
      }
      break;
    case Failure::NoRomDevice:
      if (resume) {  // a retry sends no key: the FM-1 just wasn't there in ROM boot
        PUT(out.why, "The FM-1 wasn't in ROM boot.");
        PUT(out.todo, "Leave it plugged in and switched on, then try again.");
      } else {
        PUT(out.why, "The FM-1 took the key but didn't show up in ROM boot.");
        PUT(out.todo, "Switch it off and on, then try again.");
      }
      break;
    case Failure::BackupStale:
      // A first stale check chains to a backup (switch_chain.h); after that
      // backup it means the FM-1 changed again.
      if (in.chain == Chain::Install) {
        PUT(out.why, "The FM-1 changed again right after its backup.");
        PUT(out.todo, "Leave it untouched for a moment, then try again.");
      } else {
        PUT(out.why, "The FM-1 changed since %s.", or_(in.backup, "its backup"));
        PUT(out.todo, "Try again.");
      }
      break;
    case Failure::NoBackupForInstall:
      PUT(out.why, "There's no backup of this FM-1 yet.");
      PUT(out.todo, "Back it up first, then switch.");
      back_up_first(out);
      break;
    case Failure::BackupMismatch:
      PUT(out.why, "The two reads differed.");
      PUT(out.todo, "Try again. If it repeats, check the cable and hub.");
      break;
    case Failure::LoaderTooSlow:
      PUT(out.why, "The FM-1 was too slow to read.");
      PUT(out.todo, "Try again.");
      break;
    case Failure::MidiNotBack:
      if (backup) {
        PUT(out.headline, "Backed up, no MIDI");
        PUT(out.why, "The backup is stored, but the FM-1 didn't come back as MIDI.");
      } else {
        PUT(out.headline, "Installed, no MIDI");
        safety(out, UiSafety::WrittenChecked);
        PUT(out.why, "Installed, but the FM-1 didn't come back as MIDI.");
      }
      PUT(out.todo, "Switch it off and on.");
      out.fix = UiAct::None;
      break;
    case Failure::NeedsFreshStartOk:
    case Failure::NeedsLayoutChangeOk:
      if (in.chain == Chain::Install) {  // the backup in the middle changed what the switch would do
        if (in.stale_backup && in.stale_backup[0]) PUT(out.why, "The FM-1 changed, and its new backup changes the plan.");
        else PUT(out.why, "It's backed up now, and the switch needs your OK first.");
        PUT(out.todo, "Open %s again to see it, then hold.", fw);
        out.fix = UiAct::None;
      } else {
        PUT(out.why, "%s.", failure_text(in.failure));
        PUT(out.todo, "Open it again to see the plan, then hold.");
      }
      break;
    case Failure::Blocked:
      PUT(out.why, "Not yet supported: Outrigger can't switch to this firmware yet.");
      PUT(out.todo, "Pick another firmware.");
      out.fix = UiAct::None;
      break;
    case Failure::V16Head:
    case Failure::UnknownHead:
      PUT(out.why, "%s", head_problem(in.failure == Failure::V16Head ? Head::V16 : Head::Other));
      PUT(out.todo, "%s", kHeadProblem2);
      out.fix = UiAct::None;
      break;
    case Failure::NeedsChipRiskOk:
      // The warning is on the plan: the hold that agrees to it starts the switch.
      if (in.chain == Chain::Install) PUT(out.why, "It's backed up now, and the switch needs your OK first.");
      else PUT(out.why, "This FM-1 has a newer flash chip (UC25HQ80).");
      PUT(out.todo, "Open %s again to read the warning, then hold.", fw);
      out.fix = UiAct::None;
      break;
    case Failure::WritesLocked:
      PUT(out.why, "This build can't write to the FM-1.");
      PUT(out.todo, "Install a build with writes on.");
      out.fix = UiAct::None;
      break;
    default: {
      char why[72];
      snprintf(why, sizeof why, "%s", failure_text(in.failure));
      if (why[0] >= 'a' && why[0] <= 'z') why[0] = char(why[0] - 'a' + 'A');
      PUT(out.why, "%s.", why);
      PUT(out.todo, "Try again.");
      break;
    }
  }
}

void describe_not_ready(bool store_ok, bool blob_ok, bool blob_missing, const char *detail, UiResult &out) {
  out = UiResult{};
  PUT(out.headline, "Can't switch yet");
  safety(out, UiSafety::NothingWritten, "The FM-1 is untouched.");
  if (!store_ok) PUT(out.why, "This board's firmware store can't be read.");
  else if (!blob_ok) PUT(out.why, blob_missing ? "The loader file is missing." : "The loader file is damaged.");
  PUT(out.todo, "It needs a fix from a computer. MIDI still works.");
  PUT(out.detail, "%s", or_(detail, ""));
}

JobKind job_for_hold(const HoldInput &in, const char **why) {
  *why = "";
  switch (in.act) {
    case UiAct::Retry:
      if (in.previewed_retry && in.pending) return JobKind::Resume;
      *why = "retry, but no install is unfinished";
      return JobKind::None;
    case UiAct::Backup:
      if (!in.pending && !in.previewed_retry) return JobKind::Backup;
      *why = "backup, but an install is unfinished";
      return JobKind::None;
    case UiAct::Switch:
      if (in.pending || in.previewed_retry) *why = "switch, but an install is unfinished";
      else if (in.selected < 0 || in.index != in.selected || !in.selected_previewed) *why = "switch, but the selection changed";
      else return JobKind::Install;
      return JobKind::None;
    default:
      *why = "a hold with nothing to run";
      return JobKind::None;
  }
}

void describe_refused(UiResult &out) {
  const uint32_t seq = out.seq;
  out = UiResult{};
  out.seq = seq;
  PUT(out.headline, "Nothing started");
  safety(out, UiSafety::NothingWritten);
  PUT(out.why, "The page changed before the hold finished.");
  PUT(out.todo, "Check it, then hold again.");
}

void describe_unfinished(const char *firmware, const char *backup, UiResult &out) {
  out = UiResult{};
  PUT(out.headline, "Install unfinished");
  safety(out, UiSafety::Danger);
  PUT(out.why, "It's in ROM boot with %s part-written.", or_(firmware, "a firmware"));
  PUT(out.todo, "Hold to retry: it carries on where it stopped.");
  PUT(out.detail, "If the retry fails: Transporter + %s.", or_(backup, "the backup"));
  out.fix = UiAct::Retry;
  PUT(out.fix_label, "retry %s", or_(firmware, "the install"));
}

}  // namespace loader_messages

#endif  // APP_LOADER
