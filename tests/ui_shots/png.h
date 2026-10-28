// Writes an 8-bit RGB PNG, deflated with zlib.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <zlib.h>

#include <vector>

namespace png {

inline void put32(std::vector<uint8_t> &v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

inline void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> c;
  put32(c, uint32_t(data.size()));
  c.insert(c.end(), type, type + 4);
  c.insert(c.end(), data.begin(), data.end());
  put32(c, uint32_t(crc32(0, c.data() + 4, uInt(c.size() - 4))));
  fwrite(c.data(), 1, c.size(), f);
}

// rgb: width * height * 3 bytes, row by row.
inline bool write(const char *path, int width, int height, const uint8_t *rgb) {
  // Each row starts with filter type 0 (none).
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (width * 3 + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + size_t(y) * width * 3, rgb + size_t(y + 1) * width * 3);
  }
  uLongf zn = compressBound(uLong(raw.size()));
  std::vector<uint8_t> z(zn);
  if (compress2(z.data(), &zn, raw.data(), uLong(raw.size()), 9) != Z_OK) return false;
  z.resize(zn);

  FILE *f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  fwrite(kSig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  put32(ihdr, uint32_t(width));
  put32(ihdr, uint32_t(height));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, no interlace
  chunk(f, "IHDR", ihdr);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  return fclose(f) == 0;
}

}  // namespace png
