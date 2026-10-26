#include "support/mock_rom.h"

#include <string.h>

#include "crypto.h"
#include "layout.h"

namespace fm1_loader {

namespace {

uint32_t be32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void resp(uint8_t *data, size_t len, uint16_t cmd, const uint8_t *payload = nullptr,
          size_t n = 0) {
  if (len < 2 + n) return;
  data[0] = uint8_t(cmd >> 8);
  data[1] = uint8_t(cmd);
  if (n) memcpy(data + 2, payload, n);
}

}  // namespace

MockRom::MockRom(const std::vector<uint8_t> &f, const std::vector<uint8_t> &loader_blob)
    : flash(f), blob_(loader_blob) {}

void MockRom::touched(uint32_t addr) {
  if (lowest_touched < 0 || long(addr) < lowest_touched) lowest_touched = addr;
}

bool MockRom::execute(const uint8_t *cdb, size_t cdb_len, Dir dir, uint8_t *data, size_t len) {
  std::string line = hex(cdb, cdb_len);
  if (dir == Dir::Out)
    line += " O" + std::to_string(len) + " " + hex(sha256(data, len)).substr(0, 16);
  else if (dir == Dir::In)
    line += " I" + std::to_string(len) + " -";
  else
    line += " N0 -";
  ++commands;
  bool ok = !(disconnect_after >= 0 && commands > disconnect_after) &&
            handle(cdb, cdb_len, dir, data, len);
  log.push_back(ok ? line : line + " !");
  return ok;
}

bool MockRom::handle(const uint8_t *cdb, size_t cdb_len, Dir dir, uint8_t *data, size_t len) {
  if (was_reset) return false;  // the FM-1 left ROM boot
  if (cdb[0] == 0x12) {
    if (cdb_len != 6 || dir != Dir::In || len != 36) {
      violations.push_back("inquiry shape");
      return false;
    }
    memset(data, 0, len);
    memcpy(data + 8, "WL82    ", 8);
    std::string product = loader_running ? loader_product : "UBOOT1.00";
    product.resize(16, ' ');
    memcpy(data + 16, product.data(), 16);
    memcpy(data + 32, "1.00", 4);
    return true;
  }
  if (cdb_len != 16) {
    violations.push_back("cdb length");
    return false;
  }
  uint16_t cmd = uint16_t(cdb[0] << 8 | cdb[1]);
  uint32_t addr = be32(cdb + 2);
  uint16_t ln = uint16_t(cdb[6] << 8 | cdb[7]);
  uint16_t crc = uint16_t(cdb[9] | cdb[10] << 8);
  bool in16 = dir == Dir::In && len == 16;

  if (!loader_running) {
    if (cmd == kWriteMemory && dir == Dir::Out) {
      if (len != ln || crc16(data, len) != crc) {
        violations.push_back("mem write length/crc");
        return false;
      }
      ram_[addr].assign(data, data + len);
      return true;
    }
    if (cmd == kJump && in16) {
      std::vector<uint8_t> image;
      for (const auto &kv : ram_) image.insert(image.end(), kv.second.begin(), kv.second.end());
      if (ram_.empty() || ram_.begin()->first != addr || addr != kLoaderAddr || image != blob_) {
        violations.push_back("loader upload differs from the blob");
        return false;
      }
      jump_arg = ln;
      loader_running = true;
      resp(data, len, cmd);
      return true;
    }
    return false;  // the ROM refuses anything else (retained-RAM reads included)
  }

  switch (cmd) {
    case kReadKey: {
      if (!in16) break;
      uint8_t k[2] = {uint8_t(key), uint8_t(key >> 8)};
      mengli(k, 2);
      uint8_t payload[6] = {0, 0, 0, 0, k[1], k[0]};
      resp(data, len, cmd, payload, 6);
      return true;
    }
    case kOnlineDevice: {
      if (!in16) break;
      uint8_t payload[6] = {3, 0, uint8_t(id), uint8_t(id >> 8), uint8_t(id >> 16),
                            uint8_t(id >> 24)};
      resp(data, len, cmd, payload, 6);
      return true;
    }
    case kUsbBufferSize: {
      if (!in16) break;
      uint8_t payload[4] = {uint8_t(io_max >> 24), uint8_t(io_max >> 16), uint8_t(io_max >> 8),
                            uint8_t(io_max)};
      resp(data, len, cmd, payload, 4);
      return true;
    }
    case kReadFlash: {
      if (dir != Dir::In || len != ln || addr + uint64_t(len) > flash.size()) break;
      ++reads;
      memcpy(data, flash.data() + addr, len);
      if (reads == flaky_read && len) data[len / 2] ^= 0x5A;
      return true;
    }
    case kWriteFlash: {
      if (dir != Dir::Out || len != ln || crc16(data, len) != crc || len > io_max ||
          addr + uint64_t(len) > flash.size()) {
        violations.push_back("flash write shape/crc");
        return false;
      }
      ++writes;
      touched(addr);
      for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (i == 0 && writes == flaky_write) b ^= 0x01;
        if (long(addr + i) == bad_cell) b ^= 0x80;
        flash[addr + i] &= b;
      }
      return true;
    }
    case kEraseSector: {
      if (!in16) break;
      if (addr % kSector || addr + uint64_t(kSector) > flash.size()) {
        violations.push_back("unaligned sector erase");
        return false;
      }
      touched(addr);
      memset(flash.data() + addr, 0xFF, kSector);
      resp(data, len, cmd);
      return true;
    }
    case kRunApp: {
      if (!in16) break;
      was_reset = true;
      resp(data, len, cmd);
      return true;
    }
    default:
      if ((cmd & 0xFF00) == 0xFB00) violations.push_back("unexpected erase/write command");
      break;
  }
  return false;
}

}  // namespace fm1_loader
