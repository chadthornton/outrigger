#include "store.h"

#include <stdio.h>
#include <stdlib.h>

#include "layout.h"

namespace fm1_loader {

bool MemoryStorage::read(const std::string &key, std::vector<uint8_t> &out) {
  auto it = current_.find(key);
  if (it == current_.end()) return false;
  out = it->second;
  return true;
}

bool MemoryStorage::write(const std::string &key, const uint8_t *data, size_t len) {
  if (fail_writes_after == 0) return false;
  if (fail_writes_after > 0) --fail_writes_after;
  ++writes;
  current_[key].assign(data, data + len);
  return true;
}

bool MemoryStorage::remove(const std::string &key) { return current_.erase(key) > 0; }

bool MemoryStorage::exists(const std::string &key) { return current_.count(key) > 0; }

std::vector<std::string> MemoryStorage::list(const std::string &prefix) {
  std::vector<std::string> out;
  for (auto it = current_.lower_bound(prefix);
       it != current_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
    out.push_back(it->first);
  return out;
}

bool MemoryStorage::sync() {
  if (fail_sync) return false;
  ++syncs;
  durable_ = current_;
  return true;
}

void MemoryStorage::lose_unsynced() { current_ = durable_; }

size_t MemoryStorage::bytes() const {
  size_t n = 0;
  for (const auto &kv : current_) n += kv.second.size();
  return n;
}

namespace {

std::string sector_key(const Hash &h) { return "s/" + hex(h); }

Hash compute_blank() {
  std::vector<uint8_t> ff(kSector, 0xFF);
  return sha256(ff.data(), ff.size());
}

}  // namespace

const Hash &SectorStore::blank() {
  static const Hash h = compute_blank();
  return h;
}

bool SectorStore::put(const uint8_t *sector, Hash &out) {
  out = sha256(sector, kSector);
  if (out == blank() || storage_.exists(sector_key(out))) return true;
  return storage_.write(sector_key(out), sector, kSector);
}

bool SectorStore::get(const Hash &h, uint8_t *sector) {
  if (h == blank()) {
    for (uint32_t i = 0; i < kSector; ++i) sector[i] = 0xFF;
    return true;
  }
  std::vector<uint8_t> data;
  if (!storage_.read(sector_key(h), data) || data.size() != kSector) return false;
  if (sha256(data.data(), data.size()) != h) return false;
  for (uint32_t i = 0; i < kSector; ++i) sector[i] = data[i];
  return true;
}

bool SectorStore::has(const Hash &h) { return h == blank() || storage_.exists(sector_key(h)); }

size_t SectorStore::collect_garbage(const std::set<Hash> &referenced) {
  size_t removed = 0;
  for (const std::string &key : storage_.list("s/")) {
    Hash h;
    if (!parse_hash(key.substr(2), h) || !referenced.count(h)) {
      storage_.remove(key);
      ++removed;
    }
  }
  return removed;
}

size_t SectorStore::count() { return storage_.list("s/").size(); }

std::string Manifest::get(const std::string &k) const {
  auto it = fields.find(k);
  return it == fields.end() ? std::string() : it->second;
}

// One "key value" line per field, then one "@<addr hex> <hash hex>" line per sector.
std::string Manifest::serialize() const {
  std::string out;
  for (const auto &kv : fields) out += kv.first + " " + kv.second + "\n";
  char addr[16];
  for (const auto &s : sectors) {
    snprintf(addr, sizeof addr, "@%05x ", unsigned(s.first));
    out += addr + hex(s.second) + "\n";
  }
  return out;
}

bool Manifest::parse(const std::string &text, Manifest &out) {
  out = Manifest();
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) return false;  // a torn write loses its last newline
    std::string line = text.substr(start, nl - start);
    start = nl + 1;
    size_t sp = line.find(' ');
    if (sp == std::string::npos || sp == 0) return false;
    if (line[0] == '@') {
      char *end = nullptr;
      unsigned long addr = strtoul(line.c_str() + 1, &end, 16);
      Hash h;
      if (end != line.c_str() + sp || !parse_hash(line.substr(sp + 1), h)) return false;
      out.sectors.emplace_back(uint32_t(addr), h);
    } else {
      out.fields[line.substr(0, sp)] = line.substr(sp + 1);
    }
  }
  return true;
}

}  // namespace fm1_loader
