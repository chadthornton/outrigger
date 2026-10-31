// Host-side tests for fm1_loader's sector store, library, units, save states and
// the sector planners.
#include <string.h>

#include "library.h"
#include "support/check.h"
#include "support/temp_storage.h"

using namespace fm1_loader;

int failures = 0;

// test_fm1_library runs these on MemoryStorage, test_fm1_library_posix on the
// file-system back end the Waveshare uses. Fault injection is MemoryStorage only.
#ifdef TEST_POSIX_STORAGE
using TestStorage = TempPosixStorage;
#else
using TestStorage = MemoryStorage;
#endif

using Image = std::vector<uint8_t>;

static Image blank_image() { return Image(kFlashSize, 0xFF); }

static void fill(Image &img, uint32_t addr, uint32_t len, uint8_t seed) {
  for (uint32_t i = 0; i < len; ++i) img[addr + i] = uint8_t(seed + i * 7 + (i >> 8));
}

static FlashHashes store_image(Library &lib, const Image &img) {
  FlashHashes out(kSectors);
  for (uint32_t i = 0; i < kSectors; ++i) CHECK(lib.sectors().put(img.data() + i * kSector, out[i]));
  return out;
}

// Applies planned ops to an image the way NOR flash would: erase, then write.
static void apply(Library &lib, Image &img, const std::vector<SectorOp> &ops) {
  for (const SectorOp &op : ops) {
    CHECK(guard(op.addr, kSector, op.right, op.family));
    CHECK(lib.sectors().get(op.want, img.data() + op.addr));
  }
}

static Image unit_image(uint8_t seed) {
  Image img = blank_image();
  fill(img, 0, kHeadEnd, 1);
  fill(img, kUnitSector, kSector, seed);
  return img;
}

static PackageInfo load(Library &lib, const char *id, const char *file, const char *project = "") {
  std::vector<uint8_t> data = read_file(std::string("tests/fixtures/") + file);
  CHECK(lib.add_package(id, data.data(), data.size(), nullptr, "", "", project) == PackageError::None);
  PackageInfo p;
  CHECK(lib.package(id, p));
  return p;
}

static void put_app(Library &lib, Image &img, const PackageInfo &p) {
  for (uint32_t i = 0; i < kAppSectors; ++i)
    CHECK(lib.sectors().get(p.app[i], img.data() + kAppStart + i * kSector));
}

static void test_sector_store() {
  TestStorage st;
  SectorStore s(st);
  std::vector<uint8_t> a(kSector), b(kSector), ff(kSector, 0xFF), out(kSector);
  fill(a, 0, kSector, 3);
  fill(b, 0, kSector, 4);
  Hash ha, ha2, hb, hf;
  CHECK(s.put(a.data(), ha));
  CHECK(s.put(a.data(), ha2));
  CHECK(s.put(b.data(), hb));
  CHECK(s.put(ff.data(), hf));
  CHECK(ha == ha2);
  CHECK(hf == SectorStore::blank());
  CHECK_EQ(s.count(), size_t(2));  // deduplicated, blank not stored
  CHECK(s.get(ha, out.data()) && out == a);
  CHECK(s.get(hf, out.data()) && out == ff);
  // A damaged stored sector is refused, never handed out.
  std::vector<uint8_t> bad = b;
  bad[100] ^= 1;
  st.write("s/" + hex(hb).substr(0, 2) + "/" + hex(hb).substr(2), bad.data(), bad.size());
  CHECK(!s.get(hb, out.data()));
  CHECK_EQ(s.collect_garbage({ha}), size_t(1));
  CHECK(s.has(ha) && !s.has(hb) && s.has(SectorStore::blank()));
}

static void test_manifest() {
  Manifest m;
  m.fields["identity"] = "FM-1_714";
  m.sectors.emplace_back(0x4000, sha256(nullptr, 0));
  Manifest back;
  CHECK(Manifest::parse(m.serialize(), back));
  CHECK(back.get("identity") == "FM-1_714" && back.sectors == m.sectors);
  std::string torn = m.serialize();
  torn.pop_back();
  CHECK(!Manifest::parse(torn, back));
  CHECK(!Manifest::parse("@zz " + hex(sha256(nullptr, 0)) + "\n", back));
}

static void test_packages() {
  TestStorage st;
  Library lib(st);
  PackageInfo skiff = load(lib, "skiff-1.4", "skiff-1.4.fwsc");
  size_t after_one = lib.sectors().count();
  PackageInfo sloop = load(lib, "sloop-2.8.1", "sloop-2.8.1.fwsc");
  CHECK_EQ(skiff.identity, std::string("FM-1_714"));
  CHECK_EQ(sloop.identity, std::string("FM-1_900"));
  CHECK_EQ(skiff.app.size(), size_t(kAppSectors));
  CHECK(lib.sectors().count() > after_one);
  CHECK(lib.sectors().count() <= 2 * kAppSectors);
  CHECK_EQ(lib.package_ids().size(), size_t(2));

  Firmware fw = lib.identify_app(skiff.app);
  CHECK(fw.known() && fw.identity == "FM-1_714" && fw.app_sha256 == skiff.app_sha256);
  std::vector<Hash> other = skiff.app;
  other[5] = sloop.app[5] == skiff.app[5] ? SectorStore::blank() : sloop.app[5];
  fw = lib.identify_app(other);
  CHECK(!fw.known());

  std::vector<uint8_t> data = read_file("tests/fixtures/sloop-2.8.1.fwsc");
  CHECK(lib.add_package("../x", data.data(), data.size()) == PackageError::BadId);
  Hash wrong{};
  CHECK(lib.add_package("x", data.data(), data.size(), &wrong) == PackageError::HashMismatch);
#ifndef TEST_POSIX_STORAGE
  st.fail_writes_after = 0;
  CHECK(lib.add_package("stock", read_file("tests/fixtures/FM-1_015.fwsc").data(), 699956) ==
        PackageError::StoreFailed);
#endif
}

static void test_remove_package() {
  TestStorage st;
  Library lib(st);
  load(lib, "skiff-1.4", "skiff-1.4.fwsc");
  load(lib, "sloop-2.8.1", "sloop-2.8.1.fwsc");
  Unit u;
  u.id = "0123456789abcdef";
  u.installed_package = "skiff-1.4";
  u.has_pending = true;
  u.pending.package_id = "sloop-2.8.1";
  CHECK(lib.put_unit(u));
  CHECK(!lib.remove_package("skiff-1.4"));    // installed on a unit
  CHECK(!lib.remove_package("sloop-2.8.1"));  // an unfinished install needs it
  u.has_pending = false;
  CHECK(lib.put_unit(u));
  CHECK(lib.remove_package("sloop-2.8.1"));
  CHECK_EQ(lib.package_ids().size(), size_t(1));
}

static void test_unit_ids() {
  std::vector<uint8_t> ff(kSector, 0xFF), zero(kSector, 0), a(kSector), b(kSector);
  fill(a, 0, kSector, 1);
  fill(b, 0, kSector, 2);
  CHECK(unit_id_of(ff.data()).empty());
  CHECK(unit_id_of(zero.data()).empty());
  CHECK_EQ(unit_id_of(a.data()).size(), size_t(16));
  CHECK(unit_id_of(a.data()) != unit_id_of(b.data()));
  CHECK(unit_id_of(a.data()) == unit_id_of(a.data()));
}

static void test_backups() {
  TestStorage st;
  Library lib(st);
  Image img = unit_image(9);
  Unit u;
  u.id = unit_id_of(img.data() + kUnitSector);
  FlashHashes h1 = store_image(lib, img);
  std::string b1, b2, b3;
  CHECK(lib.add_backup(u, h1, b1));
  CHECK_EQ(u.baseline_backup, b1);
  CHECK_EQ(u.latest_backup, b1);
  fill(img, 0x97000, kSector, 5);
  CHECK(lib.add_backup(u, store_image(lib, img), b2));
  fill(img, 0x97000, kSector, 6);  // b2's version of this sector is now unused
  CHECK(lib.add_backup(u, store_image(lib, img), b3));
  Unit back;
  CHECK(lib.unit(u.id, back));
  CHECK_EQ(back.baseline_backup, b1);  // kept forever
  CHECK_EQ(back.latest_backup, b3);
  FlashHashes got;
  CHECK(lib.backup(b1, got) && got == h1);
  CHECK(!lib.backup(b2, got));  // superseded latest is dropped
  // Its unique sector goes with the next garbage collection; the baseline's stay.
  CHECK(lib.collect_garbage() >= 1);
  CHECK(lib.backup(b1, got));
  Image out(kFlashSize);
  for (uint32_t i = 0; i < kSectors; ++i) CHECK(lib.sectors().get(got[i], out.data() + i * kSector));
  CHECK(out == unit_image(9));

  // A backup naming a sector the store doesn't have is refused.
  FlashHashes missing = h1;
  missing[3] = sha256(nullptr, 0);
  std::string b4;
  CHECK(!lib.add_backup(u, missing, b4));
#ifndef TEST_POSIX_STORAGE
  // A storage that can't sync can't record a backup.
  st.fail_sync = true;
  CHECK(!lib.add_backup(u, h1, b4));
#endif
}

static void test_diff_planner() {
  TestStorage st;
  Library lib(st);
  PackageInfo skiff = load(lib, "skiff-1.4", "skiff-1.4.fwsc");
  Image img = unit_image(9);
  put_app(lib, img, skiff);
  CHECK(plan_app(store_image(lib, img), skiff).empty());  // identical: nothing to write
  img[0x50123] ^= 0x01;
  std::vector<SectorOp> ops = plan_app(store_image(lib, img), skiff);
  CHECK_EQ(ops.size(), size_t(1));
  CHECK(ops.size() == 1 && ops[0].addr == 0x50000 && ops[0].right == Right::App);
  // Differences outside the app area are never planned.
  fill(img, 0, kHeadEnd, 77);
  fill(img, 0x93000, 0x6D000, 33);
  CHECK_EQ(plan_app(store_image(lib, img), skiff).size(), size_t(1));
  PackageInfo sloop = load(lib, "sloop-2.8.1", "sloop-2.8.1.fwsc");
  for (const SectorOp &op : plan_app(store_image(lib, img), sloop))
    CHECK(guard(op.addr, kSector, Right::App, op.family));
}

static void test_capture_save() {
  TestStorage st;
  Library lib(st);
  Image img = unit_image(9);
  fill(img, 0x97000, kSector, 1);       // a song
  fill(img, kStagingStart, kSector, 2);  // update staging: never in a save
  fill(img, 0xFC000, kSector, 3);       // globals
  fill(img, 0x93000, kSector, 4);       // stock's area, not Felucca's
  FlashHashes h = store_image(lib, img);
  Firmware fw;
  fw.identity = "FM-1_714";
  fw.family = Family::Felucca;
  SaveState s = capture_save("u", fw, h);
  CHECK_EQ(s.sectors.size(), size_t(2));
  for (const auto &sec : s.sectors) {
    CHECK(sec.first == 0x97000 || sec.first == 0xFC000);
    CHECK(!(sec.first >= kStagingStart && sec.first < kStagingEnd));
  }
  fw.identity = "FM-1_015";
  fw.family = Family::Stock;
  CHECK(capture_save("u", fw, h).sectors.empty());  // stock saves are disabled
}

// A package of a catalog project (docs/package-sources.md): the family follows the
// project, never the identity's digits.
static PackageInfo info(const char *project, const char *identity, uint8_t app_tag) {
  PackageInfo p;
  p.project = project;
  p.identity = identity;
  p.family = family_of_project(project);
  p.app_sha256.fill(app_tag);
  return p;
}

static Firmware running(const PackageInfo &p) {
  Firmware f;
  f.project = p.project;
  f.identity = p.identity;
  f.app_sha256 = p.app_sha256;
  f.family = p.family;
  return f;
}

static SaveState save_of(const char *unit, const PackageInfo &p, uint32_t seq) {
  SaveState s;
  s.id = std::string(unit) + "-s" + std::to_string(seq);
  s.unit = unit;
  s.project = p.project;
  s.identity = p.identity;
  s.app_sha256 = p.app_sha256;
  s.family = p.family;
  s.seq = seq;
  return s;
}

// The trust table: data is kept only within a project, and each project has its
// own shelf of saves.
static void test_decide_data() {
  PackageInfo skiff = info("skiff", "FM-1_714", 1), skiff13 = info("skiff", "FM-1_713", 2);
  PackageInfo skiff2 = info("skiff", "FM-1_720", 3);
  PackageInfo felucca = info("felucca", "FM-1_916", 4), felucca17 = info("felucca", "FM-1_917", 5);
  PackageInfo sloop = info("sloop", "FM-1_900", 6), stock = info("stock", "FM-1_015", 7);
  std::vector<SaveState> none;
  // No-op install.
  DataDecision d = decide_data("A", running(skiff), skiff, none);
  CHECK(d.action == DataAction::Keep && !d.layout_warning);
  // Another version of the same project keeps the data, both ways.
  d = decide_data("A", running(felucca), felucca17, none);
  CHECK(d.action == DataAction::Keep && !d.layout_warning);
  d = decide_data("A", running(felucca17), felucca, none);
  CHECK(d.action == DataAction::Keep && !d.layout_warning);
  d = decide_data("A", running(skiff13), skiff, none);
  CHECK(d.action == DataAction::Keep);
  // Skiff 2.0 saves in new formats: crossing it is a fresh start...
  d = decide_data("A", running(skiff), skiff2, none);
  CHECK(d.action == DataAction::FreshStart && d.layout_warning);
  // ...or the shelf's newest save from the other side of the break.
  d = decide_data("A", running(skiff), skiff2, {save_of("A", skiff2, 4), save_of("A", skiff, 5)});
  CHECK(d.action == DataAction::Restore && d.save_id == "A-s4");
  d = decide_data("A", running(skiff2), skiff, {save_of("A", skiff13, 2), save_of("A", skiff2, 6)});
  CHECK(d.action == DataAction::Restore && d.save_id == "A-s2");
  // Another project: its shelf's newest save of this unit comes back, whichever
  // version wrote it.
  std::vector<SaveState> saves = {save_of("A", skiff, 3), save_of("A", skiff13, 7), save_of("B", skiff, 9),
                                  save_of("A", felucca, 8), save_of("A", skiff2, 10)};
  d = decide_data("A", running(sloop), skiff, saves);
  CHECK(d.action == DataAction::Restore && d.save_id == "A-s7");
  // Felucca and Skiff are different projects: no data crosses at launch.
  d = decide_data("A", running(felucca), skiff, none);
  CHECK(d.action == DataAction::FreshStart && d.layout_warning);
  // Another unit's save is never picked: an empty shelf starts fresh.
  d = decide_data("C", running(sloop), skiff, saves);
  CHECK(d.action == DataAction::FreshStart && d.layout_warning);
  // Unknown current firmware: fresh start.
  d = decide_data("A", Firmware(), skiff, none);
  CHECK(d.action == DataAction::FreshStart && d.layout_warning);
  // bubba reports FM-1_003 but is a community firmware, not stock.
  PackageInfo bubba = info("bubba", "FM-1_003", 8);
  CHECK(bubba.family == Family::Felucca);
  d = decide_data("A", running(stock), bubba, none);
  CHECK(d.action == DataAction::FreshStart && !d.stock_unmapped);
  // Sideloaded: its own shelf (its app hash), so only itself keeps its data.
  PackageInfo side = info("sha:0909090909090909", "FM-1_916", 9);
  d = decide_data("A", running(felucca), side, none);
  CHECK(d.action == DataAction::FreshStart && d.layout_warning);
  d = decide_data("A", running(side), side, none);
  CHECK(d.action == DataAction::Keep);
  // Stock: data kept in place, flagged, never erased or restored.
  d = decide_data("A", running(skiff), stock, {save_of("A", stock, 1)});
  CHECK(d.action == DataAction::Keep && d.stock_unmapped && d.layout_warning);
}

// Projects of packages and saves recorded before projects existed.
static void test_projects() {
  CHECK(family_of_project("stock") == Family::Stock);
  CHECK(family_of_project("felucca") == Family::Felucca);
  CHECK(family_of_project("bubba") == Family::Felucca);
  TestStorage st;
  Library lib(st);
  PackageInfo skiff = load(lib, "skiff-1.4", "skiff-1.4.fwsc", "skiff");
  CHECK_EQ(skiff.project, std::string("skiff"));
  // No project given: sideloaded, its shelf keyed by its app hash.
  PackageInfo sloop = load(lib, "sloop-2.8.1", "sloop-2.8.1.fwsc");
  CHECK_EQ(sloop.project, "sha:" + hex(sloop.app_sha256).substr(0, 16));
  // A save written without a project gets it from the package with its app.
  Unit u;
  u.id = "u1";
  CHECK(lib.put_unit(u));
  SaveState s;
  s.unit = "u1";
  s.identity = skiff.identity;
  s.app_sha256 = skiff.app_sha256;
  s.family = skiff.family;
  CHECK(lib.add_save(u, s));
  std::vector<SaveState> saves = lib.saves_of("u1");
  CHECK(saves.size() == 1 && saves[0].project == "skiff");
  // ...and from its app hash when no stored package has that app.
  SaveState t = s;
  t.app_sha256.fill(0x42);
  CHECK(lib.add_save(u, t));
  saves = lib.saves_of("u1");
  CHECK(saves.size() == 2 && saves[1].project == "sha:" + hex(t.app_sha256).substr(0, 16));
  // Packages stored before projects (the board's store): stock stays stock, the
  // rest are sideloaded until store-add records their project.
  for (const char *id : {"old-stock", "old-skiff"}) {
    const bool is_stock = std::string(id) == "old-stock";
    std::vector<uint8_t> data = read_file(is_stock ? "tests/fixtures/FM-1_015.fwsc" : "tests/fixtures/skiff-1.4.fwsc");
    CHECK(lib.add_package(id, data.data(), data.size(), nullptr, "", "", is_stock ? "stock" : "skiff") ==
          PackageError::None);
    std::vector<uint8_t> raw;
    CHECK(st.read(std::string("m/pkg/") + id, raw));
    std::string text(raw.begin(), raw.end()), old;
    for (const std::string &line : lines_of(text))
      if (line.compare(0, 8, "project ") != 0) old += line + "\n";
    CHECK(st.write(std::string("m/pkg/") + id, reinterpret_cast<const uint8_t *>(old.data()), old.size()));
    PackageInfo p;
    CHECK(lib.package(id, p));
    if (is_stock) CHECK(p.project == "stock" && p.family == Family::Stock);
    else CHECK(p.project == sideload_project(p.app_sha256) && p.family == Family::Felucca);
  }
  // The running firmware's project comes from the stored package it matches.
  CHECK_EQ(lib.identify_app(skiff.app).project, std::string("skiff"));
  CHECK(lib.identify_app(std::vector<Hash>(kAppSectors, Hash{})).project.empty());
}

static void test_save_restore_round_trip() {
  TestStorage st;
  Library lib(st);
  PackageInfo skiff = load(lib, "skiff-1.4", "skiff-1.4.fwsc");
  PackageInfo sloop = load(lib, "sloop-2.8.1", "sloop-2.8.1.fwsc");
  Image img = unit_image(9);
  put_app(lib, img, skiff);
  fill(img, 0x97000, 0x3000, 11);  // songs
  fill(img, 0xDC000, kSector, 12);  // presets
  fill(img, 0xFD000, kSector, 13);  // globals
  fill(img, kStagingStart, kSector, 14);
  const Image saved_state = img;
  Unit u;
  u.id = unit_id_of(img.data() + kUnitSector);
  SaveState s = capture_save(u.id, lib.identify_app(skiff.app), store_image(lib, img));
  CHECK(lib.add_save(u, s));

  // SLOOP runs a while: other data, including sectors blank in the save.
  put_app(lib, img, sloop);
  fill(img, 0x97000, kSector, 21);
  fill(img, 0xA0000, 0x2000, 22);  // sample slots, blank in the save
  fill(img, 0xE5000, kSector, 23);  // autosave, blank in the save
  fill(img, kStagingStart, kSector, 24);  // staging: never restored
  fill(img, 0x93000, kSector, 25);  // outside the Felucca table: untouched

  SaveState back;
  CHECK(lib.save(s.id, back));
  std::vector<SectorOp> ops;
  CHECK(plan_restore(back, u.id, skiff, store_image(lib, img), ops));
  apply(lib, img, ops);
  put_app(lib, img, skiff);
  const FamilyInfo &f = family_info(Family::Felucca);
  for (size_t r = 0; r < f.save_range_count; ++r)
    CHECK(memcmp(img.data() + f.save_ranges[r].start, saved_state.data() + f.save_ranges[r].start,
                 f.save_ranges[r].end - f.save_ranges[r].start) == 0);
  CHECK(img[0xA0000] == 0xFF && img[0xE5000] == 0xFF);
  CHECK(img[kStagingStart] == uint8_t(24));
  CHECK(img[0x93000] == uint8_t(25));
  for (const SectorOp &op : ops) CHECK(!(op.addr >= kStagingStart && op.addr < kStagingEnd));

  // Refused: another unit, another identity, another version.
  CHECK(!plan_restore(back, "0000000000000000", skiff, store_image(lib, img), ops));
  CHECK(!plan_restore(back, u.id, sloop, store_image(lib, img), ops));
  PackageInfo skiff_other = skiff;
  skiff_other.app_sha256[0] ^= 1;
  CHECK(!plan_restore(back, u.id, skiff_other, store_image(lib, img), ops));
  // A save naming a sector outside the family's ranges is refused outright.
  SaveState evil = back;
  evil.sectors.emplace_back(kStagingStart, evil.sectors[0].second);
  CHECK(!plan_restore(evil, u.id, skiff, store_image(lib, img), ops));
}

static void test_fresh_start() {
  TestStorage st;
  Library lib(st);
  Image img = unit_image(9);
  fill(img, 0x97000, kSector, 1);
  fill(img, 0xFC000, kSector, 2);
  fill(img, kStagingStart, kSector, 3);
  fill(img, 0x93000, kSector, 4);
  std::vector<SectorOp> ops = plan_fresh(Family::Felucca, store_image(lib, img));
  CHECK_EQ(ops.size(), size_t(2));
  apply(lib, img, ops);
  CHECK(img[0x97000] == 0xFF && img[0xFC000] == 0xFF);
  CHECK(img[kStagingStart] == 3 && img[0x93000] == 4);
  CHECK(plan_fresh(Family::Stock, store_image(lib, img)).empty());  // never erases 0x93000
}

static void test_two_units() {
  TestStorage st;
  Library lib(st);
  Image a = unit_image(1), b = unit_image(2);
  Unit ua, ub;
  ua.id = unit_id_of(a.data() + kUnitSector);
  ub.id = unit_id_of(b.data() + kUnitSector);
  CHECK(ua.id != ub.id);
  ua.name = "A";
  ub.name = "B\nsticker";
  std::string ba, bb;
  CHECK(lib.add_backup(ua, store_image(lib, a), ba));
  CHECK(lib.add_backup(ub, store_image(lib, b), bb));
  Firmware fw;
  fw.identity = "FM-1_714";
  fw.family = Family::Felucca;
  fill(a, 0x97000, kSector, 5);
  SaveState sa = capture_save(ua.id, fw, store_image(lib, a));
  CHECK(lib.add_save(ua, sa));
  CHECK_EQ(lib.saves_of(ua.id).size(), size_t(1));
  CHECK(lib.saves_of(ub.id).empty());
  Unit back;
  CHECK(lib.unit(ub.id, back) && back.name == "B sticker" && back.baseline_backup == bb);
  CHECK(lib.unit(ua.id, back) && back.name == "A" && back.baseline_backup == ba);
  CHECK_EQ(lib.unit_ids().size(), size_t(2));
}

static void test_install_preview() {
  TestStorage st;
  Library lib(st);
  PackageInfo skiff = load(lib, "skiff-1.4", "skiff-1.4.fwsc");
  load(lib, "sloop-2.8.1", "sloop-2.8.1.fwsc");
  CHECK(!preview_install(lib, "", "skiff-1.4").ok);  // no unit seen yet
  Image img = unit_image(9);
  put_app(lib, img, skiff);
  fill(img, 0x97000, kSector, 1);  // a Skiff project
  Unit u;
  u.id = unit_id_of(img.data() + kUnitSector);
  std::string b;
  CHECK(lib.add_backup(u, store_image(lib, img), b));
  CHECK_EQ(lib.last_unit(), u.id);

  InstallPreview p = preview_install(lib, u.id, "skiff-1.4");  // what it runs: nothing to do
  CHECK(p.ok && p.app_sectors == 0 && p.data_sectors == 0);
  CHECK(p.current.identity == "FM-1_714" && p.decision.action == DataAction::Keep);

  p = preview_install(lib, u.id, "sloop-2.8.1");  // another firmware: fresh start
  CHECK(p.ok && p.app_sectors > 100 && p.decision.action == DataAction::FreshStart);
  CHECK_EQ(p.data_sectors, uint32_t(1));  // the project sector gets erased
  CHECK(p.decision.layout_warning);

  // With a Skiff save on record, coming back from SLOOP restores it.
  SaveState s = capture_save(u.id, lib.identify_app(skiff.app), store_image(lib, img));
  CHECK(lib.add_save(u, s));
  Image on_sloop = img;
  PackageInfo sloop;
  CHECK(lib.package("sloop-2.8.1", sloop));
  put_app(lib, on_sloop, sloop);
  memset(on_sloop.data() + 0x97000, 0xFF, kSector);
  CHECK(lib.add_backup(u, store_image(lib, on_sloop), b));
  p = preview_install(lib, u.id, "skiff-1.4");
  CHECK(p.ok && p.decision.action == DataAction::Restore && p.data_sectors == 1);

  CHECK(!preview_install(lib, u.id, "nope").ok);
  CHECK(!preview_install(lib, "0000000000000000", "skiff-1.4").ok);
  // Pins.
  CHECK(lib.set_pinned("skiff-1.4", true));
  PackageInfo got;
  CHECK(lib.package("skiff-1.4", got) && got.pinned);
  CHECK(lib.set_pinned("skiff-1.4", false) && lib.package("skiff-1.4", got) && !got.pinned);
  CHECK(!lib.set_pinned("nope", true));
}

int main() {
  test_sector_store();
  test_manifest();
  test_packages();
  test_unit_ids();
  test_remove_package();
  test_install_preview();
  test_backups();
  test_diff_planner();
  test_capture_save();
  test_decide_data();
  test_projects();
  test_save_restore_round_trip();
  test_fresh_start();
  test_two_units();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
#ifdef TEST_POSIX_STORAGE
  printf("all fm1_loader library tests passed (PosixStorage)\n");
#else
  printf("all fm1_loader library tests passed\n");
#endif
  return 0;
}
