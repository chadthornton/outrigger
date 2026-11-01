// Host-side tests for the bench's sector writer (firmware/waveshare_169/bench_writer.*)
// against the mock FM-1 in ROM boot. Run with `make test`.
#include <string.h>

#include "bench_writer.h"
#include "library.h"
#include "support/check.h"
#include "support/mock_rom.h"

using namespace fm1_loader;

int failures = 0;

using Image = std::vector<uint8_t>;

struct FakeHost : LoaderHost {
  bool rom_ok = true, midi_ok = true;
  int keys = 0;
  bool send_rom_boot_key() override { return ++keys, true; }
  bool wait_for_rom_device(uint32_t) override { return rom_ok; }
  bool wait_for_midi_return(uint32_t) override { return midi_ok; }
};

static const Image &blob() {
  static const Image b = read_file("tests/fixtures/wl82loader.bin");
  return b;
}

// A flash with a unit sector whose ID is the test's bench unit.
static Image flash(const char *unit_text = "bench unit") {
  Image f(kFlashSize, 0xFF);
  memset(f.data() + kUnitSector, 0, kSector);
  memcpy(f.data() + kUnitSector, unit_text, strlen(unit_text));
  return f;
}

static std::string unit_of(const Image &f) { return unit_id_of(f.data() + kUnitSector); }

static bench::Sector mark(uint32_t addr, uint8_t fill) {
  bench::Sector s{addr, Image(kSector, fill)};
  memcpy(s.data.data(), "BENCHMARK", 9);
  return s;
}

static void test_rules() {
  const char *why;
  CHECK(!bench::allowed(0x0000, true, &why));       // boot head
  CHECK(!bench::allowed(0x3000, true, &why));
  CHECK(!bench::allowed(0x4000, true, &why));       // the app
  CHECK(!bench::allowed(0x92000, true, &why));
  CHECK(bench::allowed(0x93000, false, &why));      // stock VM, Felucca data
  CHECK(bench::allowed(0x9F000, false, &why));      // finding 3's sector
  CHECK(bench::allowed(0xE8000, false, &why));
  CHECK(!bench::allowed(0xE9000, false, &why));     // the unit ID, only with protected_ok
  CHECK(bench::allowed(0xE9000, true, &why));
  CHECK(!bench::allowed(0xEA000, false, &why));     // stock's resources, only with protected_ok
  CHECK(!bench::allowed(0xFB000, false, &why));
  CHECK(bench::allowed(0xFB000, true, &why));
  CHECK(bench::allowed(0xFC000, false, &why));      // Felucca globals
  CHECK(bench::allowed(0xFE000, false, &why));
  CHECK(!bench::allowed(0xFF000, true, &why));      // key_mac
  CHECK(!bench::allowed(0x100000, true, &why));
  CHECK(!bench::allowed(0x9F800, true, &why));      // not a sector start
}

static void test_dry_run_writes_nothing() {
  Image f = flash();
  MockRom rom(f, blob());
  FakeHost host;
  bench::WriteOptions o;
  o.unit = unit_of(f);
  const bench::Report r = bench::write_sectors(rom, host, blob(), {mark(0x9F000, 0x11), mark(0xFC000, 0x22)}, o);
  CHECK(r.ok());
  CHECK_EQ(r.differ, 2);
  CHECK_EQ(r.written, 0);
  CHECK(!r.erased);
  CHECK(rom.flash == f);
  CHECK(rom.was_reset);  // back to its firmware
  CHECK_EQ(host.keys, 1);
}

static void test_write_and_verify() {
  Image f = flash();
  MockRom rom(f, blob());
  FakeHost host;
  bench::WriteOptions o;
  o.unit = unit_of(f);
  o.write = true;
  const auto a = mark(0x9F000, 0x11), b = mark(0xFC000, 0x22);
  bench::Sector same{0xE5000, Image(f.begin() + 0xE5000, f.begin() + 0xE6000)};
  const bench::Report r = bench::write_sectors(rom, host, blob(), {a, b, same}, o);
  CHECK(r.ok());
  CHECK_EQ(r.same, 1);
  CHECK_EQ(r.written, 2);
  CHECK(Image(rom.flash.begin() + 0x9F000, rom.flash.begin() + 0xA0000) == a.data);
  CHECK(Image(rom.flash.begin() + 0xFC000, rom.flash.begin() + 0xFD000) == b.data);
  // Nothing else changed.
  Image want = f;
  memcpy(want.data() + 0x9F000, a.data.data(), kSector);
  memcpy(want.data() + 0xFC000, b.data.data(), kSector);
  CHECK(rom.flash == want);
  CHECK(rom.violations.empty());
}

static void test_protected_needs_flag() {
  Image f = flash();
  MockRom rom(f, blob());
  FakeHost host;
  bench::WriteOptions o;
  o.unit = unit_of(f);
  o.write = true;
  bench::Report r = bench::write_sectors(rom, host, blob(), {mark(0x9F000, 1), mark(0xEA000, 2)}, o);
  CHECK(r.stop == bench::Stop::Refused);
  CHECK_EQ(rom.commands, 0);  // refused before the FM-1 heard anything
  CHECK_EQ(host.keys, 0);
  o.protected_ok = true;
  r = bench::write_sectors(rom, host, blob(), {mark(0x9F000, 1), mark(0xEA000, 2)}, o);
  CHECK(r.ok());
  CHECK_EQ(r.written, 2);
}

static void test_head_and_app_always_refused() {
  for (uint32_t addr : {0x0000u, 0x4000u, 0x50000u, 0xFF000u}) {
    Image f = flash();
    MockRom rom(f, blob());
    FakeHost host;
    bench::WriteOptions o;
    o.unit = unit_of(f);
    o.write = o.protected_ok = true;
    const bench::Report r = bench::write_sectors(rom, host, blob(), {mark(addr, 3)}, o);
    CHECK(r.stop == bench::Stop::Refused);
    CHECK_EQ(rom.commands, 0);
  }
}

static void test_wrong_unit_refused() {
  Image f = flash("someone else");
  MockRom rom(f, blob());
  FakeHost host;
  bench::WriteOptions o;
  o.unit = unit_of(flash());
  o.write = true;
  bench::Report r = bench::write_sectors(rom, host, blob(), {mark(0x9F000, 1)}, o);
  CHECK(r.stop == bench::Stop::WrongUnit);
  CHECK(rom.flash == f);
  CHECK(rom.was_reset);
  // No unit named: refused before the key.
  o.unit = "";
  FakeHost host2;
  r = bench::write_sectors(rom, host2, blob(), {mark(0x9F000, 1)}, o);
  CHECK(r.stop == bench::Stop::WrongUnit);
  CHECK_EQ(host2.keys, 0);
}

static void test_fm1_1_refused() {
  FakeHost host;
  bench::WriteOptions o;
  o.unit = bench::kFm11Unit;
  o.write = true;
  Image f = flash();
  MockRom rom(f, blob());
  const bench::Report r = bench::write_sectors(rom, host, blob(), {mark(0x9F000, 1)}, o);
  CHECK(r.stop == bench::Stop::Fm11Unit);
  CHECK_EQ(host.keys, 0);
}

static void test_verify_retries_then_fails() {
  Image f = flash();
  MockRom rom(f, blob());
  rom.bad_cell = 0x9F100;  // this byte never programs correctly
  FakeHost host;
  bench::WriteOptions o;
  o.unit = unit_of(f);
  o.write = true;
  const bench::Report r = bench::write_sectors(rom, host, blob(), {mark(0x9F000, 0x11)}, o);
  CHECK(r.stop == bench::Stop::VerifyFailed);
  CHECK(r.erased);
}

static void test_flaky_write_retried() {
  Image f = flash();
  MockRom rom(f, blob());
  rom.flaky_write = 1;  // the first write programs one wrong bit
  FakeHost host;
  bench::WriteOptions o;
  o.unit = unit_of(f);
  o.write = true;
  const auto a = mark(0x9F000, 0x11);
  const bench::Report r = bench::write_sectors(rom, host, blob(), {a}, o);
  CHECK(r.ok());
  CHECK(Image(rom.flash.begin() + 0x9F000, rom.flash.begin() + 0xA0000) == a.data);
}

static void test_no_rom_device() {
  Image f = flash();
  MockRom rom(f, blob());
  FakeHost host;
  host.rom_ok = false;
  bench::WriteOptions o;
  o.unit = unit_of(f);
  o.write = true;
  const bench::Report r = bench::write_sectors(rom, host, blob(), {mark(0x9F000, 1)}, o);
  CHECK(r.stop == bench::Stop::NoRom);
  CHECK(rom.flash == f);
}

int main() {
  test_rules();
  test_dry_run_writes_nothing();
  test_write_and_verify();
  test_protected_needs_flag();
  test_head_and_app_always_refused();
  test_wrong_unit_refused();
  test_fm1_1_refused();
  test_verify_retries_then_fails();
  test_flaky_write_retried();
  test_no_rom_device();
  if (failures) {
    printf("bench_writer: %d failure(s)\n", failures);
    return 1;
  }
  printf("bench_writer: all passed\n");
  return 0;
}
