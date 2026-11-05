// Hashes, CRCs and ciphers the FM-1 loader needs: SHA-256 for the content-addressed
// store and package checks, CRC-16/XMODEM and JieLi's two scramblers for the UFW
// package headers and the ROM-boot protocol. Plain C++, no Arduino headers.
// Ported in part from jl-uboot-tool (MIT, Andrey Grigoryev) and the Unbricker (MIT, Michael P Jones): see NOTICE.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <string>

namespace fm1_loader {

using Hash = std::array<uint8_t, 32>;

class Sha256 {
 public:
  Sha256();
  void update(const uint8_t *data, size_t len);
  Hash finish();

 private:
  void block(const uint8_t *p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t buf_len_ = 0;
  uint64_t total_ = 0;
};

Hash sha256(const uint8_t *data, size_t len);
std::string hex(const uint8_t *data, size_t len);
inline std::string hex(const Hash &h) { return hex(h.data(), h.size()); }
// Parses 64 hex digits; false on anything else.
bool parse_hash(const std::string &text, Hash &out);

// CRC-16/XMODEM (poly 0x1021, init 0): jl-uboot-tool's jl_crc16 and Felucca's ota_crc16.
uint16_t crc16(const uint8_t *data, size_t len, uint16_t crc = 0);

// CRC-32 (zlib's: reflected poly 0xEDB88320, init and final XOR 0xFFFFFFFF): the
// boot-head checksum FM-1 owners compare (layout.h kHeadCrcV15).
uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc = 0);

// JieLi "ENC" cipher, key 0xFFFF: the UFW header and entry scramble (its own inverse).
void jl_enc(uint8_t *buf, size_t len, uint16_t key = 0xFFFF);

// JieLi "CrcDecode" (MengLi) cipher, jl-uboot-tool's jl_crc_cipher (its own inverse).
void mengli(uint8_t *buf, size_t len, uint32_t key = 0xFFFFFFFF);

}  // namespace fm1_loader
