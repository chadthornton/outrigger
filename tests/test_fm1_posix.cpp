// Host-side tests for PosixStorage, the file-system back end the Waveshare
// uses on LittleFS. Here it runs on a temp directory.
#include <stdio.h>
#include <sys/stat.h>

#include "library.h"
#include "support/check.h"
#include "support/temp_storage.h"

using namespace fm1_loader;

int failures = 0;

static bool put(Storage &s, const std::string &k, const std::string &v) {
  return s.write(k, reinterpret_cast<const uint8_t *>(v.data()), v.size());
}

static std::string get(Storage &s, const std::string &k) {
  std::vector<uint8_t> out;
  return s.read(k, out) ? std::string(out.begin(), out.end()) : std::string("<none>");
}

static void write_raw(const std::string &path, const std::string &v) {
  FILE *f = fopen(path.c_str(), "wb");
  fwrite(v.data(), 1, v.size(), f);
  fclose(f);
}

static void test_basics() {
  TempPosixStorage st;
  CHECK(put(st, "m/unit/abc", "one"));
  CHECK_EQ(get(st, "m/unit/abc"), std::string("one"));
  CHECK(put(st, "m/unit/abc", "two"));  // replaced in place
  CHECK_EQ(get(st, "m/unit/abc"), std::string("two"));
  CHECK(put(st, "m/pkg/skiff-1.4", "p"));
  CHECK(put(st, "empty", ""));
  CHECK_EQ(get(st, "empty"), std::string(""));
  CHECK(st.exists("m/unit/abc") && !st.exists("m/unit") && !st.exists("m/unit/x"));
  std::vector<std::string> all = st.list("m/");
  CHECK_EQ(all.size(), size_t(2));
  CHECK(all.size() == 2 && all[0] == "m/pkg/skiff-1.4" && all[1] == "m/unit/abc");
  CHECK_EQ(st.list("m/unit/").size(), size_t(1));
  CHECK_EQ(st.list("m/u").size(), size_t(1));
  CHECK_EQ(st.list("").size(), size_t(3));
  CHECK(st.remove("m/unit/abc") && !st.exists("m/unit/abc"));
  CHECK(!st.remove("m/unit/abc"));
  CHECK(st.sync());
}

static void test_refused_keys() {
  TempPosixStorage st;
  CHECK(!put(st, "../escape", "x"));
  CHECK(!put(st, "a//b", "x"));
  CHECK(!put(st, "/abs", "x"));
  CHECK(!put(st, "a/", "x"));
  CHECK(!put(st, "~tmp", "x"));
  CHECK(!put(st, "a b", "x"));
  CHECK(!put(st, std::string(64, 'a'), "x"));  // over LittleFS's component limit
  CHECK(put(st, std::string(63, 'a'), "x"));
  CHECK(put(st, "s/ab/" + std::string(62, 'c'), "x"));  // a sector's name fits
}

static void test_temp_files_invisible() {
  TempPosixStorage st;
  CHECK(put(st, "m/unit/abc", "good"));
  // A write torn before its rename leaves only the temp file behind.
  write_raw(st.dir + "/m/unit/~abc", "half");
  CHECK_EQ(get(st, "m/unit/abc"), std::string("good"));
  CHECK_EQ(st.list("m/").size(), size_t(1));
  // The next write of that key replaces the stale temp and succeeds.
  CHECK(put(st, "m/unit/abc", "better"));
  CHECK_EQ(get(st, "m/unit/abc"), std::string("better"));
  struct stat sb;
  CHECK(stat((st.dir + "/m/unit/~abc").c_str(), &sb) != 0);
}

static void test_sector_store_on_files() {
  TempPosixStorage st;
  SectorStore s(st);
  std::vector<uint8_t> a(kSector, 0x11), out(kSector);
  Hash h;
  CHECK(s.put(a.data(), h));
  CHECK(st.exists("s/" + hex(h).substr(0, 2) + "/" + hex(h).substr(2)));
  CHECK(s.get(h, out.data()) && out == a);
  CHECK_EQ(s.count(), size_t(1));
  // Something that isn't a sector name under s/ is garbage too.
  CHECK(put(st, "s/zz/nope", "x"));
  CHECK_EQ(s.collect_garbage({h}), size_t(1));
  CHECK_EQ(s.count(), size_t(1));
}

int main() {
  test_basics();
  test_refused_keys();
  test_temp_files_invisible();
  test_sector_store_on_files();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all fm1_loader PosixStorage tests passed\n");
  return 0;
}
