// FM-1 .fwsc packages: the identity string spelled by marker bytes, the UFW
// container inside, and its flash.bin. A port of fm1_unbrick.py's parse_fwsc
// and ufw_flash (themselves ports of Felucca's firmware/src/ota.c).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "crypto.h"
#include "layout.h"

namespace fm1_loader {

enum class PackageError {
  None,
  TooShort,          // shorter than the marked blocks or the UFW header
  HeaderCrc,         // UFW header CRC-16 fails
  EntryCount,        // the header lists 0 or more than 11 entries
  EntryCrc,          // UFW entry-list CRC-16 fails
  NoFlashBin,        // no type-0 entry
  FlashBinPastEnd,   // flash.bin runs past the package
  FlashBinShort,     // flash.bin doesn't reach 0x93000
  BadIdentity,       // identity isn't FM-1_NNN
  BlankApp,          // the app area is all 0xFF or all 0x00
  HashMismatch,      // the file isn't the one recorded in the library
  BadId,             // library id isn't 1-64 of [A-Za-z0-9._-]
  StoreFailed,       // the library's storage refused the write
};
const char *package_error_text(PackageError e);

struct Package {
  std::string identity;      // e.g. "FM-1_714"
  Family family = Family::Felucca;
  Hash file_sha256{};
  uint32_t flash_offset = 0;  // flash.bin within the logical image
  uint32_t flash_len = 0;
  Hash app_sha256{};          // of flash addresses 0x4000-0x92FFF
  Hash head_sha256{};         // of 0x0-0x3FFF (informational: never written)
  std::vector<uint8_t> app;   // kAppLen bytes, flash address kAppStart first
};

// Parses and checks a .fwsc file. expected_sha256, when given, must match the
// whole file (the hash recorded when the package was added to the library).
PackageError parse_package(const uint8_t *data, size_t len, Package &out,
                           const Hash *expected_sha256 = nullptr);

// Family from an identity: FM-1_0NN is M-VAVE's stock line, anything else is
// Felucca-format third-party firmware (fm1_unbrick.py classify()).
Family family_of(const std::string &identity);
bool valid_identity(const std::string &identity);

}  // namespace fm1_loader
