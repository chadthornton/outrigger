// The FM-1's 1 MiB SPI NOR flash: what lives where, and the one gate every erase
// and write goes through. Ranges follow Quixotic7's fm1_unbrick.py and Felucca's
// firmware/src/storage.c; end addresses are exclusive.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fm1_loader {

constexpr uint32_t kFlashSize = 0x100000;
constexpr uint32_t kSector = 0x1000;
constexpr uint32_t kSectors = kFlashSize / kSector;

constexpr uint32_t kHeadEnd = 0x4000;          // 0x0000-0x3FFF boot head: never written
constexpr uint32_t kAppStart = 0x4000;         // app area, the only thing an install writes
constexpr uint32_t kAppEnd = 0x93000;
constexpr uint32_t kAppLen = kAppEnd - kAppStart;
constexpr uint32_t kAppSectors = kAppLen / kSector;  // 143
constexpr uint32_t kVmStart = 0x93000;         // stock V15's settings store (JieLi "VM")
constexpr uint32_t kVmEnd = 0xE9000;
constexpr uint32_t kCleanStart = 0x94000;      // a clean slate erases from here (0x93000 is factory data)
constexpr uint32_t kStagingStart = 0xE0000;    // Felucca update loader and record
constexpr uint32_t kStagingEnd = 0xE5000;
constexpr uint32_t kUnitSector = 0xE9000;      // per-unit Bluetooth data: the unit ID
constexpr uint32_t kFactoryEnd = 0xFC000;      // 0xE9000-0xFBFFF Bluetooth data + factory voices
constexpr uint32_t kGlobalsStart = 0xFC000;    // Felucca-family globals
constexpr uint32_t kGlobalsEnd = 0xFF000;
constexpr uint32_t kKeyMacStart = 0xFF000;     // erased at the factory, never written

constexpr uint32_t kChipKey = 0x980F;
constexpr uint32_t kFlashId = 0x856014;
// The UC25HQ80 some FM-1s ship with: named on screen, never read or written yet.
constexpr uint32_t kFlashIdUc25hq80 = 0xB36014;

struct Range {
  uint32_t start, end;
};

// What a clean slate erases (Right::CleanSlate): stock's VM but its factory
// sector, and the globals. Every save range lies inside it.
constexpr Range kCleanSlate[] = {{kCleanStart, kVmEnd}, {kGlobalsStart, kGlobalsEnd}};
constexpr size_t kCleanSlateCount = sizeof kCleanSlate / sizeof kCleanSlate[0];

// Firmware families differ in where they keep user data, so save states are per family.
enum class Family { Stock, Felucca };

// The ranges a save state of this family holds. Felucca family (verified from
// Felucca's storage.c): songs, FM6 user presets, sample slots, preset banks,
// autosave, voice bank, globals; plus 0x94000-0x96FFF and 0xE8000, which other
// firmwares on this shelf map write. Together they are the clean slate's
// ranges minus update staging 0xE0000-0xE4FFF, which is never saved.
//
// Stock V15: its store is the whole VM (0x93000-0xE8FFF), with the factory data
// in its first sector and an update record at 0xE8F00 (fm1_unbrick.py
// SETTINGS_AREAS). Which of its sectors hold what isn't mapped yet, so stock
// saves stay disabled: stock installs keep data in place and never save or
// restore it. A stock fresh start must never erase 0x93000 (factory data).
struct FamilyInfo {
  Family family;
  const char *name;
  const Range *save_ranges;
  size_t save_range_count;
  bool saves_enabled;
};
const FamilyInfo &family_info(Family f);

// What an erase or write is for. Each right has its own allowed ranges.
enum class Right {
  App,            // installing a package: 0x4000-0x92FFF only
  SaveRestore,    // restoring or freshly erasing a family's save ranges
  SettingsErase,  // the explicit "erase settings" repair: 0x93000-0xE8FFF, 0xFC000-0xFEFFF
  CleanSlate,     // a switch no test has proven: 0x94000-0xE8FFF, 0xFC000-0xFEFFF, any family
};

// The single gate: true if [addr, addr+len) may be erased or written for this
// right. Always refused: empty or unaligned-to-nothing requests, anything past
// the flash, the boot head, the Bluetooth data and factory voices, key_mac.
// SaveRestore also needs the family's save ranges and never reaches staging.
// why (optional) gets a reason when refused.
bool guard(uint32_t addr, uint32_t len, Right right, Family family, const char **why = nullptr);

// Whether a whole-flash address range lies inside one of the given ranges.
bool inside(uint32_t addr, uint32_t len, const Range *ranges, size_t count);

}  // namespace fm1_loader
