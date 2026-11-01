// Host-side test for GET /store's body (firmware/waveshare_169/store_json.cpp):
// it lists what the store holds and is valid JSON (the Makefile parses the
// file this writes with Python).
#include <stdio.h>

#include "library.h"
#include "store_json.h"
#include "support/check.h"

using namespace fm1_loader;

int failures = 0;

int main(int argc, char **argv) {
  MemoryStorage storage;
  Library lib(storage);
  std::vector<uint8_t> data = read_file("tests/fixtures/skiff-1.4.fwsc");
  CHECK(lib.add_package("skiff-1.4", data.data(), data.size(), nullptr, "Skiff \"quoted\"", "1.4") ==
        PackageError::None);
  CHECK(lib.set_pinned("skiff-1.4", true));
  Unit u;
  u.id = "0123456789abcdef";
  u.installed_package = "skiff-1.4";
  CHECK(lib.put_unit(u));

  const std::string json = store_json(lib, storage, 12 << 20, 3 << 20);
  CHECK(json.find("\"id\":\"skiff-1.4\"") != std::string::npos);
  CHECK(json.find("Skiff \\\"quoted\\\"") != std::string::npos);
  CHECK(json.find("\"pinned\":true") != std::string::npos);
  CHECK(json.find("\"bytes\":585728") != std::string::npos);  // 143 app sectors
  CHECK(json.find("\"installed\":\"skiff-1.4\"") != std::string::npos);
  CHECK(json.find("\"fs_total\":12582912") != std::string::npos);
  if (argc > 1) {
    FILE *f = fopen(argv[1], "w");
    CHECK(f != nullptr);
    if (f) {
      fputs(json.c_str(), f);
      fclose(f);
    }
  }
  if (failures) {
    printf("store_json: %d failure(s)\n", failures);
    return 1;
  }
  printf("store_json: all passed\n");
  return 0;
}
