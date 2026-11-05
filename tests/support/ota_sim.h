// A model of an Outrigger board for lib/ota's tests: app0 and the recovery
// slot, otadata, the NVS record, the store's u/ files, and the ESP-IDF 5.5.5
// bootloader's choice between the two app slots with app rollback on
// (bootloader_utility_get_selected_boot_partition, esp_ota_set_boot_partition,
// esp_ota_mark_app_valid_cancel_rollback, esp_ota_get_state_partition).
//
// Every durable write counts as one step. cut_at = N throws PowerCut at step N,
// leaving that write torn: half of a flash write or erase, an otadata entry with
// a bad CRC. NVS writes are atomic (NVS keeps the old value on a cut), as are
// LittleFS file writes (temp file, then rename).
#pragma once

#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "crypto.h"
#include "ota.h"

namespace ota_sim {

struct PowerCut {};

enum OtaState : uint32_t {
  kNew = 0,
  kPendingVerify = 1,
  kValid = 2,
  kInvalid = 3,
  kAborted = 4,
  kUndefined = 0xFFFFFFFF,
};

struct Entry {
  uint32_t seq = UINT32_MAX;
  uint32_t state = kUndefined;
  bool crc_ok = false;  // an erased entry's CRC never matches
};

// A build the board can run. confirms: it reaches Play mode and confirms itself.
// ota_aware: it has lib/ota (verifyRollbackLater, plan_boot); older builds let
// Arduino mark any PENDING_VERIFY image valid at startup.
struct Build {
  std::string name;
  std::vector<uint8_t> bytes;
  bool confirms = true;
  bool ota_aware = true;
  ota::Image image() const {
    ota::Image im;
    im.size = uint32_t(bytes.size());
    fm1_loader::Hash h = fm1_loader::sha256(bytes.data(), bytes.size());
    memcpy(im.sha256, h.data(), 32);
    return im;
  }
};

constexpr uint32_t kApp0Size = 16 * ota::kSector;

class Board : public ota::Ports {
 public:
  std::vector<uint8_t> app0 = std::vector<uint8_t>(kApp0Size, 0xFF);
  bool recovery_installed = true;
  Entry od[2];
  bool have_rec = false;
  uint8_t rec_blob[ota::kRecordBytes] = {};
  std::map<std::string, std::vector<uint8_t>> files;
  std::vector<Build> builds;  // every image app0 may validate as
  long cut_at = -1;
  long steps = 0;
  long app0_erases = 0;
  int flip_on_write = -1;  // >= 0: that app0 write (counting from 0) lands with one bit wrong
  int app0_writes = 0;
  std::vector<std::string> logs;
  std::vector<std::string> boots;  // what each boot ran: a build's name or "recovery"

  // A USB install (make flash, the installer): app0 written, boot_app0 at otadata.
  void install(const Build &b) {
    std::fill(app0.begin(), app0.end(), 0xFF);
    std::copy(b.bytes.begin(), b.bytes.end(), app0.begin());
    od[0] = {1, kUndefined, true};
    od[1] = Entry{};
  }

  void step() {
    if (cut_at >= 0 && steps == cut_at) {
      ++steps;
      throw PowerCut{};
    }
    ++steps;
  }
  bool cutting_now() const { return cut_at >= 0 && steps == cut_at; }

  // ---------- otadata (esp_ota_ops.c / bootloader_common.c) ----------
  static bool invalid(const Entry &e) { return e.seq == UINT32_MAX || e.state == kInvalid || e.state == kAborted; }
  static bool valid(const Entry &e) { return !invalid(e) && e.crc_ok; }
  int active() const {
    const bool v0 = valid(od[0]), v1 = valid(od[1]);
    if (v0 && v1) return od[0].seq >= od[1].seq ? 0 : 1;
    if (v0) return 0;
    if (v1) return 1;
    return -1;
  }
  void write_entry(int i, Entry e) {
    if (cutting_now()) {
      od[i] = Entry{e.seq, e.state, false};
      step();
    }
    step();
    od[i] = e;
  }
  // esp_ota_get_state_partition: the first entry with a good CRC mapping to the slot.
  uint32_t state_of(int slot) const {
    for (int i = 0; i < 2; ++i)
      if (od[i].crc_ok && int((od[i].seq - 1) % 2) == slot) return od[i].state;
    return kUndefined;
  }
  bool slot_validates(int slot) const {
    if (slot == 1) return recovery_installed;
    return build_in_app0() != nullptr;
  }
  const Build *build_in_app0() const {
    for (const Build &b : builds)
      if (b.bytes.size() <= app0.size() && std::equal(b.bytes.begin(), b.bytes.end(), app0.begin())) return &b;
    return nullptr;
  }
  // esp_ota_set_boot_partition with rollback on.
  bool set_boot(int slot) {
    if (!slot_validates(slot)) return false;
    const int a = active();
    if (a < 0) {
      write_entry(0, {uint32_t(slot + 1), kNew, true});
      return true;
    }
    uint32_t i = 0;
    const uint32_t seq = od[a].seq;
    while (seq > uint32_t((slot + 1) % 2) + i * 2) ++i;
    write_entry(a ^ 1, {uint32_t((slot + 1) % 2) + i * 2, kNew, true});
    return true;
  }
  // esp_ota_mark_app_valid_cancel_rollback: the active entry becomes VALID.
  void mark_valid() {
    const int a = active();
    if (a >= 0 && od[a].state != kValid) write_entry(a, {od[a].seq, kValid, true});
  }
  // The bootloader: which slot it starts, or -1 when nothing loads.
  int bootloader() {
    for (int i = 0; i < 2; ++i)
      if (od[i].crc_ok && od[i].state == kPendingVerify) write_entry(i, {od[i].seq, kAborted, true});
    int index = 0;
    bool initial = false;
    if (invalid(od[0]) && invalid(od[1])) {
      initial = (od[0].seq == UINT32_MAX || !od[0].crc_ok) && (od[1].seq == UINT32_MAX || !od[1].crc_ok);
    } else {
      const int a = active();
      if (a >= 0) {
        index = int((od[a].seq - 1) % 2);
        if (od[a].state == kNew) write_entry(a, {od[a].seq, kPendingVerify, true});
      }
    }
    // Backwards from index to factory (there is none), then forwards.
    const int order[2][2] = {{0, 1}, {1, 0}};
    for (int slot : order[index]) {
      if (!slot_validates(slot)) continue;
      if (slot > 0 && initial) write_entry(0, {uint32_t(slot + 1), kValid, true});
      return slot;
    }
    return -1;
  }

  // ---------- the store ----------
  void write_file(const char *name, const std::vector<uint8_t> &data) {
    step();
    files[name] = data;
  }
  void remove_files() {
    for (const char *n : {ota::kNextFile, ota::kPrevFile})
      if (files.count(n)) {
        step();
        files.erase(n);
      }
  }
  bool app0_holds(const ota::Image &im) const {
    if (im.size == 0 || im.size > app0.size()) return false;
    fm1_loader::Hash h = fm1_loader::sha256(app0.data(), im.size);
    return memcmp(h.data(), im.sha256, 32) == 0;
  }

  // ---------- ota::Ports ----------
  bool load(ota::Record &out) override { return have_rec && ota::decode(rec_blob, sizeof rec_blob, out); }
  bool save(const ota::Record &r) override {
    step();
    ota::encode(r, rec_blob);
    have_rec = true;
    return true;
  }
  int64_t file_size(const char *name) override {
    auto it = files.find(name);
    return it == files.end() ? -1 : int64_t(it->second.size());
  }
  bool file_read(const char *name, uint32_t offset, uint8_t *buf, size_t len) override {
    auto it = files.find(name);
    if (it == files.end() || offset + len > it->second.size()) return false;
    memcpy(buf, it->second.data() + offset, len);
    return true;
  }
  uint32_t app0_size() override { return kApp0Size; }
  bool app0_erase(uint32_t offset, uint32_t len) override {
    if (offset % ota::kSector || len % ota::kSector || offset + len > app0.size()) return false;
    ++app0_erases;
    if (cutting_now()) {
      std::fill(app0.begin() + offset, app0.begin() + offset + len / 2, 0xFF);
      step();
    }
    step();
    std::fill(app0.begin() + offset, app0.begin() + offset + len, 0xFF);
    return true;
  }
  bool app0_write(uint32_t offset, const uint8_t *buf, size_t len) override {
    if (offset + len > app0.size()) return false;
    // NOR flash: a write only clears bits.
    const size_t n = cutting_now() ? len / 2 : len;
    for (size_t i = 0; i < n; ++i) app0[offset + i] &= buf[i];
    if (app0_writes++ == flip_on_write && n) app0[offset] ^= 0x01;
    step();
    return true;
  }
  bool app0_read(uint32_t offset, uint8_t *buf, size_t len) override {
    if (offset + len > app0.size()) return false;
    memcpy(buf, app0.data() + offset, len);
    return true;
  }
  bool boot_app0() override { return set_boot(0); }
  void sha_begin() override { sha_ = fm1_loader::Sha256(); }
  void sha_update(const uint8_t *data, size_t len) override { sha_.update(data, len); }
  void sha_finish(uint8_t out[32]) override {
    fm1_loader::Hash h = sha_.finish();
    memcpy(out, h.data(), 32);
  }
  void log(const char *line) override { logs.push_back(line); }

  ota::Record record() {
    ota::Record r;
    if (!load(r)) r = ota::Record{};
    return r;
  }

 private:
  fm1_loader::Sha256 sha_;
};

// How a boot ended.
enum class End { Settled, Restart, Halted, NothingBoots };

// Outrigger's boot as firmware/waveshare_169/update_app.cpp runs it.
inline End outrigger_boot(Board &s, const Build &b) {
  const bool pending = s.state_of(0) == kPendingVerify;
  if (!b.ota_aware) {  // initArduino with the default verifyRollbackLater()
    if (pending) s.mark_valid();
    return End::Settled;
  }
  ota::Record r;
  const bool have = s.load(r);
  const ota::BootPlan plan =
      ota::plan_boot(have, r, pending, have && s.app0_holds(r.next), have && s.app0_holds(r.prev));
  if (plan.write) s.save(plan.rec);
  if (plan.remove_files) s.remove_files();
  if (!b.confirms) return End::Restart;  // a crash, or the confirm deadline's restart
  if (pending) {
    s.mark_valid();
    if (plan.trial) {
      s.save(ota::confirmed(plan.rec));
      s.remove_files();
    }
  }
  return End::Settled;
}

// One power-on to the end of that boot.
inline End boot_once(Board &s) {
  const int slot = s.bootloader();
  if (slot < 0) {
    s.boots.push_back("nothing");
    return End::NothingBoots;
  }
  if (slot == 1) {
    s.boots.push_back("recovery");
    if (s.state_of(1) == kPendingVerify) s.mark_valid();  // initArduino
    return ota::run_recovery(s) == ota::Outcome::Restart ? End::Restart : End::Halted;
  }
  const Build *b = s.build_in_app0();
  s.boots.push_back(b->name);
  return outrigger_boot(s, *b);
}

// Boots until the board settles in a build, halts, or 30 boots pass. A power
// cut ends a boot like a restart does.
inline End run(Board &s) {
  for (int i = 0; i < 30; ++i) {
    End e;
    try {
      e = boot_once(s);
    } catch (PowerCut &) {
      e = End::Restart;
    }
    if (e != End::Restart) return e;
  }
  return End::Restart;
}

// Outrigger starting an update (update_app.cpp, ota_start): both files, their
// checks, the record, then otadata. False when the recovery app isn't there.
inline bool stage(Board &s, const Build &next, const Build &running) {
  s.write_file(ota::kNextFile, next.bytes);
  s.write_file(ota::kPrevFile, running.bytes);
  s.save(ota::staged(next.image(), running.image(), next.name.c_str()));
  if (!s.set_boot(1)) {
    ota::Record r = s.record();
    r.state = ota::State::Idle;
    r.result = ota::Result::Abandoned;
    s.save(r);
    s.remove_files();
    return false;
  }
  return true;
}

}  // namespace ota_sim
