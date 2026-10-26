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
