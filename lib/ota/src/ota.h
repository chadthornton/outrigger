// Outrigger's own updates over Wi-Fi (docs/ota-updates.md). Plain C++, no
// Arduino headers: the Mac tests (tests/test_ota.cpp) run this same code against
// a model of the ESP-IDF bootloader, with a power cut at every write.
//
// How an update runs:
// 1. Outrigger puts the new image in the store as u/next.bin and its own running
//    image as u/prev.bin, checks both by SHA-256, saves a Staged record in NVS,
//    and points otadata at the recovery app (staged(), then
//    esp_ota_set_boot_partition).
// 2. The recovery app (firmware/recovery) runs run_recovery(): it copies
//    u/next.bin over app0, checks the copy, counts the boot in the record, and
//    points otadata at app0.
// 3. The bootloader boots the new app0 in PENDING_VERIFY. Outrigger confirms it
//    (esp_ota_mark_app_valid_cancel_rollback) once it is healthy. Any boot that
//    ends without confirming (a crash, a watchdog, a power cut, the deadline)
//    makes the bootloader mark app0 ABORTED and boot the recovery app again.
// 4. After kMaxTrials boots without a confirm, the recovery app copies
//    u/prev.bin back and boots that.
//
// A power cut at any point leaves the record and otadata saying what to redo:
// while app0 is being written, otadata still points at the recovery app, and
// a half-written app0 fails the bootloader's image check anyway.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ota {

constexpr int kMaxTrials = 3;       // boots the new image gets to confirm itself
constexpr uint32_t kSector = 4096;  // flash erase unit
// Store files, relative to the store's root (/store on the board). LittleFS
// garbage collection and package eviction never touch u/.
constexpr const char *kNextFile = "u/next.bin";
constexpr const char *kPrevFile = "u/prev.bin";

struct Image {
  uint32_t size = 0;  // bytes, the image as built (header to appended SHA-256)
  uint8_t sha256[32] = {};
};
bool same_image(const Image &a, const Image &b);

enum class State : uint8_t {
  Idle = 0,         // nothing pending
  Staged = 1,       // both images in the store, checked; the recovery app copies next
  Trial = 2,        // app0 holds next; it hasn't confirmed itself yet
  RollingBack = 3,  // next failed: the recovery app copies prev back
  RolledBack = 4,   // app0 holds prev again
};

enum class Result : uint8_t {
  None = 0,
  Updated = 1,     // next confirmed itself
  RolledBack = 2,  // next never confirmed (or couldn't be copied); prev runs
  Abandoned = 3,   // the update never got going, or a USB install replaced it
};

struct Record {
  State state = State::Idle;
  Result result = Result::None;  // the last update's outcome, kept while Idle
  uint8_t attempts = 0;          // boots of next so far
  // GPIOs the recovery app drives while it works, from Outrigger's board
  // profile (it has none of its own): the backlight on, so the screen keeps
  // "Updating..." lit, and pins wired to a speaker amplifier off. -1: unused.
  int8_t pins_high[4] = {-1, -1, -1, -1};
  int8_t pins_low[4] = {-1, -1, -1, -1};
  Image next, prev;
  char label[16] = {};  // the new build's commit, for /status
};

// NVS holds the record as one blob, so it changes all at once.
constexpr size_t kRecordBytes = 4 + 4 + 8 + 2 * 36 + 16 + 4;
void encode(const Record &r, uint8_t out[kRecordBytes]);
// False for anything but an intact record of this format.
bool decode(const uint8_t *data, size_t len, Record &out);

const char *state_name(State s);
const char *result_name(Result r);

// ---------- The recovery app ----------

// What the recovery app reaches the hardware through. On the board: NVS
// (Preferences), LittleFS at /store, the app0 partition, mbedtls; in the Mac
// tests, the bootloader model.
class Ports {
 public:
  virtual ~Ports() = default;
  virtual bool load(Record &out) = 0;  // false: no record, or a damaged one
  virtual bool save(const Record &r) = 0;
  virtual int64_t file_size(const char *name) = 0;  // -1: missing
  virtual bool file_read(const char *name, uint32_t offset, uint8_t *buf, size_t len) = 0;
  virtual uint32_t app0_size() = 0;
  virtual bool app0_erase(uint32_t offset, uint32_t len) = 0;  // kSector-aligned
  virtual bool app0_write(uint32_t offset, const uint8_t *buf, size_t len) = 0;
  virtual bool app0_read(uint32_t offset, uint8_t *buf, size_t len) = 0;
  // esp_ota_set_boot_partition(app0): false when app0's image doesn't validate.
  virtual bool boot_app0() = 0;
  virtual void sha_begin() = 0;
  virtual void sha_update(const uint8_t *data, size_t len) = 0;
  virtual void sha_finish(uint8_t out[32]) = 0;
  virtual void log(const char *line) = 0;
};

enum class Outcome {
  Restart,  // otadata points at app0: restart into it
  Halt,     // nothing bootable to hand over to; stay in the recovery app
};

// The recovery app's whole job, from the top on every boot of it.
Outcome run_recovery(Ports &p);

// ---------- Outrigger's side ----------

// The record that starts an update, once both files are in the store and checked.
Record staged(const Image &next, const Image &prev, const char *label);

// What Outrigger does with the record when it boots.
struct BootPlan {
  bool trial = false;         // this boot is next's trial: confirm once healthy
  bool write = false;         // save rec
  bool remove_files = false;  // u/next.bin and u/prev.bin are no longer needed
  Record rec;
};
// have: a record was read. pending_verify: the bootloader started this image in
// PENDING_VERIFY. running_is_next / _prev: app0 holds that image of the record.
BootPlan plan_boot(bool have, const Record &r, bool pending_verify, bool running_is_next, bool running_is_prev);

// The record once next has confirmed itself.
Record confirmed(const Record &r);

}  // namespace ota
