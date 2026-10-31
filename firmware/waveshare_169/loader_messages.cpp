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
    warn = 0;
    line("No backup of this FM-1 yet.");
    line("Back it up first, from Backups.");
  } else if (!p.ok) {
    warn = 0;
    snprintf(buf, sizeof buf, "%s", p.problem);
    if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] = char(buf[0] - 'a' + 'A');
    line(buf);
  } else if (p.app_sectors == 0) {
    line("It's already running.");
    line("Nothing to write.");
  } else {
    if (in.running[0]) snprintf(buf, sizeof buf, "Switches from %s.", in.running);
    else snprintf(buf, sizeof buf, "Switches from a firmware this board doesn't know.");
    line(buf);
    // Today's install: one read must match the latest backup, or it stops
    // with nothing written (BackupStale).
    snprintf(buf, sizeof buf, "Checks the FM-1 still matches %s.", or_(in.backup, "its backup"));
    line(buf);
    if (p.current.known() && family_info(p.current.family).saves_enabled) {
      snprintf(buf, sizeof buf, "Saves %s's songs.", running);
      line(buf);
    }
    snprintf(buf, sizeof buf, "Writes %s: %u sectors.", in.target, unsigned(p.app_sectors));
    line(buf);
    char target[28];
    name_of(in.target, target, sizeof target);
    switch (p.decision.action) {
      case DataAction::Restore:
        snprintf(buf, sizeof buf, "Brings back %s's songs.", target);
        line(buf);
        break;
      case DataAction::FreshStart:
        snprintf(buf, sizeof buf, "Starts %s fresh: no songs saved yet.", target);
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
    line("MIDI stops until it's done.");
  }
  out.line_count = uint8_t(n);
  out.warn_from = uint8_t(warn < 0 ? n : warn);
  if (!in.writes_enabled) PUT(out.hold, "read-only build");
  out.can_act = in.ready && in.writes_enabled && !in.unfinished && in.has_unit && p.ok;
}

// ---------- A running job ----------
void describe_job(const JobInput &in, UiJob &out) {
  out = UiJob{};
  out.active = true;
  out.danger = in.danger;
  const char *fw = or_(in.firmware, "the firmware");
  const char *backup = or_(in.backup, "its backup");
  const uint8_t pct = in.total ? uint8_t(in.done * 100 / in.total) : 0;
  struct Plan {
    const char *done, *next;  // the step's text once done, and still to come
  };
  // Which of the job's steps each Installer step belongs to, and its live text.
  int now = 0;
  char live[40] = "", checked[40], check_next[40];
  Plan steps[kJobSteps] = {};
  int count = 0;
  const Step s = in.step;
  auto restart = [&](int i) {
    steps[i] = {"FM-1 restarted", "Restart the FM-1"};
    if (s == Step::Reset || s == Step::WaitMidi) {
      now = i;
      put(live, sizeof live, "Restarting the FM-1");
    }
  };
  auto rom = [&]() {
    steps[0] = {"FM-1 in ROM boot", "Put the FM-1 in ROM boot"};
    if (s == Step::Idle) put(live, sizeof live, "Restarting this board");
    else if (s == Step::Confirm || s == Step::SendKey) put(live, sizeof live, "Asking the FM-1");
    else if (s == Step::WaitRom) put(live, sizeof live, "Waiting for ROM boot");
    else if (s == Step::Identify || s == Step::UploadLoader || s == Step::ReadIds)
      put(live, sizeof live, "Starting the loader");
  };
  switch (in.kind) {
    case JobKind::Backup:
      PUT(out.title, "Backing up");
      rom();
      steps[1] = {"Read twice and stored", "Read it twice and store it"};
      if (s == Step::Backup) {
        now = 1;
        put(live, sizeof live, "Reading twice: %u%%", unsigned(pct));
      }
      restart(2);
      count = 3;
      PUT(out.note, "Writes nothing to the FM-1.");
      break;
    case JobKind::Resume:
      PUT(out.title, "Retrying %s", fw);
      rom();
      steps[1] = {"Read the FM-1", "Read the FM-1"};
      if (s == Step::ResumeRead || s == Step::Plan) {
        now = 1;
        put(live, sizeof live, "Reading the FM-1: %u%%", unsigned(pct));
      }
      steps[2] = {"Written and checked", "Finish writing"};
      if (s == Step::WriteApp || s == Step::WriteData) {
        now = 2;
        put(live, sizeof live, "Writing %u of %u", unsigned(in.done), unsigned(in.total));
      }
      restart(3);
      count = 4;
      break;
    default:
      PUT(out.title, "Switching to %s", fw);
      rom();
      // The install's one read, compared with the backup it planned from.
      snprintf(checked, sizeof checked, "Checked: same as %s", backup);
      snprintf(check_next, sizeof check_next, "Check it matches %s", backup);
      steps[1] = {checked, check_next};
      if (s == Step::Check) {
        now = 1;
        put(live, sizeof live, "Checking: %u%%", unsigned(pct));
      }
      steps[2] = {"Written and checked", "Write"};
      if (s == Step::WriteApp || s == Step::WriteData) {
        now = 2;
        put(live, sizeof live, "Writing %u of %u", unsigned(in.done), unsigned(in.total));
      }
      restart(3);
      count = 4;
      break;
  }
  out.step_count = uint8_t(count);
  for (int i = 0; i < count; ++i) {
    out.steps[i].state = i < now ? UiStepState::Done : i == now ? UiStepState::Now : UiStepState::Next;
    snprintf(out.steps[i].text, sizeof out.steps[i].text, "%s",
             i < now ? steps[i].done : i == now && live[0] ? live : steps[i].next);
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

void describe_result(const JobOutcome &in, UiResult &out) {
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
      PUT(out.why, "The FM-1 took the key but didn't show up in ROM boot.");
      PUT(out.todo, "Switch it off and on, then try again.");
      break;
    case Failure::BackupStale:
      // Today's switch: it stops instead of backing up by itself.
      PUT(out.why, "The FM-1 changed since %s.", or_(in.backup, "its last backup"));
      PUT(out.todo, "Back it up again, then switch.");
      back_up_first(out);
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
