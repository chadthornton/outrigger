// Host-side tests for the install state machine against a mock FM-1 in ROM boot.
// The golden tests check the exact SCSI command stream, and the final flash,
// against tests/golden/*.txt from the Unbricker and jl-uboot-tool (make golden).
#include <string.h>

#include "switch_chain.h"
#include "installer.h"
#include "support/check.h"
#include "support/mock_rom.h"

using namespace fm1_loader;

int failures = 0;

using Image = std::vector<uint8_t>;

struct FakeHost : LoaderHost {
  bool rom_ok = true, midi_ok = true;
  int keys = 0;
  uint32_t ms = 0, tick = 0;  // the clock: each reading advances it by tick
  bool send_rom_boot_key() override { return ++keys, true; }
  bool wait_for_rom_device(uint32_t) override { return rom_ok; }
  bool wait_for_midi_return(uint32_t) override { return midi_ok; }
  uint32_t now_ms() override { return ms += tick; }
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

static void add(Library &lib, const char *id, const char *file, const char *project = "") {
  Image data = read_file(std::string("tests/fixtures/") + file);
  CHECK(lib.add_package(id, data.data(), data.size(), nullptr, "", "", project) == PackageError::None);
}

struct Rig {
  MemoryStorage storage;
  Library lib{storage};
  FakeHost host;
  MockRom rom;
  Installer inst{rom, host, lib, blob()};
  explicit Rig(const Image &flash) : rom(flash, blob()) {
    add(lib, "skiff-1.4", "skiff-1.4.fwsc", "skiff");
    // SLOOP's file as a second firmware to switch to, on a project the board
    // doesn't block (families_gen.h blocks SLOOP itself).
    add(lib, "sloop-2.8.1", "sloop-2.8.1.fwsc", "other");
  }
  // "Back up this FM-1", as the board does before an install (the install plans
  // from this backup). Then the mock is an FM-1 back in its firmware, about to
  // be keyed again, with a clean command log for the install's golden stream.
  void back_up(const char *unit_name = "") {
    fresh_rom();
    InstallOptions o;
    o.confirmed = true;
    o.unit_name = unit_name;
    inst.start_backup(o);
    inst.run();
    CHECK(inst.current() == Step::Done);
    fresh_rom();
  }
  void fresh_rom() {
    rom.was_reset = false;
    rom.loader_running = false;
    rom.log.clear();
    rom.reads = rom.writes = rom.commands = 0;
    rom.lowest_touched = -1;
    rom.reads_before_first_erase = -1;
    host.keys = 0;
  }
  void install(const char *id, bool fresh_ok = true, bool layout_ok = false) {
    InstallOptions o;
    o.confirmed = true;
    o.fresh_start_ok = fresh_ok;
    o.layout_change_ok = layout_ok;
    inst.start_install(id, o);
    inst.run();
  }
  Unit unit() {
    Unit u;
    CHECK(lib.unit(lib.last_unit(), u));
    return u;
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
  r.back_up();
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
  r.back_up();
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  CHECK_EQ(r.inst.sectors_total(), uint32_t(0));
  CHECK_EQ(r.rom.writes, 0);
  CHECK_EQ(r.host.keys, 0);  // planned from the backup: nothing to write, no ROM boot
  CHECK(r.unit().installed_package == "skiff-1.4" && !r.unit().has_pending);
  check_golden("noop", r, "done");
}

static void test_golden_retry_once() {
  Rig r(base_flash(sloop()));
  r.back_up();
  r.rom.flaky_write = 3;
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  check_golden("retry_once", r, "done");
}

static void test_golden_retry_fails() {
  Rig r(base_flash(sloop()));
  r.back_up();
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
    r.back_up();
    r.rom.key = 0x1234;
    r.install("skiff-1.4");
    CHECK(r.inst.failure() == Failure::IdentityMismatch);
    CHECK(r.rom.was_reset && !r.inst.danger() && r.rom.lowest_touched < 0);
    check_golden("wrong_key", r, "identity");
  }
  {
    Rig r(base_flash(sloop()));
    r.back_up();
    r.rom.id = 0x856015;
    r.install("skiff-1.4");
    CHECK(r.inst.failure() == Failure::IdentityMismatch);
    check_golden("wrong_flash_id", r, "identity");
  }
}

static void test_golden_backup_mismatch() {
  Rig r(base_flash(sloop()));
  r.rom.flaky_read = 2048 + 7;
  InstallOptions o;
  o.confirmed = true;
  r.inst.start_backup(o);
  r.inst.run();
  CHECK(r.inst.failure() == Failure::BackupMismatch);
  CHECK(r.rom.was_reset && r.rom.lowest_touched < 0);
  CHECK(r.lib.unit_ids().empty());  // no unit is named from an unverified read
  check_golden("backup_mismatch", r, "backup-mismatch");
}

// The install reads the FM-1 once and compares it with the backup it planned
// from: a difference stops it before any erase, with the FM-1 sent home.
static void test_golden_stale_backup() {
  Rig r(base_flash(sloop()));
  r.back_up();
  r.rom.flaky_read = 7;
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::BackupStale);
  CHECK(r.rom.was_reset && r.rom.lowest_touched < 0 && !r.inst.danger());
  CHECK(!r.unit().has_pending);
  CHECK(r.inst.failure_detail().find("0x00000") != std::string::npos);
  check_golden("stale", r, "backup-stale");
  // The FM-1 changed since its backup (a new song): the same.
  Rig s(base_flash(sloop()));
  s.back_up();
  s.rom.flash[0x97000] ^= 1;
  s.install("skiff-1.4");
  CHECK(s.inst.failure() == Failure::BackupStale);
  CHECK(s.rom.lowest_touched < 0 && !s.unit().has_pending);
}

// Checkpoint 3: the FM-1 left ROM boot on its own about 8.5 s after the loader
// started, when the install's two backup reads delayed the first erase. The
// first erase now follows one read of the flash.
static void test_first_erase_soon_after_the_loader() {
  Rig r(base_flash(sloop()));
  r.back_up();
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  const int one_pass = int(kSectors * kSector / 512);
  printf("  reads before the first erase: %d (one pass is %d)\n", r.rom.reads_before_first_erase, one_pass);
  CHECK(r.rom.reads_before_first_erase >= 0 && r.rom.reads_before_first_erase <= one_pass);
  // Too slow anyway (a slow USB link): it stops before the first erase.
  Rig t(base_flash(sloop()));
  t.back_up();
  t.host.tick = 7000;
  t.install("skiff-1.4");
  CHECK(t.inst.failure() == Failure::LoaderTooSlow);
  CHECK(t.rom.lowest_touched < 0 && t.rom.was_reset && !t.inst.danger() && !t.unit().has_pending);
}

// An install that stops before any erase reached the FM-1 leaves nothing: no
// danger, no pending record, and the board doesn't hold the FM-1 in Load mode.
static void test_stop_before_any_erase_leaves_nothing() {
  Rig r(base_flash(sloop()));
  r.back_up();
  r.rom.gone_at_first_erase = true;  // the FM-1 restarted itself before the erase was sent
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::DeviceLost);
  CHECK(!r.inst.danger() && r.rom.lowest_touched < 0);
  CHECK(!r.unit().has_pending);
  // Once an erase may have happened, the record stays: test_disconnect_mid_write.
}

// A verified install records what it wrote as the unit's latest backup, so the
// page's "running" mark and the next preview describe the FM-1 as it is now
// (checkpoint 3: a second Felucca hold was planned from the backup before the first).
static void test_install_records_the_written_flash() {
  Image f = base_flash(skiff());
  memset(f.data() + 0x97000, 0x21, kSector);  // a Skiff project
  Rig r(f);
  r.back_up();
  r.install("sloop-2.8.1");
  CHECK(r.inst.current() == Step::Done);
  Unit u = r.unit();
  FlashHashes latest;
  CHECK(r.lib.backup(u.latest_backup, latest));
  bool same = latest.size() == kSectors;
  for (uint32_t i = 0; same && i < kSectors; ++i)
    same = latest[i] == sha256(r.rom.flash.data() + i * kSector, kSector);
  CHECK(same);
  CHECK(u.baseline_backup != u.latest_backup);  // the baseline stays the first backup
  // The preview for the same package now says there's nothing to write, and a
  // second install of it doesn't send the key.
  InstallPreview p = preview_install(r.lib, u.id, "sloop-2.8.1");
  CHECK(p.ok && p.app_sectors == 0 && p.current.app_sha256 == r.lib.identify_app(
      std::vector<Hash>(latest.begin() + kAppStart / kSector, latest.begin() + kAppEnd / kSector)).app_sha256);
  r.fresh_rom();
  r.install("sloop-2.8.1");
  CHECK(r.inst.current() == Step::Done);
  CHECK_EQ(r.host.keys, 0);
  // And straight back to Skiff, with no backup in between: its check read passes.
  r.fresh_rom();
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  CHECK(r.rom.flash[0x97000] == 0x21);  // the Skiff project came back
}

// The backup an install planned from holds the FM-1's only copy of firmware no
// package has (flashed by Transporter, say): the install's own record doesn't
// replace it. One of a known package still goes, so the store stays small.
static void test_install_keeps_the_only_copy_of_unknown_firmware() {
  Rig r(base_flash(skiff()));
  r.back_up();  // the baseline
  memcpy(r.rom.flash.data() + kAppStart, synth("custom-fw", kAppLen).data(), kAppLen);
  r.back_up();
  const std::string unknown = r.unit().latest_backup;
  CHECK(unknown != r.unit().baseline_backup);
  r.install("sloop-2.8.1");
  CHECK(r.inst.current() == Step::Done);
  FlashHashes kept;
  CHECK(r.lib.backup(unknown, kept));
  CHECK(r.unit().latest_backup != unknown);

  const std::string known = r.unit().latest_backup;  // Sloop: a package
  r.fresh_rom();
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  CHECK(!r.lib.backup(known, kept));
  CHECK(r.lib.backup(unknown, kept));
}

// The board restarted after the key because no ROM device showed up: the job
// runs again without a second key, straight to waiting for ROM boot.
static void test_key_already_sent() {
  Rig r(base_flash(sloop()));
  r.back_up();
  InstallOptions o;
  o.confirmed = true;
  o.fresh_start_ok = true;
  o.key_sent = true;
  r.inst.start_install("skiff-1.4", o);
  r.inst.run();
  CHECK(r.inst.current() == Step::Done);
  CHECK_EQ(r.host.keys, 0);
  r.fresh_rom();
  r.inst.start_backup(o);
  r.inst.run();
  CHECK(r.inst.current() == Step::Done);
  CHECK_EQ(r.host.keys, 0);
}

// No backup of this FM-1 yet: nothing to plan from, so no key either.
static void test_install_needs_a_backup() {
  Rig r(base_flash(sloop()));
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::NoBackupForInstall);
  CHECK(r.host.keys == 0 && r.rom.log.empty());
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

static void test_golden_backup() {
  Image f = base_flash(skiff());
  memset(f.data() + 0x97000, 0x21, kSector);  // a Skiff project: autosaved too
  Rig r(f);
  // gen_golden's backup scenario has no project data; the command stream doesn't
  // depend on content, only the final flash hash does.
  Rig g(base_flash(skiff()));
  InstallOptions o;
  o.confirmed = true;
  g.inst.start_backup(o);
  g.inst.run();
  CHECK(g.inst.current() == Step::Done);
  check_golden("backup", g, "done");

  r.inst.start_backup(o);
  r.inst.run();
  CHECK(r.inst.current() == Step::Done);
  CHECK(r.rom.lowest_touched < 0 && r.rom.writes == 0);  // nothing written
  CHECK(!r.inst.backup_id().empty());
  Unit u;
  CHECK(r.lib.unit(r.inst.unit().id, u) && u.latest_backup == r.inst.backup_id());
  CHECK(u.baseline_backup == r.inst.backup_id() && !u.has_pending);
  CHECK_EQ(r.lib.saves_of(u.id).size(), size_t(1));
  // The region hashes the checkpoint compares with the 2026-10-25 dump.
  Hash head, factory;
  CHECK(r.lib.region_sha256(r.inst.backup_id(), 0, kHeadEnd, head));
  CHECK(r.lib.region_sha256(r.inst.backup_id(), kUnitSector, kFactoryEnd, factory));
  CHECK(head == sha256(f.data(), kHeadEnd));
  CHECK(factory == sha256(f.data() + kUnitSector, kFactoryEnd - kUnitSector));
  CHECK(!r.lib.region_sha256(r.inst.backup_id(), 1, kHeadEnd, head));
  // No package is needed to back up.
  MemoryStorage empty_storage;
  Library empty(empty_storage);
  FakeHost host;
  MockRom rom(base_flash(skiff()), blob());
  Installer inst(rom, host, empty, blob());
  inst.start_backup(o);
  inst.run();
  CHECK(inst.current() == Step::Done);
  // Unconfirmed: nothing at all happens.
  inst.start_backup(InstallOptions());
  inst.run();
  CHECK(inst.failure() == Failure::NotConfirmed);
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
    r.back_up();
    r.install("nope");
    CHECK(r.inst.failure() == Failure::PackageMissing);
    CHECK(r.host.keys == 0 && r.rom.log.empty());
  }
  {
    Rig r(base_flash(sloop()));
    r.back_up();
    r.host.rom_ok = false;
    r.install("skiff-1.4");
    CHECK(r.inst.failure() == Failure::NoRomDevice);
    CHECK(r.host.keys == 1 && r.rom.log.empty());
  }
}

static void test_loader_blob_checked_first() {
  MemoryStorage st;
  Library lib(st);
  add(lib, "skiff-1.4", "skiff-1.4.fwsc", "skiff");
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
  CHECK(host.keys == 0 && rom.log.empty());  // found before the key, so the FM-1 never leaves its firmware
  CHECK(!loader_needs_host_scramble(true, true));
  CHECK(loader_needs_host_scramble(false, true));
  CHECK(loader_needs_host_scramble(true, false));
  CHECK(!loader_needs_host_scramble(false, false));
}

static void test_not_an_fm1() {
  Rig r(base_flash(sloop()));
  r.back_up();
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
  r.back_up();
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

// The board itself stopped the install (not a crash): it knows its loader is
// still running in the FM-1, so the retry skips the upload whatever inquiry says.
static void test_resume_with_loader_running() {
  Rig r(base_flash(sloop()));
  r.back_up();
  InstallOptions o;
  o.confirmed = true;
  o.fresh_start_ok = true;
  r.inst.start_install("skiff-1.4", o);
  while (r.rom.writes < 200 && r.inst.step()) {
  }
  r.rom.loader_product = "UBOOT1.00";  // the unknown case
  const size_t before = r.rom.log.size();
  Installer again(r.rom, r.host, r.lib, blob());
  o.loader_running = true;
  again.start_resume(o);
  again.run();
  CHECK(again.current() == Step::Done);
  CHECK(memcmp(r.rom.flash.data() + kAppStart, skiff().data(), kAppLen) == 0);
  for (size_t i = before; i < r.rom.log.size(); ++i) CHECK(r.rom.log[i].compare(0, 4, "fb06") != 0);
  CHECK(r.rom.violations.empty());
  // The hint is only a hint: a loader that's gone (the FM-1 was reset back to
  // its ROM) doesn't answer, and the upload runs as usual.
  {
    Rig g(base_flash(sloop()));
    g.back_up();
    InstallOptions go;
    go.confirmed = true;
    go.fresh_start_ok = true;
    g.inst.start_install("skiff-1.4", go);
    while (g.rom.writes < 200 && g.inst.step()) {
    }
    g.rom.loader_running = false;  // lost: the ROM answers again
    Installer retry(g.rom, g.host, g.lib, blob());
    go.loader_running = true;
    retry.start_resume(go);
    retry.run();
    CHECK(retry.current() == Step::Done);
    CHECK(memcmp(g.rom.flash.data() + kAppStart, skiff().data(), kAppLen) == 0);
  }
  // A fresh install ignores the hint: the FM-1 just entered ROM boot.
  Rig f(base_flash(sloop()));
  f.back_up();
  f.inst.start_install("skiff-1.4", o);
  f.inst.run();
  CHECK(f.inst.current() == Step::Done);
  check_golden("install", f, "done");
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
  r.back_up();
  r.rom.disconnect_after = 2101 + 40;  // after identify + the check read, a few sectors in
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::DeviceLost);
  CHECK(r.inst.danger() && !r.rom.was_reset);
  Unit u;
  CHECK(r.lib.unit(r.inst.unit().id, u) && u.has_pending);
  // The failure names the command, the address and how far it got.
  const std::string d = r.inst.failure_detail();
  printf("  detail: %s\n", d.c_str());
  CHECK(d.compare(0, 6, "erase ") == 0 || d.compare(0, 6, "write ") == 0 || d.compare(0, 5, "read ") == 0);
  CHECK(d.find(" 0x") != std::string::npos && d.find("sector ") != std::string::npos);
  // During the check read: what and where, and nothing left behind.
  Rig b(base_flash(sloop()));
  b.back_up();
  b.rom.disconnect_after = 100;
  b.install("skiff-1.4");
  CHECK(b.inst.failure() == Failure::DeviceLost);
  CHECK(b.inst.failure_detail().find("check read at") != std::string::npos);
  CHECK(!b.inst.danger() && !b.unit().has_pending);
  // And during a backup.
  Rig k(base_flash(sloop()));
  k.rom.disconnect_after = 100;
  InstallOptions ko;
  ko.confirmed = true;
  k.inst.start_backup(ko);
  k.inst.run();
  CHECK(k.inst.failure_detail().find("backup read 1") != std::string::npos);
  // A sector that won't verify says which.
  Rig v(base_flash(sloop()));
  v.back_up();
  v.rom.bad_cell = long(kAppStart + first_difference() + 0x123);
  v.install("skiff-1.4");
  CHECK(v.inst.failure_detail().find("verify") != std::string::npos);
  // No failure, no detail.
  Rig ok(base_flash(skiff()));
  ok.back_up();
  ok.install("skiff-1.4");
  CHECK(ok.inst.failure_detail().empty());
}

static void test_storage_failures() {
  InstallOptions o;
  o.confirmed = true;
  {
    Rig r(base_flash(sloop()));
    r.storage.fail_writes_after = 2;  // dies on the third new sector of the backup
    r.inst.start_backup(o);
    r.inst.run();
    CHECK(r.inst.failure() == Failure::StorageFailed);
    CHECK(r.rom.was_reset && r.rom.lowest_touched < 0);
  }
  {
    Rig r(base_flash(sloop()));
    r.storage.fail_sync = true;  // the backup can't be made durable
    r.inst.start_backup(o);
    r.inst.run();
    CHECK(r.inst.failure() == Failure::StorageFailed);
    CHECK(r.rom.was_reset && r.rom.lowest_touched < 0);
  }
  {
    Rig r(base_flash(sloop()));
    r.back_up();
    r.storage.fail_sync = true;  // the pending record can't be made durable: no key
    r.install("skiff-1.4");
    CHECK(r.inst.failure() == Failure::StorageFailed);
    CHECK(r.host.keys == 0 && r.rom.log.empty());
  }
}

static void test_fresh_start_needs_ok() {
  Image f = base_flash(sloop());
  memset(f.data() + 0x97000, 0x11, kSector);  // SLOOP data
  Rig r(f);
  r.back_up();
  r.install("skiff-1.4", false);
  CHECK(r.inst.failure() == Failure::NeedsFreshStartOk);
  CHECK(r.host.keys == 0 && r.rom.log.empty());  // refused from the backup, before the key
  CHECK(r.inst.decision().layout_warning);
}

static void test_save_states_end_to_end() {
  Image f = base_flash(skiff());
  memset(f.data() + 0x97000, 0x21, kSector);   // a Skiff project
  memset(f.data() + 0xFC000, 0x22, kSector);   // its globals
  memset(f.data() + kStagingStart, 0x23, 16);  // a stale update record
  const Image skiff_state = f;
  Rig r(f);

  // Skiff -> SLOOP, untested: autosave Skiff, then a clean slate.
  r.back_up();
  r.install("sloop-2.8.1");
  CHECK(r.inst.current() == Step::Done);
  CHECK(r.inst.decision().action == DataAction::FreshStart && r.inst.decision().clean_slate);
  CHECK(r.rom.flash[0x97000] == 0xFF && r.rom.flash[0xFC000] == 0xFF);
  CHECK(r.rom.flash[kStagingStart] == 0xFF);  // the stale update record goes too, never to return
  CHECK_EQ(r.lib.saves_of(r.inst.unit().id).size(), size_t(1));

  // Make a SLOOP song, then go back to Skiff: the Skiff save comes back.
  memset(r.rom.flash.data() + 0x98000, 0x31, kSector);
  r.back_up();
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

  CHECK(r.rom.flash[kStagingStart] == 0xFF);  // staging is never restored
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
  a.back_up();
  a.install("skiff-1.4");
  std::string id_a = a.inst.unit().id;
  // Same storage, the other FM-1.
  MockRom rom_b(base_flash(sloop(), "unit-B"), blob());
  Installer inst_b(rom_b, a.host, a.lib, blob());
  InstallOptions o;
  o.confirmed = true;
  o.fresh_start_ok = true;
  o.unit_name = "B";
  inst_b.start_backup(o);
  inst_b.run();
  CHECK(inst_b.current() == Step::Done);
  rom_b.was_reset = false;
  rom_b.loader_running = false;
  inst_b.start_install("skiff-1.4", o);  // B was backed up last: the install is for B
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
  InstallOptions o;
  o.confirmed = true;
  r.inst.start_backup(o);
  r.inst.run();
  CHECK(r.inst.failure() == Failure::BlankUnitSector);
  CHECK(r.rom.was_reset && r.rom.lowest_touched < 0);
}

// Back to stock is untested: a clean slate, after the user's OK. Stock's factory
// data (0x93000) and resources (0xEA000-0xFBFFF) stay.
static void test_stock_clean_slate() {
  Image f = base_flash(skiff());
  memset(f.data() + 0x97000, 0x41, kSector);  // Skiff data, which stock would read as its own
  memset(f.data() + 0x93000, 0x42, kSector);
  memset(f.data() + 0xEA000, 0x43, kSector);
  Rig r(f);
  add(r.lib, "stock-v15", "FM-1_015.fwsc", "stock");
  r.back_up();
  r.install("stock-v15", false);
  CHECK(r.inst.failure() == Failure::NeedsFreshStartOk);
  CHECK(r.inst.decision().clean_slate && !r.inst.decision().stock_unmapped);
  CHECK(r.host.keys == 0 && r.rom.lowest_touched < 0);
  r.install("stock-v15");
  CHECK(r.inst.current() == Step::Done);
  CHECK(r.rom.flash[0x97000] == 0xFF && r.rom.flash[0x93000] == 0x42 && r.rom.flash[0xEA000] == 0x43);
  CHECK(memcmp(r.rom.flash.data() + kAppStart, pkg("FM-1_015.fwsc").app.data(), kAppLen) == 0);
  CHECK(r.rom.violations.empty());
}

// An untested switch with a save on the target's shelf: a clean slate that
// brings the save back, and it asks first (it erases more than the shelf holds).
static void test_clean_slate_restore() {
  Image f = base_flash(skiff());
  memset(f.data() + 0x9F000, 0x51, kSector);  // Skiff's FM6 user presets, copy A
  memset(f.data() + 0xE8000, 0x52, kSector);  // its second bank sector
  const Image skiff_state = f;
  Rig r(f);
  r.back_up();
  r.install("sloop-2.8.1");
  CHECK(r.inst.current() == Step::Done);
  CHECK(r.rom.flash[0x9F000] == 0xFF && r.rom.flash[0xE8000] == 0xFF);
  memset(r.rom.flash.data() + 0x94000, 0x61, kSector);  // SLOOP-era data where Skiff keeps none
  memset(r.rom.flash.data() + kStagingStart, 0x62, kSector);
  r.back_up();
  r.install("skiff-1.4", false);
  CHECK(r.inst.failure() == Failure::NeedsFreshStartOk);
  CHECK(r.inst.decision().action == DataAction::Restore && r.inst.decision().clean_slate);
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  CHECK(r.rom.flash[0x9F000] == 0x51 && r.rom.flash[0xE8000] == 0x52);
  CHECK(r.rom.flash[0x94000] == 0xFF && r.rom.flash[kStagingStart] == 0xFF);
  CHECK(r.rom.violations.empty());
}

// A crash mid clean slate: the resume finishes the same clean slate.
static void test_resume_clean_slate() {
  Image f = base_flash(sloop());
  memset(f.data() + 0x94000, 0x71, kSector);
  memset(f.data() + kStagingStart, 0x72, kSector);
  memset(f.data() + 0xFC000, 0x73, kSector);
  Rig r(f);
  r.back_up();
  InstallOptions o;
  o.confirmed = true;
  o.fresh_start_ok = true;
  r.inst.start_install("skiff-1.4", o);
  while (!(r.inst.current() == Step::WriteData && r.rom.flash[0x94000] == 0xFF) && r.inst.step()) {
  }
  CHECK(r.inst.current() == Step::WriteData && r.rom.flash[kStagingStart] == 0x72);  // stopped part way
  Installer again(r.rom, r.host, r.lib, blob());
  o.loader_running = true;
  again.start_resume(o);
  again.run();
  CHECK(again.current() == Step::Done);
  CHECK(r.rom.flash[kStagingStart] == 0xFF && r.rom.flash[0xFC000] == 0xFF);
  CHECK(memcmp(r.rom.flash.data() + kAppStart, skiff().data(), kAppLen) == 0);
}

// A blocked project (families_gen.h) is refused before the key, whichever
// reason: Melodee's Bluetooth sector, or SLOOP's older Felucca. The words fit
// both; the preview before the hold gives the specific reason.
static void test_blocked_firmware() {
  for (const char *project : {"melodee", "orbit"}) {
    Rig r(base_flash(sloop()));
    add(r.lib, "blocked-1", "skiff-1.4.fwsc", project);  // any package, on the blocked project
    r.back_up();
    r.install("blocked-1");
    CHECK(r.inst.failure() == Failure::Blocked);
    CHECK(r.host.keys == 0 && r.rom.lowest_touched < 0);
  }
  CHECK(strstr(failure_text(Failure::Blocked), "can't switch to this firmware yet"));
  CHECK(!strstr(failure_text(Failure::Blocked), "Bluetooth"));
}

static void test_resume_with_nothing_left() {
  // The host died after the last verified sector, before the record was cleared.
  Rig r(base_flash(skiff()));
  leave_pending(r, base_flash(sloop()));
  r.inst.start_resume();
  r.inst.run();
  CHECK(r.inst.current() == Step::Done);
  CHECK_EQ(r.inst.sectors_total(), uint32_t(0));
  CHECK(!r.inst.danger());
  Unit u;
  CHECK(r.lib.unit(r.inst.unit().id, u) && !u.has_pending);
}

static void test_autosave_not_repeated() {
  Rig r(base_flash(skiff()));
  r.back_up();
  r.install("skiff-1.4");
  r.back_up();
  r.install("skiff-1.4");
  CHECK(r.inst.current() == Step::Done);
  CHECK_EQ(r.lib.saves_of(r.inst.unit().id).size(), size_t(1));
}

static void test_midi_not_back() {
  Rig r(base_flash(sloop()));
  r.back_up();
  r.host.midi_ok = false;
  r.install("skiff-1.4");
  CHECK(r.inst.failure() == Failure::MidiNotBack);
  CHECK(!r.inst.danger());
  CHECK(memcmp(r.rom.flash.data() + kAppStart, skiff().data(), kAppLen) == 0);
}

// ---------- A switch's chain (switch_chain.h) ----------
// The board runs each stage in its own boot; here each stage runs on the same
// mock FM-1, which goes home between them as the real one does.
struct ChainRun {
  std::vector<loader_messages::Chain> stages;  // the stages that ran, in order
  loader_messages::JobKind last_kind = loader_messages::JobKind::None;
  loader_messages::Chain last_chain = loader_messages::Chain::First;
  bool done = false;
  Failure failure = Failure::None;
};
// between(stage): runs before each stage after the first (the FM-1 changing, a flaky read).
// fresh_ok: the consent the plan's hold carried (the preview's), for every stage.
template <class Between>
static ChainRun switch_to(Rig &r, const char *id, Between between, bool fresh_ok = true) {
  using namespace loader_messages;
  ChainRun c;
  JobKind kind = JobKind::Install;
  Chain chain = Chain::First;
  for (int stage = 0; stage < 4; ++stage) {
    if (stage) between(chain);
    c.stages.push_back(chain);
    if (kind == JobKind::Backup) {
      InstallOptions o;
      o.confirmed = true;
      r.inst.start_backup(o);
      r.inst.run();
    } else {
      r.install(id, fresh_ok);
    }
    c.last_kind = kind;
    c.last_chain = chain;
    c.done = r.inst.current() == Step::Done;
    c.failure = r.inst.failure();
    const NextJob n = next_after(kind, chain, c.done, c.failure, r.inst.danger());
    if (!n.restart) break;
    kind = n.kind;
    chain = n.chain;
    r.fresh_rom();
  }
  return c;
}
static ChainRun switch_to(Rig &r, const char *id) {
  return switch_to(r, id, [](loader_messages::Chain) {});
}

// Nothing changed since the backup: the switch writes straight away.
static void test_switch_unchanged_backs_up_nothing() {
  Rig r(base_flash(skiff()));
  r.back_up();
  const ChainRun c = switch_to(r, "sloop-2.8.1");
  CHECK(c.done && c.stages.size() == 1);
  // One read of the flash (the check) before the first erase: no backup's two.
  CHECK_EQ(r.rom.reads_before_first_erase, int(kSectors * kSector / 512));
}

// The FM-1 changed (a new song): a stopped visit, a backup, then the install,
// with Skiff's songs saved from the new backup.
static void test_switch_changed_backs_up_first() {
  Image f = base_flash(skiff());
  memset(f.data() + 0x97000, 0x21, kSector);  // a Skiff project
  Rig r(f);
  r.back_up();
  const std::string before = r.unit().latest_backup;
  memset(r.rom.flash.data() + 0x97000, 0x22, kSector);  // edited since
  const ChainRun c = switch_to(r, "sloop-2.8.1");
  CHECK(c.done);
  CHECK(c.stages.size() == 3 && c.stages[1] == loader_messages::Chain::Backup &&
        c.stages[2] == loader_messages::Chain::Install);
  CHECK(r.unit().latest_backup != before);
  CHECK(memcmp(r.rom.flash.data() + kAppStart, sloop().data(), kAppLen) == 0);
  // The save is the edited project, not the one in the old backup.
  const std::vector<SaveState> saves = r.lib.saves_of(r.unit().id);
  CHECK(!saves.empty());
  Image edited(kSector, 0x22);
  bool found = false;
  for (const auto &s : saves.back().sectors) found |= s.first == 0x97000 && s.second == sha256(edited.data(), kSector);
  CHECK(found);
}

// No backup at all: the switch backs up first instead of stopping.
static void test_switch_without_backup_backs_up_first() {
  Rig r(base_flash(skiff()));
  const ChainRun c = switch_to(r, "sloop-2.8.1");
  CHECK(c.done && c.stages.size() == 3);
  CHECK(!r.unit().latest_backup.empty());
}

// No backup yet, so no plan to agree to: a switch that turns out to need a
// fresh start backs up, then stops for the user's OK with nothing written.
static void test_switch_without_backup_asks_before_a_fresh_start() {
  Rig r(base_flash(skiff()));
  const ChainRun c = switch_to(r, "sloop-2.8.1", [](loader_messages::Chain) {}, false);
  CHECK(!c.done && c.failure == Failure::NeedsFreshStartOk && c.last_chain == loader_messages::Chain::Install);
  CHECK(!r.unit().latest_backup.empty());
  CHECK(r.rom.lowest_touched < 0 && !r.unit().has_pending);
}

// It changes again right after its backup: the switch stops, nothing written, no loop.
static void test_switch_changed_twice_stops() {
  Rig r(base_flash(skiff()));
  r.back_up();
  r.rom.flash[0x97000] ^= 1;
  const ChainRun c = switch_to(r, "sloop-2.8.1", [&](loader_messages::Chain next) {
    if (next == loader_messages::Chain::Install) r.rom.flash[0x98000] ^= 1;
  });
  CHECK(!c.done && c.failure == Failure::BackupStale && c.last_chain == loader_messages::Chain::Install);
  CHECK(c.stages.size() == 3);
  CHECK(r.rom.lowest_touched < 0 && !r.inst.danger() && !r.unit().has_pending);
}

// The backup in the middle fails: the switch stops with nothing written, and
// no pending record is left to look like "Install unfinished".
static void test_switch_failed_backup_stops() {
  Rig r(base_flash(skiff()));
  r.back_up();
  r.rom.flash[0x97000] ^= 1;
  const ChainRun c = switch_to(r, "sloop-2.8.1", [&](loader_messages::Chain next) {
    if (next == loader_messages::Chain::Backup) r.rom.flaky_read = 300;  // in the second read
  });
  CHECK(!c.done && c.failure == Failure::BackupMismatch && c.last_kind == loader_messages::JobKind::Backup);
  CHECK(c.stages.size() == 2);
  CHECK(r.rom.lowest_touched < 0 && !r.unit().has_pending);
}

int main() {
  test_golden_install();
  test_golden_noop();
  test_golden_retry_once();
  test_golden_retry_fails();
  test_golden_identity();
  test_golden_backup_mismatch();
  test_golden_stale_backup();
  test_switch_unchanged_backs_up_nothing();
  test_switch_changed_backs_up_first();
  test_switch_without_backup_backs_up_first();
  test_switch_without_backup_asks_before_a_fresh_start();
  test_switch_changed_twice_stops();
  test_switch_failed_backup_stops();
  test_first_erase_soon_after_the_loader();
  test_stop_before_any_erase_leaves_nothing();
  test_install_needs_a_backup();
  test_key_already_sent();
  test_install_records_the_written_flash();
  test_install_keeps_the_only_copy_of_unknown_firmware();
  test_golden_resume();
  test_golden_backup();
  test_refusals_before_the_device();
  test_loader_blob_checked_first();
  test_not_an_fm1();
  test_resume_after_host_restart();
  test_resume_with_loader_running();
  test_resume_refusals();
  test_disconnect_mid_write();
  test_storage_failures();
  test_fresh_start_needs_ok();
  test_save_states_end_to_end();
  test_two_units();
  test_blank_unit_sector();
  test_midi_not_back();
  test_stock_clean_slate();
  test_clean_slate_restore();
  test_resume_clean_slate();
  test_blocked_firmware();
  test_resume_with_nothing_left();
  test_autosave_not_repeated();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all fm1_loader installer tests passed\n");
  return 0;
}
