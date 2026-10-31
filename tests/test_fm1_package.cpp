// Host-side tests for fm1_loader's hashes, ciphers, package parser and address
// guard. Package vectors come from fm1_unbrick.py (tests/golden/packages.txt).
#include <string.h>

#include <map>

#include "crypto.h"
#include "layout.h"
#include "package.h"
#include "support/check.h"

using namespace fm1_loader;

int failures = 0;

static std::string sha_hex(const std::string &s) {
  return hex(sha256(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
}

// key -> value from "name value" lines of tests/golden/crypto.txt
static std::map<std::string, std::string> crypto_golden() {
  std::map<std::string, std::string> out;
  for (const std::string &line : lines_of(read_text("tests/golden/crypto.txt"))) {
    size_t b = line.rfind(' ');
    out[line.substr(0, b)] = line.substr(b + 1);
  }
  return out;
}

static void test_sha256() {
  CHECK_EQ(sha_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  // 56 bytes: the padding spills into a second block.
  CHECK_EQ(sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  Hash h;
  CHECK(parse_hash(sha_hex("abc"), h));
  CHECK_EQ(hex(h), sha_hex("abc"));
  CHECK(!parse_hash("xyz", h));
}

static void test_crc_and_ciphers() {
  auto g = crypto_golden();
  const uint8_t digits[] = "123456789";
  CHECK_EQ(hex(std::vector<uint8_t>{uint8_t(crc16(digits, 9) >> 8), uint8_t(crc16(digits, 9))}.data(), 2),
           g["crc16 123456789"]);
  std::vector<uint8_t> s = synth("crc", 512);
  uint16_t c = crc16(s.data(), s.size());
  uint8_t cb[2] = {uint8_t(c >> 8), uint8_t(c)};
  CHECK_EQ(hex(cb, 2), g["crc16 synth512"]);

  std::vector<uint8_t> z(32, 0);
  mengli(z.data(), z.size());
  CHECK_EQ(hex(z.data(), z.size()), g["mengli zero32"]);
  std::vector<uint8_t> m = synth("mengli", 64);
  mengli(m.data(), m.size());
  CHECK_EQ(hex(m.data(), m.size()), g["mengli synth64"]);
  mengli(m.data(), m.size());
  CHECK(m == synth("mengli", 64));  // its own inverse

  std::vector<uint8_t> e(64, 0);
  jl_enc(e.data(), e.size());
  CHECK_EQ(hex(e.data(), e.size()), g["jlenc zero64"]);

  // What the loader's READ_KEY answer carries for chip key 0x980F.
  uint8_t key[2] = {0x0F, 0x98};
  mengli(key, 2);
  uint8_t wire[2] = {key[1], key[0]};
  CHECK_EQ(hex(wire, 2), g["chipkey 980f"]);
}

struct GoldenPackage {
  std::string identity, kind, file, app, head;
  uint32_t fl_off, fl_len;
};

static std::map<std::string, GoldenPackage> packages_golden() {
  std::map<std::string, GoldenPackage> out;
  for (const std::string &line : lines_of(read_text("tests/golden/packages.txt"))) {
    GoldenPackage p;
    std::string name = line.substr(0, line.find(' '));
    auto field = [&](const char *key) {
      size_t at = line.find(std::string(" ") + key + "=");
      size_t start = at + strlen(key) + 2;
      return line.substr(start, line.find(' ', start) - start);
    };
    p.identity = field("identity");
    p.kind = field("kind");
    p.file = field("file");
    p.app = field("app");
    p.head = field("head");
    p.fl_off = uint32_t(strtoul(field("fl_off").c_str(), nullptr, 16));
    p.fl_len = uint32_t(strtoul(field("fl_len").c_str(), nullptr, 16));
    out[name] = p;
  }
  return out;
}

static void test_real_packages() {
  for (const auto &kv : packages_golden()) {
    const GoldenPackage &want = kv.second;
    std::vector<uint8_t> file = read_file("tests/fixtures/" + kv.first);
    Package p;
    Hash expected;
    CHECK(parse_hash(want.file, expected));
    PackageError err = parse_package(file.data(), file.size(), p, &expected);
    if (err != PackageError::None) printf("  %s: %s\n", kv.first.c_str(), package_error_text(err));
    CHECK(err == PackageError::None);
    CHECK_EQ(p.identity, want.identity);
    CHECK_EQ(hex(p.file_sha256), want.file);
    CHECK_EQ(p.flash_offset, want.fl_off);
    CHECK_EQ(p.flash_len, want.fl_len);
    CHECK_EQ(hex(p.app_sha256), want.app);
    CHECK_EQ(hex(p.head_sha256), want.head);
    CHECK_EQ(p.app.size(), size_t(kAppLen));
    CHECK(p.family == (want.kind == "official-v15" ? Family::Stock : Family::Felucca));
  }
}

static PackageError parse(const std::vector<uint8_t> &file, const Hash *expected = nullptr) {
  Package p;
  return parse_package(file.data(), file.size(), p, expected);
}

// Byte i of the logical image sits at file offset i + i / 0x2F for the marked
// blocks, and i + 20 after them.
static size_t file_offset(size_t logical) {
  return logical < 20 * 0x2F ? logical + logical / 0x2F : logical + 20;
}

static void test_damaged_packages() {
  const std::vector<uint8_t> good = read_file("tests/fixtures/sloop-2.8.1.fwsc");
  CHECK(parse(good) == PackageError::None);

  std::vector<uint8_t> f = good;
  f[file_offset(0)] ^= 0x01;  // the header CRC itself
  CHECK(parse(f) == PackageError::HeaderCrc);

  f = good;
  f[file_offset(0x30)] ^= 0x40;  // a header byte the CRC covers
  CHECK(parse(f) == PackageError::HeaderCrc);

  f = good;
  f[file_offset(0x40 + 5)] ^= 0x01;  // inside the first entry
  CHECK(parse(f) == PackageError::EntryCrc);

  f = good;
  f.resize(good.size() / 2);  // truncated: flash.bin runs past the end
  CHECK(parse(f) == PackageError::FlashBinPastEnd);

  f.assign(good.begin(), good.begin() + 500);
  CHECK(parse(f) == PackageError::TooShort);

  Hash other = sha256(good.data(), 10);
  CHECK(parse(good, &other) == PackageError::HashMismatch);

  f = good;
  f[file_offset(0) + 0x2F] = 0x7D;  // drop the identity's first character
  CHECK(parse(f) == PackageError::BadIdentity);
}

static void test_identities() {
  // FM-1_ and 3 to 15 digits: the package marks hold 20 characters. Catalog firmwares
  // use 3 to 10: Obervave FM-1_9530, x0x FM-1_9010100, mope1 FM-1_6673000300.
  CHECK(valid_identity("FM-1_015"));
  CHECK(valid_identity("FM-1_9530"));
  CHECK(valid_identity("FM-1_9010100"));
  CHECK(valid_identity("FM-1_6673000300"));
  CHECK(valid_identity("FM-1_123456789012345"));
  CHECK(!valid_identity("FM-1_15"));
  CHECK(!valid_identity("FM-1_1234567890123456"));
  CHECK(!valid_identity("FM-1_9a30"));
  CHECK(!valid_identity("FM-1_"));
  CHECK(!valid_identity("FM-2_015"));
  CHECK(family_of("FM-1_015") == Family::Stock);
  CHECK(family_of("FM-1_714") == Family::Felucca);
  CHECK(family_of("FM-1_900") == Family::Felucca);
}

static void test_guard_app() {
  const char *why = nullptr;
  CHECK(guard(kAppStart, kSector, Right::App, Family::Felucca));
  CHECK(guard(kAppEnd - kSector, kSector, Right::App, Family::Felucca));
  CHECK(guard(kAppStart, kAppLen, Right::App, Family::Felucca));
  CHECK(guard(0x50000, 512, Right::App, Family::Stock));
  // The boot head, including requests that only straddle its edge.
  CHECK(!guard(0, kSector, Right::App, Family::Felucca, &why));
  CHECK(why && strstr(why, "boot head"));
  CHECK(!guard(kHeadEnd - 1, 2, Right::App, Family::Felucca));
  CHECK(!guard(kHeadEnd - kSector, 2 * kSector, Right::App, Family::Felucca));
  CHECK(!guard(0x3000, kSector, Right::App, Family::Felucca));
  // Past the app area.
  CHECK(!guard(kAppEnd - kSector, 2 * kSector, Right::App, Family::Felucca));
  CHECK(!guard(kAppEnd, kSector, Right::App, Family::Felucca));
  // Bluetooth data and factory voices, and key_mac, under every right.
  for (Right r : {Right::App, Right::SaveRestore, Right::SettingsErase}) {
    CHECK(!guard(kUnitSector, kSector, r, Family::Felucca));
    CHECK(!guard(kUnitSector - 1, 2, r, Family::Felucca));
    CHECK(!guard(kFactoryEnd - 1, 2, r, Family::Felucca));
    CHECK(!guard(0xF0000, kSector, r, Family::Felucca));
    CHECK(!guard(kKeyMacStart, kSector, r, Family::Felucca));
    CHECK(!guard(0, kSector, r, Family::Felucca));
    CHECK(!guard(kSector, 0, r, Family::Felucca));
    CHECK(!guard(0xFFFFF000, 0x2000, r, Family::Felucca));  // wraps 32 bits
    CHECK(!guard(kFlashSize, kSector, r, Family::Felucca));
  }
}

static void test_guard_data() {
  // Settings erase: only the two settings areas.
  CHECK(guard(kVmStart, kSector, Right::SettingsErase, Family::Stock));
  CHECK(guard(kVmEnd - kSector, kSector, Right::SettingsErase, Family::Stock));
  CHECK(guard(kGlobalsStart, kGlobalsEnd - kGlobalsStart, Right::SettingsErase, Family::Stock));
  CHECK(!guard(kAppEnd - kSector, kSector, Right::SettingsErase, Family::Stock));
  CHECK(!guard(kVmEnd - kSector, 2 * kSector, Right::SettingsErase, Family::Stock));
  // Save restore: the Felucca table, never staging, never app, nothing for stock.
  CHECK(guard(0x97000, kSector, Right::SaveRestore, Family::Felucca));
  CHECK(guard(0xA0000, 0x3C000, Right::SaveRestore, Family::Felucca));
  CHECK(guard(0xDF000, kSector, Right::SaveRestore, Family::Felucca));
  CHECK(guard(0xE5000, kSector, Right::SaveRestore, Family::Felucca));
  CHECK(guard(0xE7000, kSector, Right::SaveRestore, Family::Felucca));
  CHECK(guard(0xFE000, kSector, Right::SaveRestore, Family::Felucca));
  const char *why = nullptr;
  CHECK(!guard(kStagingStart, kSector, Right::SaveRestore, Family::Felucca, &why));
  CHECK(why && strstr(why, "staging"));
  CHECK(!guard(0xE4000, kSector, Right::SaveRestore, Family::Felucca));
  CHECK(!guard(0xDF000, 2 * kSector, Right::SaveRestore, Family::Felucca));  // into staging
  CHECK(!guard(0x93000, kSector, Right::SaveRestore, Family::Felucca));      // not in the table
  CHECK(!guard(0x9F000, kSector, Right::SaveRestore, Family::Felucca));
  CHECK(!guard(0xE8000, kSector, Right::SaveRestore, Family::Felucca));
  CHECK(!guard(0x50000, kSector, Right::SaveRestore, Family::Felucca));
  CHECK(!guard(0x97000, kSector, Right::SaveRestore, Family::Stock));
  // No save range overlaps staging or the never-touch areas.
  const FamilyInfo &f = family_info(Family::Felucca);
  for (size_t i = 0; i < f.save_range_count; ++i)
    CHECK(guard(f.save_ranges[i].start, f.save_ranges[i].end - f.save_ranges[i].start,
                Right::SaveRestore, Family::Felucca));
  CHECK(!family_info(Family::Stock).saves_enabled);
}

int main() {
  test_sha256();
  test_crc_and_ciphers();
  test_identities();
  test_real_packages();
  test_damaged_packages();
  test_guard_app();
  test_guard_data();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all fm1_loader package/guard tests passed\n");
  return 0;
}
