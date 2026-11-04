#include "app_table.h"

#include <string.h>

#include "crypto.h"

namespace fm1_loader {

namespace {

constexpr size_t kRecord = 32;
constexpr size_t kMaxRecords = 12;
constexpr size_t kTail = 64;  // EXT_RESERVED's records after the payload (sdk/Package.cs)

uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return u16(p) | uint32_t(u16(p + 2)) << 16; }

// Open's user partition, from M-VAVE's app/instrument.h and app/project.c (V14):
// 16 project sectors (the last one settings) and a staging sector, then the
// sample bank, whose last four sectors before tuning hold the voice library.
constexpr uint32_t kOpenSettings = kOpenUserStart + 15 * kSector;
constexpr uint32_t kOpenJournal = kOpenUserStart + 16 * kSector;
constexpr uint32_t kOpenSamples = kOpenUserStart + 69632;           // GROOVE_SAMPLE_OFFSET
constexpr uint32_t kOpenTuning = kOpenUserStart + 536576 - kSector;  // GROOVE_TUNING_OFFSET
constexpr uint32_t kOpenLibrary = kOpenTuning - 4 * kSector;          // GROOVE_LIBRARY_OFFSET

bool aligned_range(const Range &r) {
  return r.start % kSector == 0 && r.end % kSector == 0 && r.start < r.end && r.end <= kFlashSize;
}

}  // namespace

bool read_app_table(const uint8_t *flash, size_t len, AppTable &out) {
  out = AppTable();
  if (len < kAppStart + kRecord * kMaxRecords) return false;
  bool head = false, vm = false, btif = false;
  for (size_t i = 0; i < kMaxRecords; ++i) {
    uint8_t r[kRecord];
    memcpy(r, flash + kAppStart + i * kRecord, kRecord);
    // Each 32-byte block has its own key: the chip key and its offset in the area.
    jl_enc(r, kRecord, uint16_t(kChipKey ^ ((i * kRecord) >> 2)));
    if (crc16(r + 2, kRecord - 2) != u16(r)) break;  // the end of the table, or damage
    char name[13] = {};
    memcpy(name, r + 16, 12);
    const uint32_t addr = u32(r + 4), size = u32(r + 8);
    if (i == 0) {
      if (strcmp(name, "app_area_hea") != 0 || size == 0 || size > kFlashSize) return false;
      out.image_end = (kAppStart + size + kTail + kSector - 1) / kSector * kSector;
      head = true;
    } else if (strcmp(name, "VM") == 0 && size <= kFlashSize) {
      out.vm = {addr, addr + size};
      vm = true;
    } else if (strcmp(name, "BTIF") == 0 && size <= kFlashSize) {
      out.btif = {addr, addr + size};
      btif = true;
    }
  }
  if (!head || !vm || !btif || !aligned_range(out.vm) || !aligned_range(out.btif)) {
    out = AppTable();
    return false;
  }
  return true;
}

Layout layout_of(const AppTable &t) {
  if (t.image_end <= kAppStart) return Layout::Unknown;
  if (t.vm.start == kVmStart && t.vm.end == kVmEnd && t.btif.start == kUnitSector &&
      t.btif.end == kUnitSector + kSector && t.image_end <= kAppEnd)
    return Layout::V15;
  if (t.btif.start == kOpenBtif && t.btif.end == kOpenUserStart && t.vm.end == kOpenBtif &&
      t.vm.start == t.image_end)
    return Layout::Open;
  return Layout::Unknown;
}

const char *layout_name(Layout l) {
  switch (l) {
    case Layout::Unknown: return "unknown";
    case Layout::V15: return "V15";
    case Layout::Open: return "Open";
  }
  return "?";
}

OpenMap open_map(const AppTable &t) {
  OpenMap m;
  m.vm = t.vm;
  m.btif = {kOpenBtif, kOpenUserStart};
  m.projects = {kOpenUserStart, kOpenSettings};
  m.settings = {kOpenSettings, kOpenJournal};
  m.journal = {kOpenJournal, kOpenSamples};
  m.samples = {kOpenSamples, kOpenLibrary};
  m.library = {kOpenLibrary, kOpenTuning};
  m.tuning = {kOpenTuning, kKeyMacStart};
  return m;
}

}  // namespace fm1_loader
