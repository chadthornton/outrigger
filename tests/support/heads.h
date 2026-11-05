// Boot heads for the fm1_loader tests: stock V15's, taken from the stock package
// fixture (the board installs only on it), and synthetic ones forged to a CRC-32,
// for V16, whose bytes we don't have.
#pragma once

#include <string.h>

#include <vector>

#include "check.h"
#include "crypto.h"
#include "layout.h"
#include "package.h"

inline const std::vector<uint8_t> &v15_head() {
  static const std::vector<uint8_t> head = [] {
    std::vector<uint8_t> file = read_file("tests/fixtures/FM-1_015.fwsc"), out;
    fm1_loader::Package p;
    if (fm1_loader::parse_package(file.data(), file.size(), p, nullptr, &out) != fm1_loader::PackageError::None)
      out.clear();
    return out;
  }();
  return head;
}

// kHeadEnd bytes from seed, the last 4 chosen so the CRC-32 is crc: CRC-32 is
// affine over GF(2), so the effect of each of those 32 bits is one column of a
// system solved by elimination.
inline std::vector<uint8_t> forged_head(uint32_t crc, uint8_t seed = 1) {
  std::vector<uint8_t> h(fm1_loader::kHeadEnd);
  for (size_t i = 0; i < h.size(); ++i) h[i] = uint8_t(seed + i * 7 + (i >> 8));
  uint8_t *tail = h.data() + h.size() - 4;
  memset(tail, 0, 4);
  const uint32_t base = fm1_loader::crc32(h.data(), h.size());
  uint32_t col[32];
  for (int b = 0; b < 32; ++b) {
    tail[b / 8] ^= uint8_t(1 << (b % 8));
    col[b] = fm1_loader::crc32(h.data(), h.size()) ^ base;
    tail[b / 8] ^= uint8_t(1 << (b % 8));
  }
  // Rows: output bits; columns: input bits, augmented with the wanted change.
  uint64_t row[32];
  const uint32_t want = crc ^ base;
  for (int r = 0; r < 32; ++r) {
    row[r] = uint64_t((want >> r) & 1) << 32;
    for (int b = 0; b < 32; ++b) row[r] |= uint64_t((col[b] >> r) & 1) << b;
  }
  for (int c = 0, r = 0; c < 32; ++c) {
    int p = r;
    while (p < 32 && !((row[p] >> c) & 1)) ++p;
    if (p == 32) continue;
    uint64_t t = row[p];
    row[p] = row[r];
    row[r] = t;
    for (int k = 0; k < 32; ++k)
      if (k != r && ((row[k] >> c) & 1)) row[k] ^= row[r];
    ++r;
  }
  for (int r = 0; r < 32; ++r)
    for (int c = 0; c < 32; ++c)
      if ((row[r] >> c) & 1) {
        if ((row[r] >> 32) & 1) tail[c / 8] ^= uint8_t(1 << (c % 8));
        break;
      }
  return h;
}
