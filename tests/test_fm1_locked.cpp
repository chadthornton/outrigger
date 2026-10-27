// Built WITHOUT FM1_LOADER_WRITES, like every firmware build until hardware
// checkpoint 3: nothing may ever erase or write the FM-1's flash, whatever the
// installer is asked to do.
#include <string.h>

#include "installer.h"
#include "support/check.h"
#include "support/mock_rom.h"

using namespace fm1_loader;

int failures = 0;

#if FM1_LOADER_WRITES
#error "this test must be built without FM1_LOADER_WRITES"
#endif

struct FakeHost : LoaderHost {
  int keys = 0;
  bool send_rom_boot_key() override { return ++keys, true; }
  bool wait_for_rom_device(uint32_t) override { return true; }
  bool wait_for_midi_return(uint32_t) override { return true; }
};

static std::vector<uint8_t> flash_with(const std::vector<uint8_t> &app) {
  std::vector<uint8_t> f = synth("fm1-mock", kAppEnd);
  f.resize(kFlashSize, 0xFF);
  memcpy(f.data() + kAppStart, app.data(), kAppLen);
  std::vector<uint8_t> u = synth("unit-A", kSector);
  memcpy(f.data() + kUnitSector, u.data(), kSector);
  return f;
}

static bool wrote(const MockRom &rom) {
  for (const std::string &l : rom.log)
    if (l.compare(0, 4, "fb01") == 0 || l.compare(0, 4, "fb04") == 0) return true;
  return rom.writes != 0 || rom.lowest_touched >= 0;
}

int main() {
  CHECK(!kWritesEnabled);
  const std::vector<uint8_t> blob = read_file("tests/fixtures/wl82loader.bin");
  std::vector<uint8_t> sloop_file = read_file("tests/fixtures/sloop-2.8.1.fwsc");
  std::vector<uint8_t> skiff_file = read_file("tests/fixtures/skiff-1.4.fwsc");
  Package sloop;
  CHECK(parse_package(sloop_file.data(), sloop_file.size(), sloop) == PackageError::None);

  MemoryStorage st;
  Library lib(st);
  CHECK(lib.add_package("skiff-1.4", skiff_file.data(), skiff_file.size()) == PackageError::None);
  FakeHost host;
  MockRom rom(flash_with(sloop.app), blob);
  Installer inst(rom, host, lib, blob);
  InstallOptions o;
  o.confirmed = true;
  o.fresh_start_ok = true;
  o.layout_change_ok = true;

  // An install is refused before the key, so the FM-1 never leaves its firmware.
  inst.start_install("skiff-1.4", o);
  inst.run();
  CHECK(inst.failure() == Failure::WritesLocked);
  CHECK(host.keys == 0 && rom.log.empty());

  // A resume too, before any command.
  inst.start_resume(o);
  inst.run();
  CHECK(inst.failure() == Failure::WritesLocked);
  CHECK(rom.log.empty());

  // The client itself refuses, without sending anything.
  RomClient client(rom);
  uint8_t data[512] = {0};
  CHECK(!client.erase_sector(kAppStart));
  CHECK(!client.flash_write(kAppStart, data, sizeof data));
  CHECK(rom.log.empty());

  // A backup runs whole and writes nothing.
  inst.start_backup(o);
  inst.run();
  CHECK(inst.current() == Step::Done);
  CHECK(!wrote(rom));
  CHECK(rom.was_reset);
  CHECK(rom.violations.empty());

  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all fm1_loader locked-build tests passed\n");
  return 0;
}
