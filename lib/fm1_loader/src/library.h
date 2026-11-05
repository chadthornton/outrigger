// The loader's library on top of the sector store: firmware packages, FM-1 units
// (told apart by their Bluetooth data sector), their backups and save states,
// and the planning that turns "install X on this unit" into sector operations.
#pragma once

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "layout.h"
#include "package.h"
#include "store.h"

namespace fm1_loader {

// A library id or project: 1-64 of [A-Za-z0-9._-].
bool key_safe(const std::string &id);

// One hash per flash sector of a whole-flash image.
using FlashHashes = std::vector<Hash>;  // kSectors entries

// A package's project is its Firmware Hub catalog id ("skiff", "felucca",
// "stock"). Data is kept only within a project, and each project has its own
// shelf of saves (docs/package-sources.md). A package the catalog doesn't list
// is sideloaded: its project is "sha:" and the first 16 hex digits of its app hash.
std::string sideload_project(const Hash &app_sha256);
// Stock for the catalog's "stock" project and for sideloaded M-VAVE builds
// (FM-1_0NN); Felucca's ranges for every other.
Family family_of_project(const std::string &project, const std::string &identity);
// Saves from different eras of one project never mix: Skiff 2.0 saves in new
// formats (FM-1_720 and up). 0 for projects with no known break.
int data_era(const std::string &project, const std::string &identity);

struct PackageInfo {
  std::string id;  // library name, e.g. "skiff-1.4"
  std::string name, version;  // for the screen, e.g. "Skiff", "1.4" (may be empty)
  bool pinned = false;        // a favorite: never evicted
  std::string identity;
  std::string project;  // catalog id ("skiff"), or sideload_project() of its app
  Family family = Family::Felucca;  // family_of_project(project)
  Hash file_sha256{};
  Hash app_sha256{};
  std::vector<Hash> app;  // kAppSectors hashes, from kAppStart
};

// What a unit runs, as far as the loader knows.
struct Firmware {
  std::string identity;  // empty when the app area matches no library package
  std::string project;   // the matched package's project; empty when unknown
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
  bool clean_slate = false;   // DataDecision::clean_slate
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
  // The flash chip its last backup read (layout.h kFlashId or kFlashIdUc25hq80;
  // 0: recorded before the board read it, so the FM-1's own flash).
  uint32_t flash_id = 0;
  // A UC25HQ80 unit: the owner held through the risk warning once.
  bool chip_risk_ok = false;
  // The owner held through the riskier-family warning (writes_unit_sector) once.
  bool id_risk_ok = false;
  // Unit IDs of 0xE9000 sectors the owner confirmed as this FM-1's after a
  // firmware changed it (resolve_unit): looked up like its own ID.
  std::vector<std::string> aliases;
};

struct SaveState {
  std::string id;
  std::string unit;
  std::string project;  // the shelf it's on (docs/package-sources.md)
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

// Whether this release of a project is known to save on the UC25HQ80 flash
// (families_gen.h kUc25hq80Saves): from the listed release on, versions
// compared as numbers, a pre-release before its release ("v" ignored).
bool saves_on_uc25hq80(const std::string &project, const std::string &version);
// Which boot head a backup holds, by CRC-32 (layout.h). Only V15 takes
// installs: V15-based firmwares and the clean slate are untested on any other,
// and V16 keeps its Bluetooth address where a clean slate erases. A head sector
// missing from the store counts as Other.
enum class Head { V15, V16, Other };
class Library;
Head head_of(Library &lib, const FlashHashes &flash);
// What the screen says about a head that isn't V15: two lines.
const char *head_problem(Head h);
extern const char *const kHeadProblem2;

// Unit ID from the 0xE9000 sector. Empty if the sector is blank (all 0xFF or
// 0x00): such units would all collide, so they can't be identified.
std::string unit_id_of(const uint8_t *sector);
// Whether a 0xE9000 sector holds just what stock V15 writes there: JieLi VM
// record 102 (the Bluetooth address), length 6, its check byte the low byte of
// CRC-16/XMODEM of the address, then 0xFF (Melodee docs/BLE-HW-FACTS.md 14.2;
// FM-A and FM-B both). The unit ID stays the hash of the whole sector.
bool unit_record_ok(const uint8_t *sector);
// Riskier firmwares (families_gen.h kRiskierIdentity, Chad 2026-11-05): they
// may overwrite 0xE9000 and write nothing else outside the clean slate, so they
// install after the owner holds through a warning once per unit.
bool writes_unit_sector(const std::string &project);

class Library;
// A unit whose 0xE9000 may no longer name it: it runs (or Outrigger put on it)
// a riskier firmware, or its baseline backup's record parsed and its latest's
// doesn't.
bool unit_sector_suspect(Library &lib, const Unit &u);
// The one suspect unit a backup may fall back to; "" when there's none or more
// than one (count, when given, says how many).
std::string fallback_candidate(Library &lib, size_t *count = nullptr);

// Which FM-1 a backup read, from its 0xE9000 sector:
// - Known: a unit's own ID or an alias it was given;
// - New: a sector no unit has, with a Bluetooth record, or (no suspect unit
//   to fall back to) any non-blank one, as before;
// - Fallback: confirmed is fallback_candidate() (the owner held through the
//   screen that named it) and the sector doesn't parse, or it's a valid record
//   Outrigger never saw on a unit that went back to stock with its sector changed
//   (stock writes a new address);
// - Unsure: it doesn't parse, a suspect unit exists, and it wasn't confirmed;
// - Blank: all 0xFF or 0x00, and nothing to fall back to.
enum class UnitMatch { Known, New, Fallback, Unsure, Blank };
struct UnitLookup {
  UnitMatch how = UnitMatch::Blank;
  std::string id;
};
UnitLookup resolve_unit(Library &lib, const uint8_t *sector, const std::string &confirmed);
// A resume's unit, with no one to ask (the FM-1 is in ROM boot): its own ID or
// an alias, else the one unit with an unfinished install whose before-backup
// holds exactly this sector. "" when none.
std::string resume_unit(Library &lib, const uint8_t *sector);

class Library {
 public:
  explicit Library(Storage &storage) : storage_(storage), sectors_(storage) {}
  SectorStore &sectors() { return sectors_; }

  // Parses, checks and stores a .fwsc file under id.
  PackageError add_package(const std::string &id, const uint8_t *data, size_t len,
                           const Hash *expected_sha256 = nullptr, const std::string &name = "",
                           const std::string &version = "", const std::string &project = "");
  bool package(const std::string &id, PackageInfo &out);
  std::vector<std::string> package_ids();
  // Refused while a unit has it installed or an unfinished install needs it.
  bool remove_package(const std::string &id);
  bool set_pinned(const std::string &id, bool pinned);
  // What an app area with these sector hashes is (identity empty if unknown).
  Firmware identify_app(const std::vector<Hash> &app);
  // When identify_app() knows no package: true if the app reads as an M-VAVE
  // stock build (app_scan.h), with its identity and the stored stock package of
  // that identity ("" when none). Only for saying what runs: no package holds
  // these exact bytes, so installs and backups still treat the app as unknown.
  bool recognise_stock(const std::vector<Hash> &app, std::string &identity, std::string &package_id);

  bool unit(const std::string &id, Unit &out);
  bool put_unit(const Unit &u);  // writes and syncs
  // Records a confirmed 0xE9000 sector's ID as one of u's (resolve_unit
  // Fallback). Refused: empty (blank), u's own, or another unit's ID or alias.
  bool add_alias(Unit &u, const std::string &sector_id);
  // The unit with this alias, or "".
  std::string alias_owner(const std::string &sector_id);
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
  bool read_save(const std::string &id, SaveState &out, std::map<std::string, std::string> &app_projects);
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
  // An untested switch: Restore or FreshStart over the whole clean-slate area
  // (Right::CleanSlate), not just the target family's save ranges.
  bool clean_slate = false;
  // Back to stock after Skiff 2.x, which writes stock's resources at
  // 0xEA000-0xFBFFF that the loader can't put back (bench test 1 owed).
  bool stock_resources_warning = false;
  // Why this firmware can't be installed yet, for the screen; null when it can.
  const char *blocked = nullptr;
  // A UC25HQ80 unit (preview_install): its owner hasn't taken the risk yet, so
  // the plan warns and the hold is the OK (InstallOptions::chip_risk_ok).
  bool chip_risk = false;
  // A UC25HQ80 unit, and the target isn't known to save on that chip.
  bool chip_no_saves = false;
  // A riskier target (writes_unit_sector) on a unit whose owner hasn't held
  // through its warning yet (preview_install): the hold is the OK
  // (InstallOptions::id_risk_ok).
  bool id_risk = false;
};

// Why a project can't be installed yet, for the screen; null when it can. The
// list is families_gen.h: it writes the unit-ID sector, or it's built on an
// older Felucca.
const char *blocked_reason(const std::string &project);

// Whether a hardware test proved this switch, in either direction: such a pair
// switches the family map as before; every other switch is a clean slate.
bool tested_pair(const std::string &from_project, const std::string &from_identity,
                 const std::string &to_project, const std::string &to_identity);

// The trust table (docs/package-sources.md):
// - a project the loader can't handle yet (it writes the unit-ID sector): blocked;
// - the same app as now: keep (no-op install);
// - another version of the same project, same data era: keep, the autosave is the rollback;
// - otherwise this unit's newest save on the target project's shelf, same era: restore it;
// - otherwise: fresh start, or for stock (data map unknown) keep in place with a warning.
// Restore and fresh start are a clean slate unless tested_pair(): stock included.
DataDecision decide_data(const std::string &unit_id, const Firmware &current,
                         const PackageInfo &target, const std::vector<SaveState> &saves);

// What installing a package on a unit would do, from the unit's latest backup:
// shown before the hold. The install itself plans again from a fresh backup.
struct InstallPreview {
  bool ok = false;
  const char *problem = "";   // when !ok
  const char *problem2 = "";  // its second line, when it has one
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

// Whether the user must agree before this decision runs (InstallOptions::
// fresh_start_ok): a fresh start, or a clean slate, which erases more than the
// outgoing shelf holds.
bool needs_erase_ok(const DataDecision &d);

// The data sectors a decision writes: its save restored, or a fresh start, each
// on a clean slate when the decision says so. False when its save is missing or
// doesn't fit (another unit's, another project's).
bool plan_data(Library &lib, const std::string &unit_id, const DataDecision &d, const PackageInfo &target,
               const FlashHashes &flash, std::vector<SectorOp> &ops);

// A clean slate: every sector of the clean-slate ranges that differs from the
// save (blank where the save has none, and everywhere without a save).
// Refused (false) for a save plan_restore() would refuse.
bool plan_clean(const SaveState *s, const std::string &unit_id, const PackageInfo &target,
                const FlashHashes &flash, std::vector<SectorOp> &ops);

}  // namespace fm1_loader
