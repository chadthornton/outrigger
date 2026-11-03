#include "app_scan.h"

#include <string.h>

#include "crypto.h"
#include "layout.h"

namespace fm1_loader {

namespace {
constexpr size_t kBlock = 32;
constexpr char kTable[] = "app_area_head";  // the file table's first entry, its name at +16
constexpr char kFelucca[] = "FELUCCA";
constexpr char kOta[] = "ota-";
constexpr size_t kIdLen = 8;                 // "FM-1_" and three digits

bool at(const uint8_t *p, const char *s) { return memcmp(p, s, strlen(s)) == 0; }

uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

bool identity_at(const uint8_t *p) {
  if (!at(p, "FM-1_")) return false;
  for (size_t i = 5; i < kIdLen; ++i)
    if (p[i] < '0' || p[i] > '9') return false;
  return true;
}
}  // namespace

void decrypt_app(uint8_t *buf, size_t len, uint32_t offset) {
  for (size_t i = 0; i < len; i += kBlock) {
    const size_t n = len - i < kBlock ? len - i : kBlock;
    jl_enc(buf + i, n, uint16_t(kChipKey ^ ((offset + i) >> 2)));
  }
}

void StockScan::feed(const uint8_t *data, size_t len) {
  for (size_t i = 0; i + kBlock <= len; i += kBlock) {
    uint8_t w[sizeof tail_ + kBlock];
    memcpy(w, tail_, tail_len_);
    memcpy(w + tail_len_, data + i, kBlock);
    decrypt_app(w + tail_len_, kBlock, offset_);
    if (offset_ == 0 && at(w + 16, kTable)) {
      // The first entry spans every file: offset (in the area, high byte a flag)
      // and length. What lies past it is build data that names nothing.
      const uint32_t off = u32(w + 8) & 0x00FFFFFF, flen = u32(w + 12);
      table_ = true;
      end_ = off + flen < kAppLen ? off + flen : kAppLen;
    }
    const size_t n = tail_len_ + kBlock;
    scan(w, n);
    offset_ += kBlock;
    memcpy(tail_, w + n - sizeof tail_, sizeof tail_);
    tail_len_ = sizeof tail_;
  }
}

// Looks at every match that ends in this block (the tail's bytes were scanned
// with the block before, but stay as context for "ota-") and inside the files.
void StockScan::scan(const uint8_t *p, size_t len) {
  const uint32_t start = offset_ - uint32_t(tail_len_);  // where p is in the area
  auto counts = [&](size_t i, size_t n) { return i + n <= len && i + n > tail_len_ && start + i + n <= end_; };
  for (size_t i = 0; i < len; ++i) {
    if (counts(i, sizeof kFelucca - 1) && at(p + i, kFelucca)) felucca_ = true;
    if (!counts(i, kIdLen) || !identity_at(p + i)) continue;
    if (i >= 4 && at(p + i - 4, kOta)) {
      felucca_ = true;  // Felucca's update record names the stock build it replaced
    } else if (!id_[0]) {
      memcpy(id_, p + i, kIdLen);
    } else if (memcmp(id_, p + i, kIdLen) != 0) {
      several_ = true;
    }
  }
}

std::string StockScan::identity() const {
  // Stock is M-VAVE's numbering, FM-1_0NN (library.cpp's family_of_project).
  if (!table_ || felucca_ || several_ || id_[5] != '0') return "";
  return std::string(id_, kIdLen);
}

}  // namespace fm1_loader
