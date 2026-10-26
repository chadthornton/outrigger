// Host-side tests for the install state machine against a mock FM-1 in ROM boot.
// The golden tests check the exact SCSI command stream, and the final flash,
// against tests/golden/*.txt from the Unbricker and jl-uboot-tool (make golden).
#include <string.h>

#include "installer.h"
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

static const Package &pkg(const char *file) {
  static std::map<std::string, Package> cache;
  auto it = cache.find(file);
  if (it == cache.end()) {
    Image data = read_file(std::string("tests/fixtures/") + file);
    Package p;
    CHECK(parse_package(data.data(), data.size(), p) == PackageError::None);
    it = cache.emplace(file, p).first;
  }
  return it->second;
}

static const Image &skiff() { return pkg("skiff-1.4.fwsc").app; }
static const Image &sloop() { return pkg("sloop-2.8.1.fwsc").app; }

// gen_golden.py base_flash(): the Unbricker's synthetic_flash(), an app, unit A's sector.
static Image base_flash(const Image &app, const char *unit = "unit-A") {
  Image f = synth("fm1-mock", kAppEnd);
  f.resize(kFlashSize, 0xFF);
  memcpy(f.data() + kAppStart, app.data(), kAppLen);
  Image u = synth(unit, kSector);
  memcpy(f.data() + kUnitSector, u.data(), kSector);
  return f;
}

static void add(Library &lib, const char *id, const char *file) {
  Image data = read_file(std::string("tests/fixtures/") + file);
  CHECK(lib.add_package(id, data.data(), data.size()) == PackageError::None);
}

struct Rig {
  MemoryStorage storage;
  Library lib{storage};
  FakeHost host;
  MockRom rom;
  Installer inst{rom, host, lib, blob()};
  explicit Rig(const Image &flash) : rom(flash, blob()) {
    add(lib, "skiff-1.4", "skiff-1.4.fwsc");
    add(lib, "sloop-2.8.1", "sloop-2.8.1.fwsc");
  }
  void install(const char *id, bool fresh_ok = true) {
    InstallOptions o;
    o.confirmed = true;
    o.fresh_start_ok = fresh_ok;
    inst.start_install(id, o);
    inst.run();
  }
};

static std::string flash_sha(const Image &f) { return hex(sha256(f.data(), f.size())); }

// Compares the mock's command log and end state with a golden file.
static void check_golden(const char *name, const Rig &r, const char *result) {
  std::vector<std::string> want, tail;
  for (const std::string &l : lines_of(read_text(std::string("tests/golden/") + name + ".txt")))
    (l.compare(0, 2, "# ") == 0 ? tail : want).push_back(l);
  bool same = want.size() == r.rom.log.size();
  for (size_t i = 0; i < want.size() && i < r.rom.log.size(); ++i) {
    if (want[i] != r.rom.log[i]) {
      printf("FAIL golden %s: command %zu is\n  %s\nwant\n  %s\n", name, i, r.rom.log[i].c_str(),
             want[i].c_str());
      same = false;
      break;
    }
  }
  if (want.size() != r.rom.log.size())
    printf("FAIL golden %s: %zu commands, want %zu\n", name, r.rom.log.size(), want.size());
  if (!same) {
    ++failures;
    std::string path = std::string("build/golden-") + name + ".txt";
    FILE *f = fopen(path.c_str(), "w");
    for (const std::string &l : r.rom.log) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    printf("  C++ command stream written to %s\n", path.c_str());
  }
  CHECK_EQ(tail.size(), size_t(2));
  CHECK_EQ("# flash " + flash_sha(r.rom.flash), tail[0]);
  CHECK_EQ("# result " + std::string(result), tail[1]);
  CHECK(r.rom.violations.empty());
}

static size_t first_difference() {
  for (size_t o = 0; o < kAppLen; o += kSector)
    if (memcmp(skiff().data() + o, sloop().data() + o, kSector) != 0) return o;
  return 0;
}

static void test_golden_install() {
  Rig r(base_flash(sloop()));
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  CHECK(!r.inst.danger());
  CHECK_EQ(r.host.keys, 1);
  CHECK_EQ(r.rom.jump_arg, uint16_t(1));
  CHECK(r.rom.lowest_touched >= long(kAppStart));
  check_golden("install", r, "done");
  Unit u;
  CHECK(r.lib.unit(r.inst.unit().id, u));
  CHECK(u.installed_package == "skiff-1.4" && !u.has_pending && !u.baseline_backup.empty());
}

static void test_golden_noop() {
  Rig r(base_flash(skiff()));
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  CHECK_EQ(r.inst.sectors_total(), uint32_t(0));
  CHECK_EQ(r.rom.writes, 0);
  check_golden("noop", r, "done");
}

static void test_golden_retry_once() {
  Rig r(base_flash(sloop()));
  r.rom.flaky_write = 3;
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  check_golden("retry_once", r, "done");
}

static void test_golden_retry_fails() {
  Rig r(base_flash(sloop()));
  r.rom.bad_cell = long(kAppStart + first_difference() + 0x123);
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::WriteVerifyFailed);
  CHECK(r.inst.danger());     // "Don't unplug: retry or restore backup"
  CHECK(!r.rom.was_reset);    // stays in ROM boot
  check_golden("retry_fails", r, "write-failed");
  Unit u;
  CHECK(r.lib.unit(r.inst.unit().id, u) && u.has_pending && u.pending.package_id == "skiff-1.4");
}

static void test_golden_identity() {
  {
    Rig r(base_flash(sloop()));
    r.rom.key = 0x1234;
    r.install("skiff-1.4");
    CHECK(r.inst.failure() == Failure::IdentityMismatch);
    CHECK(r.rom.was_reset && !r.inst.danger() && r.rom.lowest_touched < 0);
    check_golden("wrong_key", r, "identity");
  }
  {
    Rig r(base_flash(sloop()));
    r.rom.id = 0x856015;
    r.install("skiff-1.4");
    CHECK(r.inst.failure() == Failure::IdentityMismatch);
    check_golden("wrong_flash_id", r, "identity");
  }
}

static void test_golden_backup_mismatch() {
  Rig r(base_flash(sloop()));
  r.rom.flaky_read = 2048 + 7;
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::BackupMismatch);
  CHECK(r.rom.was_reset && r.rom.lowest_touched < 0);
  CHECK(r.lib.unit_ids().empty());  // no unit is named from an unverified read
  check_golden("backup_mismatch", r, "backup-mismatch");
}

// The unit record a crashed install leaves: a verified backup and the pending install.
static void leave_pending(Rig &r, const Image &before) {
  Unit u;
  u.id = unit_id_of(before.data() + kUnitSector);
  FlashHashes h(kSectors);
  for (uint32_t i = 0; i < kSectors; ++i) CHECK(r.lib.sectors().put(before.data() + i * kSector, h[i]));
  std::string b;
  CHECK(r.lib.add_backup(u, h, b));
  u.has_pending = true;
  u.pending.package_id = "skiff-1.4";
  u.pending.action = DataAction::Keep;
  u.pending.before_backup = b;
  CHECK(r.lib.put_unit(u));
}

static void test_golden_resume() {
  Image half = sloop();
  memcpy(half.data(), skiff().data(), 0x20000);
  memset(half.data() + 0x20000, 0xFF, kSector);
  Rig r(base_flash(half));
  leave_pending(r, base_flash(sloop()));
  r.inst.start_resume();
  CHECK(r.inst.danger());
  r.inst.run();
  CHECK(r.inst.current() == Step::Done);
  CHECK_EQ(r.host.keys, 0);  // a resume never sends the key
  CHECK(!r.inst.danger());
  check_golden("resume", r, "done");
  Unit u;
  CHECK(r.lib.unit(r.inst.unit().id, u) && !u.has_pending && u.installed_package == "skiff-1.4");
}

static void test_refusals_before_the_device() {
  {
    Rig r(base_flash(sloop()));
    InstallOptions o;  // no confirmation
    r.inst.start_install("skiff-1.4", o);
    r.inst.run();
    CHECK(r.inst.failure() == Failure::NotConfirmed);
    CHECK(r.host.keys == 0 && r.rom.log.empty());
  }
  {
    Rig r(base_flash(sloop()));
    r.install("nope");
    CHECK(r.inst.failure() == Failure::PackageMissing);
    CHECK(r.host.keys == 0 && r.rom.log.empty());
  }
  {
    Rig r(base_flash(sloop()));
    r.host.rom_ok = false;
    r.install("skiff-1.4");
    CHECK(r.inst.failure() == Failure::NoRomDevice);
    CHECK(r.host.keys == 1 && r.rom.log.empty());
  }
}

static void test_loader_blob_checked_first() {
  MemoryStorage st;
  Library lib(st);
  add(lib, "skiff-1.4", "skiff-1.4.fwsc");
  FakeHost host;
  Image bad = blob();
  bad[100] ^= 1;
  MockRom rom(base_flash(sloop()), bad);
  Installer inst(rom, host, lib, bad);
  InstallOptions o;
  o.confirmed = true;
  inst.start_install("skiff-1.4", o);
  inst.run();
  CHECK(inst.failure() == Failure::LoaderBlobBad);
  CHECK_EQ(rom.log.size(), size_t(1));  // the inquiry only: no FB06 sent
  CHECK(!loader_needs_host_scramble(true, true));
  CHECK(loader_needs_host_scramble(false, true));
  CHECK(loader_needs_host_scramble(true, false));
  CHECK(!loader_needs_host_scramble(false, false));
}

static void test_not_an_fm1() {
  Rig r(base_flash(sloop()));
  r.rom.loader_running = true;  // not UBOOT1.00 and the loader doesn't answer either
  r.rom.loader_product = "UDISK";
  r.rom.disconnect_after = 1;
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::NoLoader);
  CHECK(r.rom.lowest_touched < 0);
}

// Host restart in the middle of the app write, then the resume entry.
static void host_restart(const char *loader_product, bool expect_done) {
  Rig r(base_flash(sloop()));
  InstallOptions o;
  o.confirmed = true;
  o.fresh_start_ok = true;
  r.inst.start_install("skiff-1.4", o);
  while (r.rom.writes < 200 && r.inst.step()) {
  }
  CHECK(r.inst.current() == Step::WriteApp && r.inst.danger());
  r.rom.loader_product = loader_product;  // what inquiry says once the loader runs: unknown
  Installer again(r.rom, r.host, r.lib, blob());
  again.start_resume();
  again.run();
  if (expect_done) {
    CHECK(again.current() == Step::Done);
    CHECK(memcmp(r.rom.flash.data() + kAppStart, skiff().data(), kAppLen) == 0);
    CHECK(r.rom.violations.empty());
  } else {
    // It re-uploads into a running loader, which refuses: it stops and stays put.
    CHECK(again.failure() == Failure::LoaderFailed);
    CHECK(again.danger() && !r.rom.was_reset);
  }
}

static void test_resume_after_host_restart() {
  host_restart("LOADER", true);
  host_restart("UBOOT1.00", false);
}

static void test_resume_refusals() {
  {
    Rig r(base_flash(sloop()));  // never seen: no backup
    r.inst.start_resume();
    r.inst.run();
    CHECK(r.inst.failure() == Failure::NoBackupForResume);
    CHECK(!r.rom.was_reset && r.rom.writes == 0);
  }
  {
    Rig r(base_flash(sloop()));
    leave_pending(r, base_flash(sloop()));
    Unit u;
    CHECK(r.lib.unit(unit_id_of(base_flash(sloop()).data() + kUnitSector), u));
    u.has_pending = false;
    CHECK(r.lib.put_unit(u));
    r.inst.start_resume();
    r.inst.run();
    CHECK(r.inst.failure() == Failure::NothingToResume);
    CHECK(!r.rom.was_reset && r.rom.writes == 0);
  }
  {
    // The pending record is gone after a crash and a restart of the storage too.
    Rig r(base_flash(sloop()));
    leave_pending(r, base_flash(sloop()));
    r.storage.sync();
    r.storage.lose_unsynced();
    r.inst.start_resume();
    r.inst.run();
    CHECK(r.inst.current() == Step::Done);
  }
}

static void test_disconnect_mid_write() {
  Rig r(base_flash(sloop()));
  r.rom.disconnect_after = 4149 + 40;  // after identify + backup, a few sectors in
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::DeviceLost);
  CHECK(r.inst.danger() && !r.rom.was_reset);
  Unit u;
  CHECK(r.lib.unit(r.inst.unit().id, u) && u.has_pending);
}

static void test_storage_failures() {
  {
    Rig r(base_flash(sloop()));
    r.storage.fail_writes_after = 2;  // dies on the third new sector of the backup
    r.install("skiff-1.4");
    CHECK(r.inst.failure() == Failure::StorageFailed);
    CHECK(r.rom.was_reset && r.rom.lowest_touched < 0);
  }
  {
    Rig r(base_flash(sloop()));
    r.storage.fail_sync = true;  // the backup can't be made durable
    r.install("skiff-1.4");
    CHECK(r.inst.failure() == Failure::StorageFailed);
    CHECK(r.rom.was_reset && r.rom.lowest_touched < 0);
  }
}

static void test_fresh_start_needs_ok() {
  Image f = base_flash(sloop());
  memset(f.data() + 0x97000, 0x11, kSector);  // SLOOP data
  Rig r(f);
  r.install("skiff-1.4", false);
  CHECK(r.inst.failure() == Failure::NeedsFreshStartOk);
  CHECK(r.rom.was_reset && r.rom.lowest_touched < 0);
  CHECK(r.inst.decision().layout_warning);
}

static void test_save_states_end_to_end() {
  Image f = base_flash(skiff());
  memset(f.data() + 0x97000, 0x21, kSector);   // a Skiff project
  memset(f.data() + 0xFC000, 0x22, kSector);   // its globals
  memset(f.data() + kStagingStart, 0x23, 16);  // a stale update record
  const Image skiff_state = f;
  Rig r(f);

  // Skiff -> SLOOP: autosave Skiff, start fresh.
  r.install("sloop-2.8.1");
  CHECK(r.inst.current() == Step::Done);
  CHECK(r.inst.decision().action == DataAction::FreshStart);
  CHECK(r.rom.flash[0x97000] == 0xFF && r.rom.flash[0xFC000] == 0xFF);
  CHECK(r.rom.flash[kStagingStart] == 0x23);  // staging is never touched
  CHECK_EQ(r.lib.saves_of(r.inst.unit().id).size(), size_t(1));

  // Make a SLOOP song, then go back to Skiff: the Skiff save comes back.
  r.rom.was_reset = false;
  r.rom.loader_running = false;
  memset(r.rom.flash.data() + 0x98000, 0x31, kSector);
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  CHECK(r.inst.decision().action == DataAction::Restore);
  const FamilyInfo &fam = family_info(Family::Felucca);
  for (size_t i = 0; i < fam.save_range_count; ++i) {
    const Range &g = fam.save_ranges[i];
    CHECK(memcmp(r.rom.flash.data() + g.start, skiff_state.data() + g.start, g.end - g.start) == 0);
  }
  CHECK(memcmp(r.rom.flash.data() + kAppStart, skiff().data(), kAppLen) == 0);
  CHECK_EQ(r.lib.saves_of(r.inst.unit().id).size(), size_t(2));  // and SLOOP's was saved

  // Protected areas never changed through all of it.
  CHECK(memcmp(r.rom.flash.data(), skiff_state.data(), kHeadEnd) == 0);
  CHECK(memcmp(r.rom.flash.data() + kUnitSector, skiff_state.data() + kUnitSector,
               kFlashSize - kUnitSector - (kGlobalsEnd - kGlobalsStart) - (kFlashSize - kKeyMacStart)) == 0);
  CHECK(memcmp(r.rom.flash.data() + kKeyMacStart, skiff_state.data() + kKeyMacStart,
               kFlashSize - kKeyMacStart) == 0);
  CHECK(r.rom.lowest_touched >= long(kAppStart));
}

static void test_two_units() {
  Rig a(base_flash(sloop(), "unit-A"));
  a.install("skiff-1.4");
  std::string id_a = a.inst.unit().id;
  // Same storage, the other FM-1.
  MockRom rom_b(base_flash(sloop(), "unit-B"), blob());
  Installer inst_b(rom_b, a.host, a.lib, blob());
  InstallOptions o;
  o.confirmed = true;
  o.fresh_start_ok = true;
  o.unit_name = "B";
  inst_b.start_install("skiff-1.4", o);
  inst_b.run();
  CHECK(inst_b.current() == Step::Done);
  CHECK(inst_b.unit().id != id_a);
  CHECK_EQ(a.lib.unit_ids().size(), size_t(2));
  Unit ub;
  CHECK(a.lib.unit(inst_b.unit().id, ub) && ub.name == "B" && !ub.baseline_backup.empty());
}

static void test_blank_unit_sector() {
  Image f = base_flash(sloop());
  memset(f.data() + kUnitSector, 0xFF, kSector);
  Rig r(f);
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::BlankUnitSector);
  CHECK(r.rom.was_reset && r.rom.lowest_touched < 0);
}

static void test_midi_not_back() {
  Rig r(base_flash(sloop()));
  r.host.midi_ok = false;
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::MidiNotBack);
  CHECK(!r.inst.danger());
  CHECK(memcmp(r.rom.flash.data() + kAppStart, skiff().data(), kAppLen) == 0);
}

int main() {
  test_golden_install();
  test_golden_noop();
  test_golden_retry_once();
  test_golden_retry_fails();
  test_golden_identity();
  test_golden_backup_mismatch();
  test_golden_resume();
  test_refusals_before_the_device();
  test_loader_blob_checked_first();
  test_not_an_fm1();
  test_resume_after_host_restart();
  test_resume_refusals();
  test_disconnect_mid_write();
  test_storage_failures();
  test_fresh_start_needs_ok();
  test_save_states_end_to_end();
  test_two_units();
  test_blank_unit_sector();
  test_midi_not_back();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all fm1_loader installer tests passed\n");
  return 0;
}
