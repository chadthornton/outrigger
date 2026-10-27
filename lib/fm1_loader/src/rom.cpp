#include "rom.h"

#include <string.h>

#include "crypto.h"

namespace fm1_loader {

namespace {

constexpr size_t kCdbLen = 16;

// Command code big-endian, then the arguments, padded with 0xFF to 16 bytes.
void make_cdb(uint16_t cmd, const uint8_t *args, size_t args_len, uint8_t cdb[kCdbLen]) {
  memset(cdb, 0xFF, kCdbLen);
  cdb[0] = uint8_t(cmd >> 8);
  cdb[1] = uint8_t(cmd);
  memcpy(cdb + 2, args, args_len);
}

void be32(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

void be16(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

std::string ascii(const uint8_t *p, size_t n) {
  std::string s(reinterpret_cast<const char *>(p), n);
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

}  // namespace

bool loader_needs_host_scramble(bool rom_mengli_quirk, bool loader_is_mengli) {
  return (!rom_mengli_quirk && loader_is_mengli) || (rom_mengli_quirk && !loader_is_mengli);
}

bool RomClient::inquiry(std::string &vendor, std::string &product, std::string &revision) {
  const uint8_t cdb[6] = {0x12, 0x00, 0x00, 0x00, 36, 0x00};
  uint8_t data[36] = {0};
  if (!t_.execute(cdb, sizeof cdb, ScsiTransport::Dir::In, data, sizeof data)) return false;
  vendor = ascii(data + 8, 8);
  product = ascii(data + 16, 16);
  revision = ascii(data + 32, 4);
  return true;
}

bool RomClient::exec(uint16_t cmd, const uint8_t *args, size_t args_len,
                     uint8_t resp[kResponseLen]) {
  uint8_t cdb[kCdbLen];
  make_cdb(cmd, args, args_len, cdb);
  memset(resp, 0, kResponseLen);
  if (!t_.execute(cdb, kCdbLen, ScsiTransport::Dir::In, resp, kResponseLen)) return false;
  return (uint16_t(resp[0] << 8) | resp[1]) == cmd;
}

// WRITE_MEMORY and WRITE_FLASH share a layout: address, length, 0x00, CRC-16 (little-endian).
static void data_args(uint32_t addr, const uint8_t *data, uint16_t len, uint8_t args[9]) {
  be32(args, addr);
  be16(args + 4, len);
  uint16_t crc = crc16(data, len);
  args[6] = 0;
  args[7] = uint8_t(crc);
  args[8] = uint8_t(crc >> 8);
}

bool RomClient::mem_write(uint32_t addr, const uint8_t *data, uint16_t len) {
  uint8_t args[9], cdb[kCdbLen];
  data_args(addr, data, len, args);
  make_cdb(kWriteMemory, args, sizeof args, cdb);
  return t_.execute(cdb, kCdbLen, ScsiTransport::Dir::Out, const_cast<uint8_t *>(data), len);
}

bool RomClient::mem_jump(uint32_t addr, uint16_t arg) {
  uint8_t args[6], resp[kResponseLen];
  be32(args, addr);
  be16(args + 4, arg);
  return exec(kJump, args, sizeof args, resp);
}

bool RomClient::usb_buffer_size(uint32_t &out) {
  uint8_t resp[kResponseLen];
  if (!exec(kUsbBufferSize, nullptr, 0, resp)) return false;
  out = uint32_t(resp[2]) << 24 | uint32_t(resp[3]) << 16 | uint32_t(resp[4]) << 8 | resp[5];
  return true;
}

bool RomClient::chip_key(uint16_t &out) {
  uint8_t args[4], resp[kResponseLen];
  be32(args, kChipKeyArg);
  if (!exec(kReadKey, args, sizeof args, resp)) return false;
  // The key is response bytes 4-5 after the echoed command, reversed, then MengLi.
  uint8_t k[2] = {resp[7], resp[6]};
  mengli(k, 2);
  out = uint16_t(k[0] | k[1] << 8);
  return true;
}

bool RomClient::online_device(uint8_t &type, uint32_t &id) {
  uint8_t resp[kResponseLen];
  if (!exec(kOnlineDevice, nullptr, 0, resp)) return false;
  type = resp[2];
  id = uint32_t(resp[4]) | uint32_t(resp[5]) << 8 | uint32_t(resp[6]) << 16 |
       uint32_t(resp[7]) << 24;
  return true;
}

bool RomClient::flash_read(uint32_t addr, uint8_t *out, uint16_t len) {
  uint8_t args[6], cdb[kCdbLen];
  be32(args, addr);
  be16(args + 4, len);
  make_cdb(kReadFlash, args, sizeof args, cdb);
  return t_.execute(cdb, kCdbLen, ScsiTransport::Dir::In, out, len);
}

bool RomClient::erase_sector(uint32_t addr) {
  if (!kWritesEnabled) return false;
  uint8_t args[4], resp[kResponseLen];
  be32(args, addr);
  return exec(kEraseSector, args, sizeof args, resp);
}

bool RomClient::flash_write(uint32_t addr, const uint8_t *data, uint16_t len) {
  if (!kWritesEnabled) return false;
  uint8_t args[9], cdb[kCdbLen];
  data_args(addr, data, len, args);
  make_cdb(kWriteFlash, args, sizeof args, cdb);
  return t_.execute(cdb, kCdbLen, ScsiTransport::Dir::Out, const_cast<uint8_t *>(data), len);
}

bool RomClient::run_app(uint32_t arg) {
  uint8_t args[4], resp[kResponseLen];
  be32(args, arg);
  return exec(kRunApp, args, sizeof args, resp);
}

}  // namespace fm1_loader
