// FM-1 .fwsc packages: the identity string spelled by marker bytes, the UFW
// container inside, and its flash.bin. A port of fm1_unbrick.py's parse_fwsc
// and ufw_flash (themselves ports of Felucca's firmware/src/ota.c).
// Ported from MvaveFM1Unbricker (MIT, Michael P Jones); see NOTICE for the ota.c lineage.
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
  BadIdentity,       // identity isn't FM-1_ and 3-15 digits
  BlankApp,          // the app area is all 0xFF or all 0x00
  HashMismatch,      // the file isn't the one recorded in the library
  BadId,             // library id isn't 1-64 of [A-Za-z0-9._-]
  StoreFailed,       // the library's storage refused the write
};
const char *package_error_text(PackageError e);

struct Package {
  std::string identity;      // e.g. "FM-1_714"
  Hash file_sha256{};
  uint32_t flash_offset = 0;  // flash.bin within the logical image
  uint32_t flash_len = 0;
  Hash app_sha256{};          // of flash addresses 0x4000-0x92FFF
  Hash head_sha256{};         // of 0x0-0x3FFF (informational: never written)
  std::vector<uint8_t> app;   // kAppLen bytes, flash address kAppStart first
};

// Parses and checks a .fwsc file. expected_sha256, when given, must match the
// whole file (the hash recorded when the package was added to the library).
// head (optional) gets the package's boot head, 0x0-0x3FFF: the tests take
// stock V15's from its package.
PackageError parse_package(const uint8_t *data, size_t len, Package &out,
                           const Hash *expected_sha256 = nullptr, std::vector<uint8_t> *head = nullptr);

// FM-1_ and 3 to 15 digits. The family comes from the catalog project
// (family_of_project, library.h), never from the identity's digits.
bool valid_identity(const std::string &identity);

}  // namespace fm1_loader
