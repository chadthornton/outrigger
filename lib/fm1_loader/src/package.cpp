#include "package.h"

#include <string.h>

namespace fm1_loader {

namespace {

constexpr size_t kBlock = 0x2F;       // data bytes per marked block
constexpr size_t kMarkedBlocks = 20;  // the first 20 blocks each carry one marker byte
constexpr uint8_t kNoChar = 0x7D;
constexpr size_t kUfwHeader = 0x40;
constexpr size_t kUfwEntry = 0x50;
constexpr size_t kUfwMin = 0x400;

uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return u16(p) | uint32_t(u16(p + 2)) << 16; }

}  // namespace

const char *package_error_text(PackageError e) {
  switch (e) {
    case PackageError::None: return "ok";
    case PackageError::TooShort: return "too short to be an FM-1 package";
    case PackageError::HeaderCrc: return "UFW header CRC fails";
    case PackageError::EntryCount: return "UFW header lists a bad entry count";
    case PackageError::EntryCrc: return "UFW entry list CRC fails";
    case PackageError::NoFlashBin: return "no flash.bin entry";
    case PackageError::FlashBinPastEnd: return "flash.bin runs past the package";
    case PackageError::FlashBinShort: return "flash.bin doesn't reach 0x93000";
    case PackageError::BadIdentity: return "identity isn't FM-1_ and 3-15 digits";
    case PackageError::BlankApp: return "app area is blank";
    case PackageError::HashMismatch: return "file hash doesn't match the library";
    case PackageError::BadId: return "bad library id";
    case PackageError::StoreFailed: return "storage write failed";
  }
  return "?";
}

// FM-1_ and 3 to 15 digits, all the package marks can hold (kMarkedBlocks). Catalog
// firmwares use 3 to 10: stock FM-1_015, x0x FM-1_9010100, mope1 FM-1_6673000300.
bool valid_identity(const std::string &id) {
  if (id.size() < 8 || id.size() > kMarkedBlocks || id.compare(0, 5, "FM-1_") != 0) return false;
  for (size_t i = 5; i < id.size(); ++i)
    if (id[i] < '0' || id[i] > '9') return false;
  return true;
}

Family family_of(const std::string &id) {
  return valid_identity(id) && id[5] == '0' ? Family::Stock : Family::Felucca;
}

PackageError parse_package(const uint8_t *data, size_t len, Package &out,
                           const Hash *expected_sha256) {
  out = Package();
  out.file_sha256 = sha256(data, len);
  if (expected_sha256 && *expected_sha256 != out.file_sha256) return PackageError::HashMismatch;

  // Undo the .fwsc framing: drop one marker byte after each of the first 20 blocks.
  const size_t stride = kBlock + 1;
  if (len < stride * kMarkedBlocks) return PackageError::TooShort;
  std::vector<uint8_t> image;
  image.reserve(len);
  for (size_t i = 0; i < kMarkedBlocks; ++i) {
    const uint8_t *block = data + i * stride;
    image.insert(image.end(), block, block + kBlock);
    if (block[kBlock] != kNoChar) out.identity += char(uint8_t(block[kBlock] - i - 1));
  }
  image.insert(image.end(), data + stride * kMarkedBlocks, data + len);

  if (image.size() < kUfwMin) return PackageError::TooShort;
  uint8_t hdr[kUfwHeader];
  memcpy(hdr, image.data(), kUfwHeader);
  jl_enc(hdr, kUfwHeader);
  if (crc16(hdr + 2, kUfwHeader - 2) != u16(hdr)) return PackageError::HeaderCrc;
  size_t entries = u16(hdr + 8);
  if (entries == 0 || entries > 11) return PackageError::EntryCount;
  // The entry-list CRC covers the entries still scrambled.
  if (crc16(image.data() + kUfwHeader, entries * kUfwEntry) != u16(hdr + 2))
    return PackageError::EntryCrc;
  bool found = false;
  for (size_t i = 0; i < entries; ++i) {
    uint8_t e[kUfwEntry];
    memcpy(e, image.data() + kUfwHeader + i * kUfwEntry, kUfwEntry);
    jl_enc(e, kUfwEntry);
    if (u16(e) == 0) {  // the last type-0 entry wins, as in ufw_flash()
      out.flash_offset = u32(e + 8);
      out.flash_len = u32(e + 12);
      found = true;
    }
  }
  if (!found) return PackageError::NoFlashBin;
  if (uint64_t(out.flash_offset) + out.flash_len > image.size())
    return PackageError::FlashBinPastEnd;
  if (out.flash_len < kAppEnd) return PackageError::FlashBinShort;
  if (!valid_identity(out.identity)) return PackageError::BadIdentity;

  const uint8_t *flash = image.data() + out.flash_offset;
  out.family = family_of(out.identity);
  out.app.assign(flash + kAppStart, flash + kAppEnd);
  out.app_sha256 = sha256(out.app.data(), out.app.size());
  out.head_sha256 = sha256(flash, kHeadEnd);
  bool all_ff = true, all_00 = true;
  for (uint8_t b : out.app) {
    all_ff &= b == 0xFF;
    all_00 &= b == 0x00;
  }
  if (all_ff || all_00) return PackageError::BlankApp;
  return PackageError::None;
}

}  // namespace fm1_loader
