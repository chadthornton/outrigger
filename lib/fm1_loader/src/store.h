// The shared, content-addressed sector store: every 4 KiB sector of every
// package, backup and save state is kept once, under its SHA-256, and each of
// those objects is a manifest listing sector hashes. The storage underneath is
// an abstract key-value interface, so internal flash (LittleFS) or a microSD card
// can sit behind it without touching the core.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "crypto.h"

namespace fm1_loader {

// Key-value storage. Keys are short ASCII paths such as "s/<hash>" or "m/unit/<id>".
// A write replaces the whole value. sync() returns only once everything written
// so far is durable (fsync on a file system).
class Storage {
 public:
  virtual ~Storage() = default;
  virtual bool read(const std::string &key, std::vector<uint8_t> &out) = 0;
  virtual bool write(const std::string &key, const uint8_t *data, size_t len) = 0;
  virtual bool remove(const std::string &key) = 0;
  virtual bool exists(const std::string &key) = 0;
  virtual std::vector<std::string> list(const std::string &prefix) = 0;
  virtual bool sync() = 0;
};

// In-memory storage for tests, with fault injection. Values written since the
// last sync() are lost by lose_unsynced(), as on a power cut.
class MemoryStorage : public Storage {
 public:
  bool read(const std::string &key, std::vector<uint8_t> &out) override;
  bool write(const std::string &key, const uint8_t *data, size_t len) override;
  bool remove(const std::string &key) override;
  bool exists(const std::string &key) override;
  std::vector<std::string> list(const std::string &prefix) override;
  bool sync() override;

  void lose_unsynced();
  int fail_writes_after = -1;  // >= 0: writes after this many more succeed fail
  bool fail_sync = false;
  size_t writes = 0;
  size_t syncs = 0;
  size_t bytes() const;  // stored value bytes (synced or not)

 private:
  std::map<std::string, std::vector<uint8_t>> durable_, current_;
};

class SectorStore {
 public:
  explicit SectorStore(Storage &storage) : storage_(storage) {}
  // Stores one kSector-byte sector; out gets its hash. Blank (all 0xFF) sectors
  // are never stored: get() makes them up.
  bool put(const uint8_t *sector, Hash &out);
  // Reads a sector back and checks its hash, so a damaged stored sector never
  // reaches the FM-1.
  bool get(const Hash &h, uint8_t *sector);
  bool has(const Hash &h);
  static const Hash &blank();
  // Removes stored sectors not in referenced; returns how many.
  size_t collect_garbage(const std::set<Hash> &referenced);
  size_t count();

 private:
  Storage &storage_;
};

// A package, backup, save or unit record: text fields plus (address, hash) pairs.
struct Manifest {
  std::map<std::string, std::string> fields;
  std::vector<std::pair<uint32_t, Hash>> sectors;

  std::string get(const std::string &k) const;
  std::string serialize() const;
  static bool parse(const std::string &text, Manifest &out);
};

}  // namespace fm1_loader
