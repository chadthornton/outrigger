// Checks firmware packages the tests don't pin (each firmware's newest release,
// fetched by tools/check_latest.py) the way the installer tests check the pinned
// ones: the package parses, the library takes it, and a mock FM-1 running the
// stock firmware is backed up, has the package installed, and ends with the
// package's app in its flash; installing it again writes nothing.
//
// What it can't check: anything that needs the real FM-1, above all whether the
// new version reads the data an older one saved (docs/firmware-loader-plan.md).
//
// usage: check_package <wl82loader.bin> <stock.fwsc> <package.fwsc>...
#include <stdio.h>
#include <string.h>

#include "installer.h"
#include "support/check.h"
#include "support/mock_rom.h"

using namespace fm1_loader;

int failures = 0;

struct Host : LoaderHost {
  int keys = 0;
  bool send_rom_boot_key() override { return ++keys, true; }
  bool wait_for_rom_device(uint32_t) override { return true; }
  bool wait_for_midi_return(uint32_t) override { return true; }
};

// The stock firmware's app, an FM-1's own unit sector, the rest synthetic.
static std::vector<uint8_t> stock_flash(const Package &stock) {
  std::vector<uint8_t> f = synth("fm1-mock", kAppEnd);
  f.resize(kFlashSize, 0xFF);
  memcpy(f.data() + kAppStart, stock.app.data(), kAppLen);
  std::vector<uint8_t> u = synth("unit-A", kSector);
  memcpy(f.data() + kUnitSector, u.data(), kSector);
  return f;
}

static bool check(const std::vector<uint8_t> &blob, const std::vector<uint8_t> &stock_file, const char *path) {
  const int before = failures;
  std::vector<uint8_t> file = read_file(path);
  Package p;
  const PackageError err = parse_package(file.data(), file.size(), p);
  if (err != PackageError::None) {
    printf("%s: FAIL %s\n", path, package_error_text(err));
    return false;
  }
  Package stock;
  CHECK(parse_package(stock_file.data(), stock_file.size(), stock) == PackageError::None);

  MemoryStorage storage;
  Library lib(storage);
  CHECK(lib.add_package("stock", stock_file.data(), stock_file.size(), nullptr, "", "", "stock") == PackageError::None);
  CHECK(lib.add_package("candidate", file.data(), file.size()) == PackageError::None);
  Host host;
  MockRom rom(stock_flash(stock), blob);
  Installer inst(rom, host, lib, blob);
  InstallOptions o;
  o.confirmed = true;
  inst.start_backup(o);
  inst.run();
  CHECK(inst.current() == Step::Done);

  rom.was_reset = false;
  rom.loader_running = false;
  o.fresh_start_ok = o.layout_change_ok = true;  // a mock: any data decision may run
  inst.start_install("candidate", o);
  inst.run();
  if (inst.current() != Step::Done) printf("  install stopped: %s %s\n", failure_text(inst.failure()),
                                           inst.failure_detail().c_str());
  CHECK(inst.current() == Step::Done);
  CHECK(memcmp(rom.flash.data() + kAppStart, p.app.data(), kAppLen) == 0);
  const char *action = inst.decision().action == DataAction::FreshStart ? "fresh start"
                       : inst.decision().action == DataAction::Restore  ? "restore"
                                                                        : "keep";
  const uint32_t written = inst.sectors_total();

  rom.was_reset = false;
  rom.loader_running = false;
  host.keys = 0;
  inst.start_install("candidate", o);
  inst.run();
  CHECK(inst.current() == Step::Done);
  CHECK_EQ(host.keys, 0);  // already installed: no ROM boot

  const bool ok = failures == before;
  printf("%s: %s %s, %u sectors written over stock, data: %s\n", path, ok ? "ok" : "FAIL",
         p.identity.c_str(), unsigned(written), action);
  return ok;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: check_package <wl82loader.bin> <stock.fwsc> <package.fwsc>...\n");
    return 2;
  }
  const std::vector<uint8_t> blob = read_file(argv[1]);
  const std::vector<uint8_t> stock = read_file(argv[2]);
  int bad = 0;
  for (int i = 3; i < argc; ++i) bad += check(blob, stock, argv[i]) ? 0 : 1;
  return bad ? 1 : 0;
}
