#include "layout.h"

namespace fm1_loader {

namespace {

const Range kFeluccaSave[] = {
    {0x97000, 0x9F000},  // projects / songs
    {0xA0000, 0xDC000},  // user sample slots
    {0xDC000, 0xE0000},  // user preset banks
    {0xE5000, 0xE7000},  // autosave A/B
    {0xE7000, 0xE8000},  // voice bank
    {0xFC000, 0xFF000},  // settings / globals
};

const Range kSettingsErase[] = {{kVmStart, kVmEnd}, {kGlobalsStart, kGlobalsEnd}};
const Range kApp[] = {{kAppStart, kAppEnd}};

const FamilyInfo kStock = {Family::Stock, "stock", nullptr, 0, false};
const FamilyInfo kFelucca = {Family::Felucca, "felucca", kFeluccaSave,
                             sizeof kFeluccaSave / sizeof kFeluccaSave[0], true};

bool overlaps(uint64_t a, uint64_t a_end, uint32_t b, uint32_t b_end) {
  return a < b_end && a_end > b;
}

bool refuse(const char **why, const char *reason) {
  if (why) *why = reason;
  return false;
}

}  // namespace

const FamilyInfo &family_info(Family f) { return f == Family::Felucca ? kFelucca : kStock; }

bool inside(uint32_t addr, uint32_t len, const Range *ranges, size_t count) {
  uint64_t end = uint64_t(addr) + len;
  for (size_t i = 0; i < count; ++i)
    if (addr >= ranges[i].start && end <= ranges[i].end) return true;
  return false;
}

bool guard(uint32_t addr, uint32_t len, Right right, Family family, const char **why) {
  uint64_t end = uint64_t(addr) + len;
  if (len == 0) return refuse(why, "empty erase/write");
  if (end > kFlashSize) return refuse(why, "past the end of the flash");
  if (overlaps(addr, end, 0, kHeadEnd)) return refuse(why, "boot head 0x0-0x3FFF");
  if (overlaps(addr, end, kUnitSector, kFactoryEnd))
    return refuse(why, "Bluetooth data and factory voices 0xE9000-0xFBFFF");
  if (overlaps(addr, end, kKeyMacStart, kFlashSize)) return refuse(why, "key_mac 0xFF000-0xFFFFF");
  switch (right) {
    case Right::App:
      if (!inside(addr, len, kApp, 1)) return refuse(why, "outside the app area 0x4000-0x92FFF");
      return true;
    case Right::SettingsErase:
      if (!inside(addr, len, kSettingsErase, 2)) return refuse(why, "outside the settings areas");
      return true;
    case Right::SaveRestore: {
      if (overlaps(addr, end, kStagingStart, kStagingEnd))
        return refuse(why, "update staging 0xE0000-0xE4FFF");
      const FamilyInfo &f = family_info(family);
      if (!f.saves_enabled) return refuse(why, "this family has no save ranges");
      if (!inside(addr, len, f.save_ranges, f.save_range_count))
        return refuse(why, "outside the family's save ranges");
      return true;
    }
  }
  return refuse(why, "unknown right");
}

}  // namespace fm1_loader
