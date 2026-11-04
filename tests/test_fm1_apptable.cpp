// Host-side tests for app_table: reading an FM-1 image's own file table at
// 0x4000 and telling the V15 layout (stock, Felucca, Skiff, SLOOP) from M-VAVE
// Open's. Packages come from tests/fixtures (tools/fetch_fixtures.py).
#include <string.h>

#include "app_table.h"
#include "crypto.h"
#include "package.h"
#include "softkey.h"
#include "support/check.h"

using namespace fm1_loader;

int failures = 0;

static std::vector<uint8_t> flash_of(const std::string &fixture, std::string *identity = nullptr) {
  const std::vector<uint8_t> file = read_file("tests/fixtures/" + fixture);
  std::string id;
  std::vector<uint8_t> flash;
  CHECK(unpack_flash_bin(file.data(), file.size(), id, flash) == PackageError::None);
  if (identity) *identity = id;
  return flash;
}

static void test_v15_layouts() {
  for (const char *f : {"FM-1_015.fwsc", "felucca-1.8.0.fwsc", "skiff-2.1.1.fwsc", "sloop-2.8.1.fwsc"}) {
    const std::vector<uint8_t> flash = flash_of(f);
    AppTable t;
    CHECK(read_app_table(flash.data(), flash.size(), t));
    CHECK_EQ(t.vm.start, 0x93000u);
    CHECK_EQ(t.vm.end, 0xE9000u);
    CHECK_EQ(t.btif.start, 0xE9000u);
    CHECK(layout_of(t) == Layout::V15);
    CHECK_EQ(t.image_end, kAppEnd);
  }
}

static void test_open_layout() {
  std::string id;
  const std::vector<uint8_t> flash = flash_of("open-v14.fwsc", &id);
  CHECK_EQ(id, std::string("FM-1_8000014"));
  CHECK_EQ(flash.size(), size_t(0x73000));
  AppTable t;
  CHECK(read_app_table(flash.data(), flash.size(), t));
  CHECK_EQ(t.vm.start, 0x73000u);
  CHECK_EQ(t.vm.end, kOpenBtif);
  CHECK_EQ(t.btif.start, kOpenBtif);
  CHECK_EQ(t.btif.end, kOpenUserStart);
  CHECK(layout_of(t) == Layout::Open);
  CHECK_EQ(t.image_end, 0x73000u);

  // Open's own data, from its app/instrument.h and app/project.c, packed with no gaps.
  const OpenMap m = open_map(t);
  CHECK_EQ(m.vm.start, 0x73000u);
  CHECK_EQ(m.projects.start, 0x7C000u);
  CHECK_EQ(m.projects.end, 0x8B000u);
  CHECK_EQ(m.settings.start, 0x8B000u);
  CHECK_EQ(m.journal.start, 0x8C000u);
  CHECK_EQ(m.samples.start, 0x8D000u);
  CHECK_EQ(m.samples.end, 0xFA000u);
  CHECK_EQ(m.library.start, 0xFA000u);
  CHECK_EQ(m.tuning.start, 0xFE000u);
  CHECK_EQ(m.tuning.end, kKeyMacStart);
  const Range order[] = {m.vm, m.btif, m.projects, m.settings, m.journal, m.samples, m.library, m.tuning};
  for (size_t i = 1; i < sizeof order / sizeof order[0]; ++i) CHECK_EQ(order[i - 1].end, order[i].start);
}

// The table works the same read from a whole-flash backup: anything past the
// image (Open's data) doesn't change the answer.
static void test_reads_whole_flash() {
  std::vector<uint8_t> flash = flash_of("open-v14.fwsc");
  flash.resize(kFlashSize, 0x5A);
  AppTable t;
  CHECK(read_app_table(flash.data(), flash.size(), t));
  CHECK(layout_of(t) == Layout::Open);
}

static void test_refuses_damage() {
  AppTable t;
  std::vector<uint8_t> flash = flash_of("felucca-1.8.0.fwsc");
  CHECK(!read_app_table(flash.data(), 0x4000, t));  // stops before the table
  std::vector<uint8_t> blank(kFlashSize, 0xFF);
  CHECK(!read_app_table(blank.data(), blank.size(), t));
  // One flipped bit in the VM record breaks its CRC: no layout from a guess.
  flash[0x4000 + 3 * 32 + 6] ^= 0x01;
  CHECK(!read_app_table(flash.data(), flash.size(), t));
}

// An app table that is valid but fits neither layout is Unknown, never a guess.
static void test_unknown_layout() {
  AppTable t;
  t.vm = {0x80000, 0x88000};
  t.btif = {0x88000, 0x89000};
  t.image_end = 0x80000;
  CHECK(layout_of(t) == Layout::Unknown);
  t = AppTable();
  CHECK(layout_of(t) == Layout::Unknown);
}

// Open answers M-VAVE's identity command (the query Outrigger sends) with 27
// bytes of ASCII, zero padded: WebUpdate.js Updater.open().
static void test_open_identity_reply() {
  std::vector<uint8_t> raw = {0x00, 0x59, 0x11, 27, 0, 0};
  const char *text = "FM-1_8000014";
  uint8_t sum = 0;
  for (size_t i = 0; i < 27; ++i) {
    uint8_t c = i < strlen(text) ? uint8_t(text[i]) : 0;
    raw.push_back(c);
    sum = uint8_t(sum + c);
  }
  raw.push_back(uint8_t(~sum));
  std::vector<uint8_t> sysex = {0xF0};
  uint32_t acc = 0;
  int bits = 0;
  for (uint8_t b : raw) {
    acc |= uint32_t(b) << bits;
    bits += 8;
    while (bits >= 7) {
      sysex.push_back(uint8_t(acc & 0x7F));
      acc >>= 7;
      bits -= 7;
    }
  }
  if (bits) sysex.push_back(uint8_t(acc & 0x7F));
  sysex.push_back(0xF7);
  CHECK_EQ(parse_identity(sysex), std::string("FM-1_8000014"));
  CHECK(valid_identity("FM-1_8000014"));
}

int main() {
  test_v15_layouts();
  test_open_layout();
  test_reads_whole_flash();
  test_refuses_damage();
  test_unknown_layout();
  test_open_identity_reply();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all fm1_loader app table tests passed\n");
  return 0;
}
