// The install state machine: from the on-screen confirmation to the FM-1 coming
// back as a MIDI device, or the resume entry when the Waveshare finds an FM-1
// already in ROM boot (4C4A:8057). Each step has a failure exit; step() does a
// small piece of work (one sector at most) so the UI can show progress.
//
// Order of an install:
//   Confirm (plans from the unit's latest stored backup, records the pending
//   install), SendKey, WaitRom, Identify, UploadLoader, ReadIds, Check (one read,
//   compared with that backup), WriteApp, WriteData, Reset, WaitMidi, Done.
// Planning before the key keeps the loader's time before the first erase to one
// read of the flash: in checkpoint 3 the FM-1 restarted itself about 8.5 s after
// the loader started when no erase came. With nothing to write, there's no key.
// A backup: Confirm, SendKey ... ReadIds, Backup (two reads), Reset, WaitMidi.
// A resume starts at Identify, then reads the unit (ResumeRead) and finishes the
// install its unit record says was pending, using the stored verified backup.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "library.h"
#include "rom.h"

namespace fm1_loader {

// The SHA-256 of wl82loader.bin from jl-uboot-tool adb3f188.
extern const char *const kLoaderSha256;

// What the firmware provides around the ROM device.
class LoaderHost {
 public:
  virtual ~LoaderHost() = default;
  // Sends the ROM-boot key F0 22 24 35 7D F7 to the FM-1. The only sender of it.
  virtual bool send_rom_boot_key() = 0;
  // Waits for 4C4A:8057 to attach and be ready for SCSI commands.
  virtual bool wait_for_rom_device(uint32_t timeout_ms) = 0;
  // After the reset: waits for the FM-1 to come back as a USB-MIDI device.
  virtual bool wait_for_midi_return(uint32_t timeout_ms) = 0;
  // A millisecond clock, for the time from the loader's start to the first erase.
  virtual uint32_t now_ms() { return 0; }
};

enum class Step {
  Idle, Confirm, SendKey, WaitRom, Identify, UploadLoader, ReadIds, Backup, ResumeRead, Plan,
  WriteApp, WriteData, Reset, WaitMidi, Done, Failed, Check,
};

// An install stops before its first erase if the loader has run this long.
constexpr uint32_t kLoaderBudgetMs = 6000;

enum class Failure {
  None,
  NotConfirmed,        // no explicit on-screen confirmation
  KeyNotSent,
  NoRomDevice,         // 4C4A:8057 never answered
  NotWl82,             // inquiry isn't WL82
  NoLoader,            // not UBOOT1.00 and no loader answering
  LoaderBlobBad,       // wl82loader.bin doesn't match its SHA-256
  LoaderFailed,        // the upload or the jump failed
  IdentityMismatch,    // chip key or flash ID isn't the FM-1's
  DeviceLost,          // a command failed: unplugged, or the device stopped answering
  BackupMismatch,      // the two backup reads differ
  NoBackupForInstall,  // install: no stored backup of this FM-1 to plan from
  BackupStale,         // install: the FM-1 differs from the backup the install planned from
  LoaderTooSlow,       // install: the first erase would come too late after the loader started
  BlankUnitSector,     // 0xE9000 is blank: can't tell this FM-1 apart
  StorageFailed,       // the Waveshare's storage couldn't record the backup or plan
  PackageMissing,      // the package isn't in the library, or a sector of it is gone
  NeedsFreshStartOk,   // the new firmware starts fresh and the user hasn't agreed
  NeedsLayoutChangeOk, // the family changes but the data stays (stock): not agreed
  RestoreRefused,      // the chosen save doesn't belong to this unit and firmware
  GuardRefused,        // a planned erase/write is outside what its right allows
  NoBackupForResume,   // resume: this unit has no stored verified backup
  NothingToResume,     // resume: this unit has no unfinished install
  WriteVerifyFailed,   // a sector read back wrong twice
  MidiNotBack,         // flash verified, but the FM-1 didn't come back as MIDI
  WritesLocked,        // this build can't write the FM-1 (FM1_LOADER_WRITES off)
  Blocked,             // the loader can't take a unit to this firmware yet (DataDecision::blocked)
};
const char *failure_text(Failure f);

struct InstallOptions {
  bool confirmed = false;       // the 2-second hold on the confirm screen
  bool fresh_start_ok = false;  // the user agreed to erase data: a fresh start or a clean slate
  bool layout_change_ok = false;  // the user saw the data-layout warning and went on
  std::string unit_name;        // for a unit seen for the first time
  // Install: the unit the preview was for (empty: the unit backed up last).
  std::string unit_id;
  // Resume only: this board may have left its loader running in the FM-1 (it
  // stopped the install itself and didn't reset). The resume asks the loader
  // first and skips the upload when it answers.
  bool loader_running = false;
  // The key went out before the board restarted (no ROM device showed up): the
  // job runs again from waiting for ROM boot, without a second key.
  bool key_sent = false;
  uint32_t rom_timeout_ms = 10000;
  uint32_t midi_timeout_ms = 15000;
};

class Installer {
 public:
  Installer(ScsiTransport &t, LoaderHost &host, Library &lib, const std::vector<uint8_t> &loader_blob)
      : rom_(t), host_(host), lib_(lib), blob_(loader_blob) {}

  void start_install(const std::string &package_id, const InstallOptions &opts);
  void start_resume(const InstallOptions &opts = InstallOptions());
  // "Back up this FM-1": identify, the double read, record the backup and the
  // autosave, reset. Writes nothing to the FM-1, so it runs in locked builds.
  void start_backup(const InstallOptions &opts);
  bool step();  // false once Done or Failed
  void run();

  Step current() const { return step_; }
  Failure failure() const { return failure_; }
  // True while the app area may be incomplete: the screen says "Don't unplug:
  // retry or restore backup" and the FM-1 stays in ROM boot.
  bool danger() const { return danger_; }
  const Unit &unit() const { return unit_; }
  const std::string &backup_id() const { return backup_id_; }  // the backup this run recorded
  // Where a failure happened, for the screen and the log: the command, the
  // flash address and how far it got ("erase 0x5A000 try 1, sector 3 of 140").
  // Empty unless the last run failed.
  const std::string &failure_detail() const { return detail_; }
  const DataDecision &decision() const { return decision_; }
  uint32_t sectors_done() const { return done_; }
  uint32_t sectors_total() const { return total_; }
  uint32_t io_chunk() const { return chunk_; }

 private:
  void reset_state(const InstallOptions &opts);
  void fail(Failure f, bool reset_device = false);
  void finish_record();
  bool read_sector(uint32_t addr, uint8_t *out);
  bool write_sector(const SectorOp &op);
  void do_identify();
  void do_upload();
  void do_read_ids();
  void do_backup();
  void do_resume_read();
  void do_plan();
  void do_write(std::vector<SectorOp> &ops, Step next);
  bool check_ops(const std::vector<SectorOp> &ops);
  bool prepare_install();
  bool plan_ops();
  bool autosave();
  void do_check();

  RomClient rom_;
  LoaderHost &host_;
  Library &lib_;
  const std::vector<uint8_t> blob_;  // a copy: 24 KB, and no dangling reference
  InstallOptions opts_;
  bool resume_ = false;
  bool backup_only_ = false;
  std::string backup_id_;
  std::string detail_;
  std::string package_id_;

  Step step_ = Step::Idle;
  Failure failure_ = Failure::None;
  bool danger_ = false;
  bool in_rom_ = false;      // the FM-1 is in ROM boot: a reset sends it home
  bool erased_ = false;      // an erase may have reached the FM-1
  bool pending_set_ = false;  // this run recorded the unit's pending install
  uint32_t loader_ms_ = 0;   // when the loader started (host clock)
  uint32_t chunk_ = 512;
  uint32_t index_ = 0;  // sector being worked on in Backup/ResumeRead/Write*
  int pass_ = 1;
  bool mismatch_ = false;
  FlashHashes flash_;
  Unit unit_;
  Firmware current_;
  DataDecision decision_;
  std::vector<SectorOp> app_ops_, data_ops_;
  uint32_t done_ = 0, total_ = 0;
};

}  // namespace fm1_loader
