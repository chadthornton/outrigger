// tools/fm1lib.py writes packages into a store from the Mac (make store-add);
// the Waveshare reads them with lib/fm1_loader. This reads a directory the
// Python wrote (make test runs `fm1lib.py selftest` first) and checks every
// package against what the C++ makes of the same file.
#include "library.h"
#include "posix_storage.h"
#include "support/check.h"
#include "support/temp_storage.h"

using namespace fm1_loader;

int failures = 0;

int main(int argc, char **argv) {
  if (argc != 2) {
    printf("usage: test_fm1_interop <dir fm1lib.py wrote>\n");
    return 2;
  }
  PosixStorage from_python(argv[1]);
  Library py(from_python);
  TempPosixStorage native_storage;
  Library native(native_storage);

  const char *const files[] = {"skiff-1.4", "sloop-2.8.1", "FM-1_015"};
  for (const char *id : files) {
    std::vector<uint8_t> data = read_file(std::string("tests/fixtures/") + id + ".fwsc");
    CHECK(native.add_package(id, data.data(), data.size()) == PackageError::None);
    PackageInfo a, b;
    CHECK(py.package(id, a));
    CHECK(native.package(id, b));
    CHECK_EQ(a.identity, b.identity);
    CHECK(a.family == b.family);
    CHECK(a.file_sha256 == b.file_sha256 && a.app_sha256 == b.app_sha256);
    CHECK(a.app == b.app);
    CHECK_EQ(a.version, std::string("test"));
    // Every sector the Python stored reads back through the C++ store.
    std::vector<uint8_t> buf(kSector);
    for (const Hash &h : a.app) CHECK(py.sectors().get(h, buf.data()));
  }
  // The sector files are the same set, byte for byte.
  CHECK(from_python.list("s/") == native_storage.list("s/"));
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all fm1_loader interop tests passed (fm1lib.py <-> C++)\n");
  return 0;
}
