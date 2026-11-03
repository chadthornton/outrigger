// Host-side test for lib/board/src/board_blob: the board profile as the page
// sends it and the board stores it. Runs every case in tests/boards/blobs.json,
// which tools/boards.py generates from firmware/boards.json and checks too
// (tests/test_boards.py), so the board's checker and boards.py can't drift.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "board_blob.h"

static int failures = 0;

static std::vector<uint8_t> from_hex(const std::string &h) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
  return out;
}

// The file is the generator's: one case per line, "name<TAB>hex<TAB>code".
int main(int argc, char **argv) {
  std::ifstream in(argc > 1 ? argv[1] : "tests/boards/blobs.tsv");
  if (!in) {
    printf("board_blob: no cases file\n");
    return 1;
  }
  std::string line;
  int cases = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream f(line);
    std::string name, hex, code;
    std::getline(f, name, '\t');
    std::getline(f, hex, '\t');
    std::getline(f, code, '\t');
    const std::vector<uint8_t> bytes = from_hex(hex);
    BoardBlob b;
    char why[96] = "";
    BlobError e = board_blob_decode(bytes.data(), bytes.size(), b);
    if (e == BlobError::Ok) e = board_blob_check(b, why, sizeof why);
    ++cases;
    if (int(e) != std::stoi(code)) {
      printf("board_blob %s: error %d (%s), wanted %s\n", name.c_str(), int(e), why, code.c_str());
      ++failures;
      continue;
    }
    if (e == BlobError::Ok) {
      // A good profile encodes back to the same bytes.
      uint8_t again[kBlobSize];
      const size_t n = board_blob_encode(b, again, sizeof again);
      if (n != bytes.size() || memcmp(again, bytes.data(), n) != 0) {
        printf("board_blob %s: doesn't encode back to the same bytes\n", name.c_str());
        ++failures;
      }
    }
  }
  if (failures) return 1;
  printf("board_blob: %d cases decode and check as boards.py says\n", cases);
  return 0;
}
