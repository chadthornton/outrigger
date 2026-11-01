#include "library.h"

#include <stdlib.h>

#include <algorithm>

namespace fm1_loader {

namespace {

const char *family_name(Family f) { return family_info(f).name; }

Family parse_family(const std::string &s) {
  return s == "stock" ? Family::Stock : Family::Felucca;
}

const char *action_name(DataAction a) {
  return a == DataAction::Restore ? "restore" : a == DataAction::FreshStart ? "fresh" : "keep";
}

DataAction parse_action(const std::string &s) {
  return s == "restore" ? DataAction::Restore : s == "fresh" ? DataAction::FreshStart
                                                              : DataAction::Keep;
}

// Field values are one line of plain text.
std::string clean(const std::string &s) {
  std::string out;
  for (char c : s) out += (c == '\n' || c == '\r') ? ' ' : c;
  return out;
}

bool key_safe(const std::string &id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.'))
      return false;
  return true;
}

}  // namespace

std::string unit_id_of(const uint8_t *sector) {
  bool all_ff = true, all_00 = true;
  for (uint32_t i = 0; i < kSector; ++i) {
    all_ff &= sector[i] == 0xFF;
    all_00 &= sector[i] == 0x00;
  }
  if (all_ff || all_00) return std::string();
  Hash h = sha256(sector, kSector);
  return hex(h.data(), 8);
}

bool Library::put_manifest(const std::string &key, const Manifest &m) {
  std::string text = m.serialize();
  return storage_.write(key, reinterpret_cast<const uint8_t *>(text.data()), text.size()) &&
         storage_.sync();
}

bool Library::get_manifest(const std::string &key, Manifest &m) {
  std::vector<uint8_t> data;
  if (!storage_.read(key, data)) return false;
  return Manifest::parse(std::string(data.begin(), data.end()), m);
}

std::string sideload_project(const Hash &app_sha256) { return "sha:" + hex(app_sha256).substr(0, 16); }

Family family_of_project(const std::string &project, const std::string &identity) {
  if (project == "stock") return Family::Stock;
  // A sideloaded M-VAVE build (FM-1_0NN, not the catalog's V15): stock's data map
  // isn't known, so its data stays in place as it always has.
  if (project.compare(0, 4, "sha:") == 0 && identity.size() == 8 && identity.compare(0, 6, "FM-1_0") == 0)
    return Family::Stock;
  return Family::Felucca;
}

namespace {
// Data-format breaks: from this identity number on, a project's saves are a new era.
struct FormatBreak {
  const char *project;
  unsigned long from;
};
constexpr FormatBreak kBreaks[] = {
    {"skiff", 720},  // Skiff 2.0 "saves in new formats" (its release notes)
};
}  // namespace

namespace {
// Firmware pairs proven on hardware, either direction. Matched by project and
// exact identity: another version is another test.
struct TestedPair {
  const char *a_project, *a_identity, *b_project, *b_identity;
  bool tested;  // false: owed, the pair switches by clean slate until it passes
};
constexpr TestedPair kTestedPairs[] = {
    {"felucca", "FM-1_916", "skiff", "FM-1_714", true},   // Felucca 1.6.1 <-> Skiff 1.4, checkpoint 3
    {"felucca", "FM-1_918", "skiff", "FM-1_721", false},  // Felucca 1.8.0 <-> Skiff 2.1.1, bench test 2
    {"stock", "FM-1_015", "felucca", "FM-1_918", false},  // V15 <-> Felucca 1.8.0, bench test 2
};

// Projects the loader can't take a unit to yet: they write 0xE9000, the sector
// whose hash is the unit ID (fw-map-bench.md finding 2).
constexpr const char *kBlocked[] = {"melodee", "tessera"};
constexpr const char *kBlockedReason = "Not yet: it writes the FM-1's Bluetooth sector";
}  // namespace

bool tested_pair(const std::string &from_project, const std::string &from_identity,
                 const std::string &to_project, const std::string &to_identity) {
  for (const TestedPair &p : kTestedPairs) {
    if (!p.tested) continue;
    if ((from_project == p.a_project && from_identity == p.a_identity && to_project == p.b_project &&
         to_identity == p.b_identity) ||
        (from_project == p.b_project && from_identity == p.b_identity && to_project == p.a_project &&
         to_identity == p.a_identity))
      return true;
  }
  return false;
}

int data_era(const std::string &project, const std::string &identity) {
  if (identity.size() <= 5) return 0;
  const unsigned long n = strtoul(identity.c_str() + 5, nullptr, 10);
  int era = 0;
  for (const FormatBreak &b : kBreaks)
    if (project == b.project) era = n >= b.from ? 2 : 1;
  return era;
}

PackageError Library::add_package(const std::string &id, const uint8_t *data, size_t len,
                                  const Hash *expected_sha256, const std::string &name,
                                  const std::string &version, const std::string &project) {
  Package p;
  PackageError err = parse_package(data, len, p, expected_sha256);
  if (err != PackageError::None) return err;
  if (!key_safe(id) || (!project.empty() && !key_safe(project))) return PackageError::BadId;
  const std::string proj = project.empty() ? sideload_project(p.app_sha256) : project;
  Manifest m;
  m.fields["identity"] = p.identity;
  m.fields["project"] = proj;
  m.fields["family"] = family_name(family_of_project(proj, p.identity));
  m.fields["file"] = hex(p.file_sha256);
  m.fields["app"] = hex(p.app_sha256);
  if (!name.empty()) m.fields["name"] = clean(name);
  if (!version.empty()) m.fields["version"] = clean(version);
  for (uint32_t i = 0; i < kAppSectors; ++i) {
    Hash h;
    if (!sectors_.put(p.app.data() + i * kSector, h)) return PackageError::StoreFailed;
    m.sectors.emplace_back(kAppStart + i * kSector, h);
  }
  if (!put_manifest("m/pkg/" + id, m)) return PackageError::StoreFailed;
  return PackageError::None;
}

bool Library::package(const std::string &id, PackageInfo &out) {
  Manifest m;
  if (!get_manifest("m/pkg/" + id, m) || m.sectors.size() != kAppSectors) return false;
  out = PackageInfo();
  out.id = id;
  out.identity = m.get("identity");
  out.name = m.get("name");
  out.version = m.get("version");
  out.pinned = m.get("pin") == "1";
  if (!parse_hash(m.get("file"), out.file_sha256) || !parse_hash(m.get("app"), out.app_sha256))
    return false;
  // Stored before projects: stock stays stock, the rest are sideloaded until
  // store-add records their catalog project.
  out.project = m.get("project");
  if (out.project.empty())
    out.project = parse_family(m.get("family")) == Family::Stock ? "stock" : sideload_project(out.app_sha256);
  out.family = family_of_project(out.project, out.identity);
  for (uint32_t i = 0; i < kAppSectors; ++i) {
    if (m.sectors[i].first != kAppStart + i * kSector) return false;
    out.app.push_back(m.sectors[i].second);
  }
  return true;
}

std::vector<std::string> Library::package_ids() {
  std::vector<std::string> out;
  for (const std::string &k : storage_.list("m/pkg/")) out.push_back(k.substr(6));
  return out;
}

bool Library::remove_package(const std::string &id) {
  // A package a unit runs, or one an unfinished install needs, stays.
  for (const std::string &uid : unit_ids()) {
    Unit u;
    if (!unit(uid, u)) return false;
    if (u.installed_package == id || (u.has_pending && u.pending.package_id == id)) return false;
  }
  return storage_.remove("m/pkg/" + id) && storage_.sync();
}

bool Library::set_pinned(const std::string &id, bool pinned) {
  Manifest m;
  if (!get_manifest("m/pkg/" + id, m)) return false;
  if (pinned) m.fields["pin"] = "1";
  else m.fields.erase("pin");
  return put_manifest("m/pkg/" + id, m);
}

Firmware Library::identify_app(const std::vector<Hash> &app) {
  Firmware fw;
  // A catalog project wins over a sideloaded copy of the same app (one stored
  // before projects, say).
  for (const std::string &id : package_ids()) {
    PackageInfo p;
    if (package(id, p) && p.app == app && (fw.project.empty() || fw.project.compare(0, 4, "sha:") == 0)) {
      fw.identity = p.identity;
      fw.project = p.project;
      fw.app_sha256 = p.app_sha256;
      fw.family = p.family;
    }
  }
  if (!fw.project.empty()) return fw;
  // Unknown firmware: still worth its app hash, for the record.
  Sha256 s;
  std::vector<uint8_t> buf(kSector);
  for (const Hash &h : app) {
    if (!sectors_.get(h, buf.data())) return fw;
    s.update(buf.data(), buf.size());
  }
  fw.app_sha256 = s.finish();
  return fw;
}

bool Library::unit(const std::string &id, Unit &out) {
  Manifest m;
  if (!get_manifest("m/unit/" + id, m)) return false;
  out = Unit();
  out.id = id;
  out.name = m.get("name");
  out.baseline_backup = m.get("baseline");
  out.latest_backup = m.get("latest");
  out.installed_package = m.get("installed");
  out.next_seq = uint32_t(strtoul(m.get("seq").c_str(), nullptr, 10));
  out.has_pending = !m.get("pending").empty();
  if (out.has_pending) {
    out.pending.package_id = m.get("pending");
    out.pending.action = parse_action(m.get("pending_action"));
    out.pending.save_id = m.get("pending_save");
    out.pending.clean_slate = m.get("pending_clean") == "1";
    out.pending.before_backup = m.get("pending_backup");
  }
  return true;
}

bool Library::put_unit(const Unit &u) {
  if (!key_safe(u.id)) return false;
  Manifest m;
  m.fields["name"] = clean(u.name);
  m.fields["baseline"] = u.baseline_backup;
  m.fields["latest"] = u.latest_backup;
  m.fields["installed"] = u.installed_package;
  m.fields["seq"] = std::to_string(u.next_seq);
  if (u.has_pending) {
    m.fields["pending"] = u.pending.package_id;
    m.fields["pending_action"] = action_name(u.pending.action);
    m.fields["pending_save"] = u.pending.save_id;
    if (u.pending.clean_slate) m.fields["pending_clean"] = "1";
    m.fields["pending_backup"] = u.pending.before_backup;
  }
  // Empty values would serialize as "key " and parse back as empty: drop them.
  for (auto it = m.fields.begin(); it != m.fields.end();)
    it = it->second.empty() ? m.fields.erase(it) : std::next(it);
  return put_manifest("m/unit/" + u.id, m);
}

std::vector<std::string> Library::unit_ids() {
  std::vector<std::string> out;
  for (const std::string &k : storage_.list("m/unit/")) out.push_back(k.substr(7));
  return out;
}

bool Library::add_backup(Unit &u, const FlashHashes &flash, std::string &backup_id) {
  if (flash.size() != kSectors) return false;
  for (const Hash &h : flash)
    if (!sectors_.has(h)) return false;
  Manifest m;
  m.fields["unit"] = u.id;
  for (uint32_t i = 0; i < kSectors; ++i) m.sectors.emplace_back(i * kSector, flash[i]);
  backup_id = u.id + "-b" + std::to_string(u.next_seq);
  if (!put_manifest("m/backup/" + backup_id, m)) return false;
  Unit next = u;
  ++next.next_seq;
  std::string old_latest = next.latest_backup;
  if (next.baseline_backup.empty()) next.baseline_backup = backup_id;
  next.latest_backup = backup_id;
  if (!put_unit(next)) return false;
  u = next;
  const std::string who = u.id;
  storage_.write("m/last", reinterpret_cast<const uint8_t *>(who.data()), who.size());
  // The previous latest goes once the new one is durable (the baseline stays).
  if (!old_latest.empty() && old_latest != u.baseline_backup && old_latest != backup_id &&
      !(u.has_pending && old_latest == u.pending.before_backup))
    storage_.remove("m/backup/" + old_latest);
  return true;
}

bool Library::backup(const std::string &id, FlashHashes &out) {
  Manifest m;
  if (!get_manifest("m/backup/" + id, m) || m.sectors.size() != kSectors) return false;
  out.clear();
  for (uint32_t i = 0; i < kSectors; ++i) {
    if (m.sectors[i].first != i * kSector) return false;
    out.push_back(m.sectors[i].second);
  }
  return true;
}

bool Library::region_sha256(const std::string &backup_id, uint32_t start, uint32_t end, Hash &out) {
  FlashHashes flash;
  if (start % kSector || end % kSector || start >= end || end > kFlashSize || !backup(backup_id, flash))
    return false;
  Sha256 s;
  std::vector<uint8_t> buf(kSector);
  for (uint32_t a = start; a < end; a += kSector) {
    if (!sectors_.get(flash[a / kSector], buf.data())) return false;
    s.update(buf.data(), buf.size());
  }
  out = s.finish();
  return true;
}

std::string Library::last_unit() {
  std::vector<uint8_t> data;
  return storage_.read("m/last", data) ? std::string(data.begin(), data.end()) : std::string();
}

InstallPreview preview_install(Library &lib, const std::string &unit_id, const std::string &package_id) {
  InstallPreview p;
  Unit u;
  FlashHashes flash;
  PackageInfo target;
  if (unit_id.empty() || !lib.unit(unit_id, u) || !lib.backup(u.latest_backup, flash)) {
    p.problem = "back up this FM-1 first";
    return p;
  }
  if (!lib.package(package_id, target)) {
    p.problem = "package missing";
    return p;
  }
  std::vector<Hash> app(flash.begin() + kAppStart / kSector, flash.begin() + kAppEnd / kSector);
  p.current = lib.identify_app(app);
  p.decision = decide_data(unit_id, p.current, target, lib.saves_of(unit_id));
  if (p.decision.blocked) {
    p.problem = p.decision.blocked;
    return p;
  }
  p.app_sectors = uint32_t(plan_app(flash, target).size());
  std::vector<SectorOp> data;
  if (!plan_data(lib, unit_id, p.decision, target, flash, data)) {
    p.problem = "save state unusable";
    return p;
  }
  p.data_sectors = uint32_t(data.size());
  p.ok = true;
  return p;
}

bool Library::add_save(Unit &u, SaveState &s) {
  for (const auto &sec : s.sectors)
    if (!sectors_.has(sec.second)) return false;
  s.unit = u.id;
  s.seq = u.next_seq;
  s.id = u.id + "-s" + std::to_string(s.seq);
  Manifest m;
  m.fields["unit"] = s.unit;
  if (!s.project.empty()) m.fields["project"] = s.project;
  m.fields["identity"] = s.identity;
  m.fields["app"] = hex(s.app_sha256);
  m.fields["family"] = family_name(s.family);
  m.fields["label"] = clean(s.label.empty() ? "autosave" : s.label);
  m.fields["seq"] = std::to_string(s.seq);
  m.sectors = s.sectors;
  if (!put_manifest("m/save/" + s.id, m)) return false;
  Unit next = u;
  ++next.next_seq;
  if (!put_unit(next)) return false;
  u = next;
  return true;
}

bool Library::save(const std::string &id, SaveState &out) {
  std::map<std::string, std::string> app_projects;
  return read_save(id, out, app_projects);
}

// A save recorded before projects is on the shelf of the stored package with its
// app; app_projects caches that lookup across the saves of one listing.
bool Library::read_save(const std::string &id, SaveState &out, std::map<std::string, std::string> &app_projects) {
  Manifest m;
  if (!get_manifest("m/save/" + id, m)) return false;
  out = SaveState();
  out.id = id;
  out.unit = m.get("unit");
  out.identity = m.get("identity");
  out.family = parse_family(m.get("family"));
  out.label = m.get("label");
  out.seq = uint32_t(strtoul(m.get("seq").c_str(), nullptr, 10));
  if (!parse_hash(m.get("app"), out.app_sha256)) return false;
  out.sectors = m.sectors;
  // Recorded before projects: the shelf of the stored package with its app.
  out.project = m.get("project");
  if (out.project.empty()) {
    if (app_projects.empty()) {
      app_projects[""] = "";  // built, even when the store has no packages
      for (const std::string &pid : package_ids()) {
        PackageInfo p;
        if (!package(pid, p)) continue;
        std::string &known = app_projects[hex(p.app_sha256)];
        if (known.empty() || known.compare(0, 4, "sha:") == 0) known = p.project;  // a catalog project wins
      }
    }
    auto it = app_projects.find(hex(out.app_sha256));
    out.project = it != app_projects.end() ? it->second : sideload_project(out.app_sha256);
  }
  return true;
}

std::vector<SaveState> Library::saves_of(const std::string &unit_id) {
  std::vector<SaveState> out;
  std::map<std::string, std::string> app_projects;
  for (const std::string &k : storage_.list("m/save/" + unit_id + "-")) {
    SaveState s;
    if (read_save(k.substr(7), s, app_projects) && s.unit == unit_id) out.push_back(s);
  }
  std::sort(out.begin(), out.end(),
            [](const SaveState &a, const SaveState &b) { return a.seq < b.seq; });
  return out;
}

size_t Library::collect_garbage() {
  std::set<Hash> used;
  for (const char *prefix : {"m/pkg/", "m/backup/", "m/save/"}) {
    for (const std::string &k : storage_.list(prefix)) {
      Manifest m;
      // An unreadable manifest keeps everything: never delete what might be in use.
      if (!get_manifest(k, m)) return 0;
      for (const auto &s : m.sectors) used.insert(s.second);
    }
  }
  size_t n = sectors_.collect_garbage(used);
  storage_.sync();
  return n;
}

SaveState capture_save(const std::string &unit_id, const Firmware &fw, const FlashHashes &flash) {
  SaveState s;
  s.unit = unit_id;
  s.project = fw.project;
  s.identity = fw.identity;
  s.app_sha256 = fw.app_sha256;
  s.family = fw.family;
  const FamilyInfo &f = family_info(fw.family);
  if (!f.saves_enabled || flash.size() != kSectors) return s;
  for (size_t r = 0; r < f.save_range_count; ++r)
    for (uint32_t a = f.save_ranges[r].start; a < f.save_ranges[r].end; a += kSector)
      if (flash[a / kSector] != SectorStore::blank()) s.sectors.emplace_back(a, flash[a / kSector]);
  return s;
}

DataDecision decide_data(const std::string &unit_id, const Firmware &current,
                         const PackageInfo &target, const std::vector<SaveState> &saves) {
  DataDecision d;
  for (const char *p : kBlocked)
    if (target.project == p) {
      d.blocked = kBlockedReason;
      return d;
    }
  if (current.known() && current.app_sha256 == target.app_sha256) return d;  // no-op
  const int era = data_era(target.project, target.identity);
  if (!current.project.empty() && current.project == target.project &&
      data_era(current.project, current.identity) == era)
    return d;  // another version of the same project: the autosave is the rollback
  d.clean_slate = !tested_pair(current.project, current.identity, target.project, target.identity);
  if (target.project == "stock") {
    // Skiff 2.x on this unit, now or earlier (data era 2 is Skiff 2.0 and up).
    bool skiff2 = current.project == "skiff" && data_era(current.project, current.identity) == 2;
    for (const SaveState &s : saves)
      skiff2 |= s.unit == unit_id && s.project == "skiff" && data_era(s.project, s.identity) == 2;
    d.stock_resources_warning = skiff2;
  }
  if (family_info(target.family).saves_enabled) {
    for (auto it = saves.rbegin(); it != saves.rend(); ++it) {
      if (it->unit == unit_id && it->project == target.project && data_era(it->project, it->identity) == era) {
        d.action = DataAction::Restore;
        d.save_id = it->id;
        return d;
      }
    }
  }
  d.layout_warning = true;
  if (!family_info(target.family).saves_enabled && !d.clean_slate) {
    d.stock_unmapped = true;  // a tested pair to stock: its data stays in place
    return d;
  }
  d.action = DataAction::FreshStart;
  return d;
}

std::vector<SectorOp> plan_app(const FlashHashes &flash, const PackageInfo &target) {
  std::vector<SectorOp> ops;
  if (flash.size() != kSectors || target.app.size() != kAppSectors) return ops;
  for (uint32_t i = 0; i < kAppSectors; ++i) {
    uint32_t addr = kAppStart + i * kSector;
    if (flash[addr / kSector] != target.app[i])
      ops.push_back({addr, target.app[i], Right::App, target.family});
  }
  return ops;
}

namespace {

// Every sector of the ranges that differs from want (blank where want has none).
void plan_ranges(const Range *ranges, size_t count, Right right, Family family, const FlashHashes &flash,
                 const std::vector<std::pair<uint32_t, Hash>> &want, std::vector<SectorOp> &ops) {
  for (size_t r = 0; r < count; ++r) {
    for (uint32_t a = ranges[r].start; a < ranges[r].end; a += kSector) {
      Hash h = SectorStore::blank();
      for (const auto &w : want)
        if (w.first == a) h = w.second;
      if (flash[a / kSector] != h) ops.push_back({a, h, right, family});
    }
  }
}

void plan_family(Family family, const FlashHashes &flash, const std::vector<std::pair<uint32_t, Hash>> &want,
                 std::vector<SectorOp> &ops) {
  const FamilyInfo &f = family_info(family);
  plan_ranges(f.save_ranges, f.save_range_count, Right::SaveRestore, family, flash, want, ops);
}

}  // namespace

namespace {

// A save the target may get back: this unit's, the target project's shelf and
// data era (decide_data), any version that wrote it, every sector in its ranges.
bool save_fits(const SaveState &s, const std::string &unit_id, const PackageInfo &target) {
  if (s.unit != unit_id || s.project != target.project ||
      data_era(s.project, s.identity) != data_era(target.project, target.identity) ||
      s.family != target.family || !family_info(s.family).saves_enabled)
    return false;
  const FamilyInfo &f = family_info(s.family);
  for (const auto &sec : s.sectors)
    if (sec.first % kSector || !inside(sec.first, kSector, f.save_ranges, f.save_range_count))
      return false;
  return true;
}

}  // namespace

bool plan_restore(const SaveState &s, const std::string &unit_id, const PackageInfo &target,
                  const FlashHashes &flash, std::vector<SectorOp> &ops) {
  ops.clear();
  if (!save_fits(s, unit_id, target) || flash.size() != kSectors) return false;
  plan_family(s.family, flash, s.sectors, ops);
  return true;
}

bool needs_erase_ok(const DataDecision &d) { return d.action == DataAction::FreshStart || d.clean_slate; }

bool plan_data(Library &lib, const std::string &unit_id, const DataDecision &d, const PackageInfo &target,
               const FlashHashes &flash, std::vector<SectorOp> &ops) {
  ops.clear();
  if (d.action == DataAction::Restore) {
    SaveState s;
    if (!lib.save(d.save_id, s)) return false;
    return d.clean_slate ? plan_clean(&s, unit_id, target, flash, ops)
                         : plan_restore(s, unit_id, target, flash, ops);
  }
  if (d.action == DataAction::FreshStart) {
    if (d.clean_slate) return plan_clean(nullptr, unit_id, target, flash, ops);
    ops = plan_fresh(target.family, flash);
  }
  return true;
}

bool plan_clean(const SaveState *s, const std::string &unit_id, const PackageInfo &target,
                const FlashHashes &flash, std::vector<SectorOp> &ops) {
  ops.clear();
  if ((s && !save_fits(*s, unit_id, target)) || flash.size() != kSectors) return false;
  plan_ranges(kCleanSlate, kCleanSlateCount, Right::CleanSlate, target.family, flash,
              s ? s->sectors : std::vector<std::pair<uint32_t, Hash>>(), ops);
  return true;
}

std::vector<SectorOp> plan_fresh(Family family, const FlashHashes &flash) {
  std::vector<SectorOp> ops;
  if (family_info(family).saves_enabled && flash.size() == kSectors)
    plan_family(family, flash, {}, ops);
  return ops;
}

}  // namespace fm1_loader
