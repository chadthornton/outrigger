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

PackageError Library::add_package(const std::string &id, const uint8_t *data, size_t len,
                                  const Hash *expected_sha256, const std::string &name,
                                  const std::string &version) {
  Package p;
  PackageError err = parse_package(data, len, p, expected_sha256);
  if (err != PackageError::None) return err;
  if (!key_safe(id)) return PackageError::BadId;
  Manifest m;
  m.fields["identity"] = p.identity;
  m.fields["family"] = family_name(p.family);
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
  out.family = parse_family(m.get("family"));
  if (!parse_hash(m.get("file"), out.file_sha256) || !parse_hash(m.get("app"), out.app_sha256))
    return false;
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
  for (const std::string &id : package_ids()) {
    PackageInfo p;
    if (package(id, p) && p.app == app) {
      fw.identity = p.identity;
      fw.app_sha256 = p.app_sha256;
      fw.family = p.family;
      return fw;
    }
  }
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
  p.app_sectors = uint32_t(plan_app(flash, target).size());
  std::vector<SectorOp> data;
  if (p.decision.action == DataAction::Restore) {
    SaveState s;
    if (!lib.save(p.decision.save_id, s) || !plan_restore(s, unit_id, target, flash, data)) {
      p.problem = "save state unusable";
      return p;
    }
  } else if (p.decision.action == DataAction::FreshStart) {
    data = plan_fresh(target.family, flash);
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
  return true;
}

std::vector<SaveState> Library::saves_of(const std::string &unit_id) {
  std::vector<SaveState> out;
  for (const std::string &k : storage_.list("m/save/" + unit_id + "-")) {
    SaveState s;
    if (save(k.substr(7), s) && s.unit == unit_id) out.push_back(s);
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
  if (current.known() && current.identity == target.identity &&
      current.app_sha256 == target.app_sha256)
    return d;
  for (auto it = saves.rbegin(); it != saves.rend(); ++it) {
    if (it->unit == unit_id && it->identity == target.identity &&
        it->app_sha256 == target.app_sha256 && family_info(target.family).saves_enabled) {
      d.action = DataAction::Restore;
      d.save_id = it->id;
      return d;
    }
  }
  if (current.known() && current.identity == target.identity) return d;  // an update
  d.layout_warning = true;
  if (!family_info(target.family).saves_enabled) {
    d.stock_unmapped = true;
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

void plan_ranges(Family family, const FlashHashes &flash,
                 const std::vector<std::pair<uint32_t, Hash>> &want, std::vector<SectorOp> &ops) {
  const FamilyInfo &f = family_info(family);
  for (size_t r = 0; r < f.save_range_count; ++r) {
    for (uint32_t a = f.save_ranges[r].start; a < f.save_ranges[r].end; a += kSector) {
      Hash h = SectorStore::blank();
      for (const auto &w : want)
        if (w.first == a) h = w.second;
      if (flash[a / kSector] != h) ops.push_back({a, h, Right::SaveRestore, family});
    }
  }
}

}  // namespace

bool plan_restore(const SaveState &s, const std::string &unit_id, const PackageInfo &target,
                  const FlashHashes &flash, std::vector<SectorOp> &ops) {
  ops.clear();
  if (s.unit != unit_id || s.identity != target.identity || s.app_sha256 != target.app_sha256 ||
      s.family != target.family || !family_info(s.family).saves_enabled ||
      flash.size() != kSectors)
    return false;
  const FamilyInfo &f = family_info(s.family);
  for (const auto &sec : s.sectors)
    if (sec.first % kSector || !inside(sec.first, kSector, f.save_ranges, f.save_range_count))
      return false;
  plan_ranges(s.family, flash, s.sectors, ops);
  return true;
}

std::vector<SectorOp> plan_fresh(Family family, const FlashHashes &flash) {
  std::vector<SectorOp> ops;
  if (family_info(family).saves_enabled && flash.size() == kSectors)
    plan_ranges(family, flash, {}, ops);
  return ops;
}

}  // namespace fm1_loader
