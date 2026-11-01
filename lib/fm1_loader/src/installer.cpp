#include "installer.h"

#include <stdio.h>
#include <string.h>

namespace fm1_loader {

const char *const kLoaderSha256 = "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376";

namespace {

std::string where(const char *what, uint32_t addr, const char *then = "") {
  char buf[64];
  snprintf(buf, sizeof buf, "%s 0x%05X%s", what, unsigned(addr), then);
  return buf;
}

// The wl82 ROM unscrambles MengLi itself (chips.yaml: memory-rw-mengli-crypt) and
// the loader is MengLi (usb-loaders.yaml).
constexpr bool kRomMengliQuirk = true;
constexpr bool kLoaderIsMengli = true;

// Flash I/O chunk: the loader's USB buffer size, as jl-uboot-tool uses it, kept to
// a size that divides a sector so every chunk stays inside one sector.
uint32_t chunk_for(uint32_t io_max) {
  if (io_max < 16 || io_max > 0xFFFF) io_max = 512;  // fm1_unbrick.py open_real_device
  if (io_max <= kSector && kSector % io_max == 0) return io_max;
  uint32_t c = 16;
  while (c * 2 <= io_max && c * 2 <= kSector) c *= 2;
  return c;
}

}  // namespace

const char *failure_text(Failure f) {
  switch (f) {
    case Failure::None: return "ok";
    case Failure::NotConfirmed: return "not confirmed on screen";
    case Failure::KeyNotSent: return "couldn't send the ROM-boot key";
    case Failure::NoRomDevice: return "the FM-1 didn't enter ROM boot";
    case Failure::NotWl82: return "not an FM-1 (WL82) in ROM boot";
    case Failure::NoLoader: return "ROM boot device doesn't answer the loader";
    case Failure::LoaderBlobBad: return "wl82loader.bin doesn't match its SHA-256";
    case Failure::LoaderFailed: return "loader upload failed";
    case Failure::IdentityMismatch: return "chip key or flash ID isn't an FM-1's";
    case Failure::DeviceLost: return "the FM-1 stopped answering";
    case Failure::BackupMismatch: return "the two backup reads differ";
    case Failure::NoBackupForInstall: return "back up this FM-1 first";
    case Failure::BackupStale: return "the FM-1 changed since its backup: back up again";
    case Failure::LoaderTooSlow: return "the FM-1 was too slow to read: nothing written";
    case Failure::BlankUnitSector: return "the unit's Bluetooth sector is blank";
    case Failure::StorageFailed: return "the Waveshare's storage failed";
    case Failure::PackageMissing: return "package missing or damaged in the library";
    case Failure::NeedsFreshStartOk: return "the new firmware needs a fresh start: confirm it";
    case Failure::NeedsLayoutChangeOk: return "old settings may crash the new firmware: confirm it";
    case Failure::RestoreRefused: return "save state doesn't match this unit and firmware";
    case Failure::GuardRefused: return "refused: outside the allowed flash areas";
    case Failure::NoBackupForResume: return "no stored backup of this unit";
    case Failure::NothingToResume: return "no unfinished install on this unit";
    case Failure::WriteVerifyFailed: return "a sector didn't verify twice";
    case Failure::MidiNotBack: return "installed, but the FM-1 didn't come back as MIDI";
    case Failure::WritesLocked: return "this build can't write the FM-1";
    case Failure::Blocked: return "not yet supported: it writes the FM-1's Bluetooth sector";
  }
  return "?";
}

void Installer::reset_state(const InstallOptions &opts) {
  opts_ = opts;
  resume_ = false;
  backup_only_ = false;
  backup_id_.clear();
  detail_.clear();
  package_id_.clear();
  step_ = Step::Idle;
  failure_ = Failure::None;
  danger_ = false;
  in_rom_ = false;
  erased_ = false;
  pending_set_ = false;
  loader_ms_ = 0;
  chunk_ = 512;
  index_ = 0;
  pass_ = 1;
  mismatch_ = false;
  flash_.clear();
  unit_ = Unit();
  current_ = Firmware();
  decision_ = DataDecision();
  app_ops_.clear();
  data_ops_.clear();
  done_ = total_ = 0;
}

void Installer::start_install(const std::string &package_id, const InstallOptions &opts) {
  reset_state(opts);
  package_id_ = package_id;
  step_ = Step::Confirm;
}

void Installer::start_backup(const InstallOptions &opts) {
  reset_state(opts);
  backup_only_ = true;
  step_ = Step::Confirm;
}

void Installer::start_resume(const InstallOptions &opts) {
  reset_state(opts);
  if (!kWritesEnabled) return fail(Failure::WritesLocked);
  resume_ = true;
  danger_ = true;  // the app area may be half-written: never leave ROM boot on a failure
  in_rom_ = true;
  erased_ = true;  // an earlier run may have erased: the pending record stays
  step_ = Step::Identify;
}

void Installer::run() {
  while (step()) {
  }
}

void Installer::fail(Failure f, bool reset_device) {
  failure_ = f;
  if (!erased_) {
    // Nothing reached the FM-1's flash: it's as it was, and so is its record.
    danger_ = false;
    if (pending_set_) {
      unit_.has_pending = false;
      unit_.pending = PendingInstall();
      lib_.put_unit(unit_);
      pending_set_ = false;
    }
  }
  // Only an FM-1 whose app area is untouched goes back to its firmware.
  if (reset_device && in_rom_ && !resume_ && !danger_) rom_.run_app(kResetArg);
  step_ = Step::Failed;
}

bool Installer::step() {
  switch (step_) {
    case Step::Idle:
    case Step::Done:
    case Step::Failed:
      return false;
    case Step::Confirm: {
      PackageInfo p;
      if (!opts_.confirmed) return fail(Failure::NotConfirmed), false;
      // Before the key: once in ROM boot, only a running loader can reset the FM-1.
      if (hex(sha256(blob_.data(), blob_.size())) != kLoaderSha256)
        return fail(Failure::LoaderBlobBad), false;
      if (backup_only_) {
        step_ = opts_.key_sent ? Step::WaitRom : Step::SendKey;
        break;
      }
      if (!kWritesEnabled) return fail(Failure::WritesLocked), false;
      if (!lib_.package(package_id_, p)) return fail(Failure::PackageMissing), false;
      for (const Hash &h : p.app)
        if (!lib_.sectors().has(h)) return fail(Failure::PackageMissing), false;
      if (!prepare_install()) return false;
      if (total_ == 0) {  // already installed: no ROM boot needed
        unit_.installed_package = package_id_;
        lib_.put_unit(unit_);
        step_ = Step::Done;
        return false;
      }
      step_ = opts_.key_sent ? Step::WaitRom : Step::SendKey;
      break;
    }
    case Step::SendKey:
      if (!host_.send_rom_boot_key()) return fail(Failure::KeyNotSent), false;
      step_ = Step::WaitRom;
      break;
    case Step::WaitRom:
      if (!host_.wait_for_rom_device(opts_.rom_timeout_ms)) return fail(Failure::NoRomDevice), false;
      in_rom_ = true;
      step_ = Step::Identify;
      break;
    case Step::Identify: do_identify(); break;
    case Step::UploadLoader: do_upload(); break;
    case Step::ReadIds: do_read_ids(); break;
    case Step::Backup: do_backup(); break;
    case Step::ResumeRead: do_resume_read(); break;
    case Step::Check: do_check(); break;
    case Step::Plan: do_plan(); break;
    case Step::WriteApp: do_write(app_ops_, Step::WriteData); break;
    case Step::WriteData: do_write(data_ops_, Step::Reset); break;
    case Step::Reset:
      rom_.run_app(kResetArg);  // the device may reset before it answers
      step_ = Step::WaitMidi;
      break;
    case Step::WaitMidi:
      if (!host_.wait_for_midi_return(opts_.midi_timeout_ms)) return fail(Failure::MidiNotBack), false;
      step_ = Step::Done;
      break;
  }
  return step_ != Step::Done && step_ != Step::Failed;
}

void Installer::do_identify() {
  std::string vendor, product, revision;
  if (!rom_.inquiry(vendor, product, revision)) return fail(Failure::NoRomDevice);
  std::string upper = vendor;
  for (char &c : upper) c = char(c >= 'a' && c <= 'z' ? c - 32 : c);
  if (upper != "WL82") return fail(Failure::NotWl82);  // vendor.upper(), as fm1_unbrick.py
  if (resume_ && opts_.loader_running) {
    // Ask the loader itself; if it doesn't answer, the FM-1 lost it (a bus reset,
    // say) and the upload below runs as usual.
    uint8_t type;
    uint32_t id;
    if (rom_.online_device(type, id)) {
      step_ = Step::ReadIds;
      return;
    }
  }
  if (product == "UBOOT1.00") {
    step_ = Step::UploadLoader;
    return;
  }
  // Not the ROM's own name: a loader may already be running (fm1_unbrick.py).
  uint8_t type;
  uint32_t id;
  if (!rom_.online_device(type, id)) return fail(Failure::NoLoader);
  loader_ms_ = host_.now_ms();  // running already: from now, at the latest
  step_ = Step::ReadIds;
}

void Installer::do_upload() {
  if (hex(sha256(blob_.data(), blob_.size())) != kLoaderSha256) return fail(Failure::LoaderBlobBad);
  const bool scramble = loader_needs_host_scramble(kRomMengliQuirk, kLoaderIsMengli);
  uint8_t block[kLoaderBlock];
  for (size_t off = 0; off < blob_.size(); off += kLoaderBlock) {
    size_t n = blob_.size() - off < kLoaderBlock ? blob_.size() - off : kLoaderBlock;
    memcpy(block, blob_.data() + off, n);
    if (scramble) mengli(block, n);
    if (!rom_.mem_write(kLoaderAddr + uint32_t(off), block, uint16_t(n)))
      return fail(Failure::LoaderFailed);
  }
  if (!rom_.mem_jump(kLoaderAddr, kLoaderArg)) return fail(Failure::LoaderFailed);
  loader_ms_ = host_.now_ms();
  step_ = Step::ReadIds;
}

void Installer::do_read_ids() {
  uint32_t io_max = 512;
  if (!rom_.usb_buffer_size(io_max)) io_max = 512;
  chunk_ = chunk_for(io_max);
  uint16_t key;
  uint8_t type;
  uint32_t id;
  if (!rom_.chip_key(key) || !rom_.online_device(type, id)) {
    detail_ = "read chip key / flash ID";
    return fail(Failure::DeviceLost);
  }
  if (key != kChipKey || id != kFlashId) return fail(Failure::IdentityMismatch, true);
  index_ = 0;
  pass_ = 1;
  if (resume_) {
    flash_.assign(kSectors, Hash{});
    step_ = Step::ResumeRead;
  } else if (backup_only_) {
    flash_.assign(kSectors, Hash{});
    step_ = Step::Backup;
  } else {
    done_ = 0;
    total_ = kSectors;
    step_ = Step::Check;  // flash_ holds the backup the install planned from
  }
}

bool Installer::read_sector(uint32_t addr, uint8_t *out) {
  for (uint32_t off = 0; off < kSector; off += chunk_)
    if (!rom_.flash_read(addr + off, out + off, uint16_t(chunk_))) return false;
  return true;
}

// The whole flash twice, compared sector by sector. Pass 1 goes into the store.
void Installer::do_backup() {
  std::vector<uint8_t> buf(kSector);
  uint32_t addr = index_ * kSector;
  if (!read_sector(addr, buf.data())) {
    detail_ = where(pass_ == 1 ? "backup read 1 at" : "backup read 2 at", addr);
    return fail(Failure::DeviceLost);
  }
  if (pass_ == 1) {
    if (!lib_.sectors().put(buf.data(), flash_[index_])) return fail(Failure::StorageFailed, true);
  } else if (sha256(buf.data(), buf.size()) != flash_[index_]) {
    mismatch_ = true;  // judged after the whole second read, as take_backup() does
  }
  done_ = (pass_ - 1) * kSectors + index_ + 1;
  total_ = 2 * kSectors;
  if (++index_ < kSectors) return;
  index_ = 0;
  if (pass_++ == 1) return;
  if (mismatch_) return fail(Failure::BackupMismatch, true);

  // Both reads agree: now the unit can be named and the backup recorded.
  if (!lib_.sectors().get(flash_[kUnitSector / kSector], buf.data()))
    return fail(Failure::StorageFailed, true);
  std::string id = unit_id_of(buf.data());
  if (id.empty()) return fail(Failure::BlankUnitSector, true);
  if (!lib_.unit(id, unit_)) {
    unit_ = Unit();
    unit_.id = id;
    unit_.name = opts_.unit_name;
  }
  if (!lib_.add_backup(unit_, flash_, backup_id_)) return fail(Failure::StorageFailed, true);
  if (!autosave()) return;
  step_ = Step::Reset;
}

// Autosave the running firmware's data (from flash_): the rollback point and the
// state to come back to. Not repeated when nothing changed.
bool Installer::autosave() {
  std::vector<Hash> app(flash_.begin() + kAppStart / kSector, flash_.begin() + kAppEnd / kSector);
  current_ = lib_.identify_app(app);
  if (current_.known() && family_info(current_.family).saves_enabled) {
    SaveState s = capture_save(unit_.id, current_, flash_);
    std::vector<SaveState> saves = lib_.saves_of(unit_.id);
    bool same = !saves.empty() && saves.back().identity == s.identity &&
                saves.back().app_sha256 == s.app_sha256 && saves.back().sectors == s.sectors;
    if (!same && !lib_.add_save(unit_, s)) return fail(Failure::StorageFailed, true), false;
  }
  return true;
}

// Before the key: the unit's latest stored backup is what the install plans
// from (the preview showed the same), the autosave comes from it, and the
// pending install is durable before the FM-1 leaves its firmware.
bool Installer::prepare_install() {
  const std::string id = opts_.unit_id.empty() ? lib_.last_unit() : opts_.unit_id;
  if (id.empty() || !lib_.unit(id, unit_) || !lib_.backup(unit_.latest_backup, flash_) ||
      flash_.size() != kSectors)
    return fail(Failure::NoBackupForInstall), false;
  if (!autosave() || !plan_ops()) return false;
  if (total_ == 0) return true;
  unit_.has_pending = true;
  unit_.pending.package_id = package_id_;
  unit_.pending.action = decision_.action;
  unit_.pending.save_id = decision_.save_id;
  unit_.pending.clean_slate = decision_.clean_slate;
  unit_.pending.before_backup = unit_.latest_backup;
  if (!lib_.put_unit(unit_)) return fail(Failure::StorageFailed), false;
  pending_set_ = true;
  return true;
}

// Install: one read of the flash, which must equal the backup it planned from;
// then the writes, unless the loader has already run too long.
void Installer::do_check() {
  std::vector<uint8_t> buf(kSector);
  const uint32_t addr = index_ * kSector;
  if (!read_sector(addr, buf.data())) {
    detail_ = where("check read at", addr);
    return fail(Failure::DeviceLost, true);
  }
  if (sha256(buf.data(), buf.size()) != flash_[index_]) {
    detail_ = where("differs from its backup at", addr);
    return fail(Failure::BackupStale, true);
  }
  done_ = index_ + 1;
  if (++index_ < kSectors) return;
  index_ = 0;
  const uint32_t ran = host_.now_ms() - loader_ms_;
  if (ran > kLoaderBudgetMs) {
    char buf2[48];
    snprintf(buf2, sizeof buf2, "loader ran %u ms before the first erase", unsigned(ran));
    detail_ = buf2;
    return fail(Failure::LoaderTooSlow, true);
  }
  done_ = 0;
  total_ = uint32_t(app_ops_.size() + data_ops_.size());
  step_ = Step::WriteApp;
}

// Resume: which unit, its stored backup and unfinished install, then one read of
// the flash as it is now.
void Installer::do_resume_read() {
  std::vector<uint8_t> buf(kSector);
  if (index_ == 0 && unit_.id.empty()) {
    if (!read_sector(kUnitSector, buf.data())) {
      detail_ = where("read unit sector", kUnitSector);
      return fail(Failure::DeviceLost);
    }
    std::string id = unit_id_of(buf.data());
    if (id.empty()) return fail(Failure::BlankUnitSector);
    FlashHashes stored;
    if (!lib_.unit(id, unit_) || !lib_.backup(unit_.latest_backup, stored))
      return fail(Failure::NoBackupForResume);
    if (!unit_.has_pending) return fail(Failure::NothingToResume);
    package_id_ = unit_.pending.package_id;
    return;
  }
  if (!read_sector(index_ * kSector, buf.data())) {
    detail_ = where("resume read at", index_ * kSector);
    return fail(Failure::DeviceLost);
  }
  flash_[index_] = sha256(buf.data(), buf.size());
  done_ = index_ + 1;
  total_ = kSectors;
  if (++index_ < kSectors) return;
  index_ = 0;
  step_ = Step::Plan;
}

bool Installer::check_ops(const std::vector<SectorOp> &ops) {
  for (const SectorOp &op : ops)
    if (!guard(op.addr, kSector, op.right, op.family)) return false;
  return true;
}

bool Installer::plan_ops() {
  PackageInfo target;
  if (!lib_.package(package_id_, target)) return fail(Failure::PackageMissing, true), false;
  for (const Hash &h : target.app)
    if (!lib_.sectors().has(h)) return fail(Failure::PackageMissing, true), false;
  if (resume_) {
    decision_ = DataDecision();
    decision_.action = unit_.pending.action;
    decision_.save_id = unit_.pending.save_id;
    decision_.clean_slate = unit_.pending.clean_slate;
  } else {
    decision_ = decide_data(unit_.id, current_, target, lib_.saves_of(unit_.id));
    if (decision_.blocked) return fail(Failure::Blocked, true), false;
    // A clean slate erases more than the outgoing shelf holds (staging): it asks too.
    if ((decision_.action == DataAction::FreshStart || decision_.clean_slate) && !opts_.fresh_start_ok)
      return fail(Failure::NeedsFreshStartOk, true), false;
    // Safety rule 6: a family change that keeps the old data needs the user's OK too.
    if (decision_.action == DataAction::Keep && decision_.layout_warning && !opts_.layout_change_ok)
      return fail(Failure::NeedsLayoutChangeOk, true), false;
  }
  app_ops_ = plan_app(flash_, target);
  if (!plan_data(lib_, unit_.id, decision_, target, flash_, data_ops_))
    return fail(Failure::RestoreRefused, true), false;
  if (!check_ops(app_ops_) || !check_ops(data_ops_)) return fail(Failure::GuardRefused, true), false;
  for (const SectorOp &op : data_ops_)
    if (!lib_.sectors().has(op.want)) return fail(Failure::RestoreRefused, true), false;
  done_ = 0;
  total_ = uint32_t(app_ops_.size() + data_ops_.size());
  index_ = 0;
  return true;
}

// Resume: after the read of the flash as it is now.
void Installer::do_plan() {
  if (!plan_ops()) return;
  if (total_ == 0) {  // the host died after the last verified sector
    danger_ = false;
    finish_record();
    step_ = Step::Reset;
    return;
  }
  step_ = Step::WriteApp;
}

// Erase, write, read back, compare; once more if it doesn't match.
bool Installer::write_sector(const SectorOp &op) {
  std::vector<uint8_t> want(kSector), back(kSector);
  if (!lib_.sectors().get(op.want, want.data())) return fail(Failure::PackageMissing), false;
  char progress[48];
  for (int attempt = 0; attempt < 2; ++attempt) {
    snprintf(progress, sizeof progress, " try %d, sector %u of %u", attempt + 1, unsigned(done_ + 1),
             unsigned(total_));
    if (!guard(op.addr, kSector, op.right, op.family)) return fail(Failure::GuardRefused), false;
    if (op.right == Right::App) danger_ = true;
    const bool erased = rom_.erase_sector(op.addr);
    // A failed erase may still have erased, unless it never reached the FM-1.
    if (erased || rom_.reached_device()) erased_ = true;
    if (!erased) {
      detail_ = where("erase", op.addr, progress);
      return fail(Failure::DeviceLost), false;
    }
    for (uint32_t off = 0; off < kSector; off += chunk_) {
      if (!guard(op.addr + off, chunk_, op.right, op.family))
        return fail(Failure::GuardRefused), false;
      if (!rom_.flash_write(op.addr + off, want.data() + off, uint16_t(chunk_))) {
        detail_ = where("write", op.addr + off, progress);
        return fail(Failure::DeviceLost), false;
      }
    }
    if (!read_sector(op.addr, back.data())) {
      detail_ = where("read back", op.addr, progress);
      return fail(Failure::DeviceLost), false;
    }
    if (back == want) return true;
  }
  detail_ = where("verify", op.addr, " failed twice");
  return fail(Failure::WriteVerifyFailed), false;
}

void Installer::do_write(std::vector<SectorOp> &ops, Step next) {
  if (index_ < ops.size()) {
    if (!write_sector(ops[index_])) return;
    ++index_;
    ++done_;
    return;
  }
  index_ = 0;
  if (step_ == Step::WriteApp) {
    danger_ = false;  // every differing app sector verified
  } else {
    finish_record();
  }
  step_ = next;
}

void Installer::finish_record() {
  // The backup the install planned from is replaced by the new record below,
  // unless its app is in no package: then it is the only copy of that firmware
  // (Transporter's, say), and it stays.
  FlashHashes before;
  const bool keep_before =
      !unit_.pending.before_backup.empty() && lib_.backup(unit_.pending.before_backup, before) &&
      !lib_.identify_app(std::vector<Hash>(before.begin() + kAppStart / kSector, before.begin() + kAppEnd / kSector))
           .known();
  unit_.installed_package = package_id_;
  if (!keep_before) {
    unit_.has_pending = false;
    unit_.pending = PendingInstall();
  }
  // What the FM-1 holds now: the flash it planned from with every written sector
  // (each one read back) in place. Recorded as the latest backup, so the page's
  // "running" mark and the next install's plan start from the FM-1 as it is.
  // add_backup() keeps the previous latest while it is the pending install's.
  FlashHashes now = flash_;
  for (const std::vector<SectorOp> *ops : {&app_ops_, &data_ops_})
    for (const SectorOp &op : *ops) now[op.addr / kSector] = op.want;
  std::string id;
  const bool recorded = lib_.add_backup(unit_, now, id);
  unit_.has_pending = false;
  unit_.pending = PendingInstall();
  // Not fatal: the flash is verified. Without the record, the next install's
  // check read stops it ("back up again"); a stale pending record only makes a
  // later resume find nothing to write.
  if (!recorded || keep_before) lib_.put_unit(unit_);
}

}  // namespace fm1_loader
