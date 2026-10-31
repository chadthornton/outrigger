// The loader's library on top of the sector store: firmware packages, FM-1 units
// (told apart by their Bluetooth data sector), their backups and save states,
// and the planning that turns "install X on this unit" into sector operations.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "layout.h"
#include "package.h"
#include "store.h"

namespace fm1_loader {

// One hash per flash sector of a whole-flash image.
using FlashHashes = std::vector<Hash>;  // kSectors entries

struct PackageInfo {
  std::string id;  // library name, e.g. "skiff-1.4"
  std::string name, version;  // for the screen, e.g. "Skiff", "1.4" (may be empty)
  bool pinned = false;        // a favorite: never evicted
  std::string identity;
  Family family = Family::Felucca;
  Hash file_sha256{};
  Hash app_sha256{};
  std::vector<Hash> app;  // kAppSectors hashes, from kAppStart
};

// What a unit runs, as far as the loader knows.
struct Firmware {
  std::string identity;  // empty when the app area matches no library package
  Hash app_sha256{};
  Family family = Family::Felucca;
  bool known() const { return !identity.empty(); }
};

enum class DataAction {
  Keep,        // leave the data areas as they are (no-op install, or an update)
  Restore,     // write back a save state of this unit and firmware
  FreshStart,  // erase the target family's save ranges (needs the user's OK)
};

// The install the loader has started on a unit but not finished. Written and
// synced before the first erase, so a resume knows what to finish.
struct PendingInstall {
  std::string package_id;
  DataAction action = DataAction::Keep;
  std::string save_id;
  std::string before_backup;  // the verified backup taken just before
};

struct Unit {
  std::string id;    // 16 hex digits of SHA-256(sector 0xE9000)
  std::string name;  // empty until the user names it
  std::string baseline_backup;  // first sight, kept forever
  std::string latest_backup;
  std::string installed_package;  // library id, empty if unknown
  uint32_t next_seq = 1;
  bool has_pending = false;
  PendingInstall pending;
};

struct SaveState {
  std::string id;
  std::string unit;
  std::string identity;
  Hash app_sha256{};
  Family family = Family::Felucca;
  std::string label;
  uint32_t seq = 0;  // orders saves of one unit, newest highest
  std::vector<std::pair<uint32_t, Hash>> sectors;  // non-blank sectors of the family's ranges
};

// One erase (and, unless want is blank, write) of a whole sector.
struct SectorOp {
  uint32_t addr;
  Hash want;
  Right right;
  Family family;  // for Right::SaveRestore
};

// Unit ID from the 0xE9000 sector. Empty if the sector is blank (all 0xFF or
// 0x00): such units would all collide, so they can't be identified.
std::string unit_id_of(const uint8_t *sector);

class Library {
 public:
  explicit Library(Storage &storage) : storage_(storage), sectors_(storage) {}
  SectorStore &sectors() { return sectors_; }

  // Parses, checks and stores a .fwsc file under id.
  PackageError add_package(const std::string &id, const uint8_t *data, size_t len,
                           const Hash *expected_sha256 = nullptr, const std::string &name = "",
                           const std::string &version = "");
  bool package(const std::string &id, PackageInfo &out);
  std::vector<std::string> package_ids();
  // Refused while a unit has it installed or an unfinished install needs it.
  bool remove_package(const std::string &id);
  bool set_pinned(const std::string &id, bool pinned);
  // What an app area with these sector hashes is (identity empty if unknown).
  Firmware identify_app(const std::vector<Hash> &app);

  bool unit(const std::string &id, Unit &out);
  bool put_unit(const Unit &u);  // writes and syncs
  std::vector<std::string> unit_ids();

  // Records a verified whole-flash backup (sectors already in the store) and
  // makes it the unit's latest; the first one is also its baseline. Synced.
  bool add_backup(Unit &u, const FlashHashes &flash, std::string &backup_id);
  bool backup(const std::string &id, FlashHashes &out);
  // SHA-256 of flash [start, end) as a backup holds it (sector-aligned).
  bool region_sha256(const std::string &backup_id, uint32_t start, uint32_t end, Hash &out);

  // The unit whose backup was recorded last: the FM-1 the screen talks about.
  std::string last_unit();
  bool add_save(Unit &u, SaveState &s);  // assigns id and seq; synced
  bool save(const std::string &id, SaveState &out);
  std::vector<SaveState> saves_of(const std::string &unit_id);

  // Drops stored sectors that no package, backup or save refers to.
  size_t collect_garbage();

 private:
  bool put_manifest(const std::string &key, const Manifest &m);
  bool get_manifest(const std::string &key, Manifest &m);
  Storage &storage_;
  SectorStore sectors_;
};

// Save state of the data the backup holds for this firmware's family.
// Empty sectors list when the family has saves disabled.
SaveState capture_save(const std::string &unit_id, const Firmware &fw, const FlashHashes &flash);

struct DataDecision {
  DataAction action = DataAction::Keep;
  std::string save_id;
  bool layout_warning = false;   // the identity changes: old data may not suit the new firmware
  bool stock_unmapped = false;   // target is stock: data kept in place, never saved or erased
};

// The plan's default restore rules (decision 0):
// - same identity and app as now: keep (no-op install);
// - a save of this unit with the target's identity and app: restore the newest;
// - same identity, different app (an update): keep, the autosave is the rollback;
// - otherwise: fresh start for a Felucca-family target, keep (with a warning)
//   for stock, whose data map isn't known yet.
DataDecision decide_data(const std::string &unit_id, const Firmware &current,
                         const PackageInfo &target, const std::vector<SaveState> &saves);

// What installing a package on a unit would do, from the unit's latest backup:
// shown before the hold. The install itself plans again from a fresh backup.
struct InstallPreview {
  bool ok = false;
  const char *problem = "";  // when !ok
  Firmware current;          // what the unit ran at its latest backup
  DataDecision decision;
  uint32_t app_sectors = 0, data_sectors = 0;
};
class Library;
InstallPreview preview_install(Library &lib, const std::string &unit_id, const std::string &package_id);

// App sectors that differ between the unit's flash and the package.
std::vector<SectorOp> plan_app(const FlashHashes &flash, const PackageInfo &target);

// Data sectors to write so the target family's save ranges hold exactly the
// save: its sectors, and blank everywhere else in those ranges. Refused (false)
// for another unit's save or another firmware's.
bool plan_restore(const SaveState &s, const std::string &unit_id, const PackageInfo &target,
                  const FlashHashes &flash, std::vector<SectorOp> &ops);

// Non-blank sectors of the target family's save ranges, to erase.
std::vector<SectorOp> plan_fresh(Family family, const FlashHashes &flash);

}  // namespace fm1_loader
