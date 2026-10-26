// Tiny test helpers shared by the fm1_loader tests.
#pragma once

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

extern int failures;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

#define CHECK_EQ(got, want)                                             \
  do {                                                                  \
    auto g_ = (got);                                                    \
    auto w_ = (want);                                                   \
    if (!(g_ == w_)) {                                                  \
      printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #got, #want); \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

inline std::vector<uint8_t> read_file(const std::string &path) {
  std::vector<uint8_t> out;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) {
    printf("missing %s: run `make fixtures`\n", path.c_str());
    exit(2);
  }
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return out;
}

inline std::string read_text(const std::string &path) {
  std::vector<uint8_t> b = read_file(path);
  return std::string(b.begin(), b.end());
}

inline std::vector<std::string> lines_of(const std::string &text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) nl = text.size();
    out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

// Deterministic bytes, as tools/gen_golden.py synth() and the Unbricker's
// synthetic_flash() make them: SHA-256(seed + 4-byte little-endian counter)...
#include "crypto.h"
inline std::vector<uint8_t> synth(const std::string &seed, size_t n) {
  std::vector<uint8_t> out;
  for (uint32_t counter = 0; out.size() < n; ++counter) {
    std::string in = seed;
    for (int i = 0; i < 4; ++i) in += char(counter >> (8 * i));
    fm1_loader::Hash h =
        fm1_loader::sha256(reinterpret_cast<const uint8_t *>(in.data()), in.size());
    out.insert(out.end(), h.begin(), h.end());
  }
  out.resize(n);
  return out;
}
