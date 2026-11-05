#include "ota.h"

#include <stdio.h>
#include <string.h>

namespace ota {

namespace {

constexpr uint32_t kMagic = 0x3141544F;  // "OTA1"
constexpr uint8_t kFormat = 1;

uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
  }
  return ~crc;
}

void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

void put_image(uint8_t *p, const Image &im) {
  put32(p, im.size);
  memcpy(p + 4, im.sha256, 32);
}
void get_image(const uint8_t *p, Image &im) {
  im.size = get32(p);
  memcpy(im.sha256, p + 4, 32);
}

void logf(Ports &p, const char *fmt, const char *a = "", const char *b = "") {
  char line[96];
  snprintf(line, sizeof line, fmt, a, b);
  p.log(line);
}

// SHA-256 of the first im.size bytes of app0 (from_file null) or of a store file.
bool hash_matches(Ports &p, const char *from_file, const Image &im) {
  if (im.size == 0 || im.size > p.app0_size()) return false;
  if (from_file && p.file_size(from_file) != int64_t(im.size)) return false;
  static uint8_t buf[kSector];
  p.sha_begin();
  for (uint32_t off = 0; off < im.size; off += kSector) {
    const size_t n = im.size - off < kSector ? im.size - off : kSector;
    if (!(from_file ? p.file_read(from_file, off, buf, n) : p.app0_read(off, buf, n))) return false;
    p.sha_update(buf, n);
  }
  uint8_t got[32];
  p.sha_finish(got);
  return memcmp(got, im.sha256, 32) == 0;
}

bool copy(Ports &p, const char *file, const Image &im) {
  const uint32_t span = (im.size + kSector - 1) / kSector * kSector;
  if (!p.app0_erase(0, span)) return false;
  static uint8_t buf[kSector];
  for (uint32_t off = 0; off < im.size; off += kSector) {
    const size_t n = im.size - off < kSector ? im.size - off : kSector;
    if (!p.file_read(file, off, buf, n) || !p.app0_write(off, buf, n)) return false;
  }
  return hash_matches(p, nullptr, im);
}

// app0 holds im when this returns true. The file is checked before app0 is
// erased, so a damaged download never costs the image app0 has.
bool ensure(Ports &p, const char *file, const Image &im, const char *what) {
  if (hash_matches(p, nullptr, im)) return true;
  if (!hash_matches(p, file, im)) {
    logf(p, "recovery: %s in the store doesn't match its record", what);
    return false;
  }
  logf(p, "recovery: copying %s over app0", what);
  for (int i = 0; i < 2; ++i)
    if (copy(p, file, im)) return true;
  logf(p, "recovery: %s didn't verify after copying", what);
  return false;
}

}  // namespace

bool same_image(const Image &a, const Image &b) { return a.size == b.size && !memcmp(a.sha256, b.sha256, 32); }

void encode(const Record &r, uint8_t out[kRecordBytes]) {
  memset(out, 0, kRecordBytes);
  put32(out, kMagic);
  out[4] = kFormat;
  out[5] = uint8_t(r.state);
  out[6] = uint8_t(r.result);
  out[7] = r.attempts;
  put_image(out + 8, r.next);
  put_image(out + 44, r.prev);
  memcpy(out + 80, r.label, sizeof r.label);
  out[95] = 0;
  put32(out + 96, crc32(out, 96));
}

bool decode(const uint8_t *data, size_t len, Record &out) {
  if (len != kRecordBytes || get32(data) != kMagic || data[4] != kFormat) return false;
  if (get32(data + 96) != crc32(data, 96)) return false;
  if (data[5] > uint8_t(State::RolledBack) || data[6] > uint8_t(Result::Abandoned) || data[95]) return false;
  Record r;
  r.state = State(data[5]);
  r.result = Result(data[6]);
  r.attempts = data[7];
  get_image(data + 8, r.next);
  get_image(data + 44, r.prev);
  memcpy(r.label, data + 80, sizeof r.label);
  out = r;
  return true;
}

const char *state_name(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Staged: return "staged";
    case State::Trial: return "trial";
    case State::RollingBack: return "rolling-back";
    case State::RolledBack: return "rolled-back";
  }
  return "?";
}

const char *result_name(Result r) {
  switch (r) {
    case Result::None: return "none";
    case Result::Updated: return "updated";
    case Result::RolledBack: return "rolled-back";
    case Result::Abandoned: return "abandoned";
  }
  return "?";
}

Outcome run_recovery(Ports &p) {
  Record r;
  if (!p.load(r)) {
    p.log("recovery: no update record");
    r = Record{};
  }
  logf(p, "recovery: record %s, update %s", state_name(r.state), r.label);

  if (r.state == State::Idle) {
    if (p.boot_app0()) return Outcome::Restart;
    p.log("recovery: nothing pending and app0 doesn't validate");
    return Outcome::Halt;
  }

  if (r.state == State::Staged || r.state == State::Trial) {
    if (r.state == State::Staged) r.attempts = 0;
    if (r.attempts >= kMaxTrials) {
      p.log("recovery: the new build didn't confirm itself; rolling back");
    } else if (ensure(p, kNextFile, r.next, "the new build")) {
      r.state = State::Trial;
      ++r.attempts;
      char line[48];
      snprintf(line, sizeof line, "recovery: trial boot %u of %d", unsigned(r.attempts), kMaxTrials);
      p.log(line);
      // Counted before otadata changes: a cut in between costs a trial, never
      // an uncounted boot.
      if (!p.save(r)) p.log("recovery: couldn't save the record");
      if (p.boot_app0()) return Outcome::Restart;
      p.log("recovery: the new build doesn't validate as an app");
    }
    r.state = State::RollingBack;
    r.result = Result::RolledBack;
    if (!p.save(r)) p.log("recovery: couldn't save the record");
  }

  // RollingBack or RolledBack: app0 must hold the old build.
  if (!ensure(p, kPrevFile, r.prev, "the old build")) {
    p.log("recovery: no good old build to go back to");
    return p.boot_app0() ? Outcome::Restart : Outcome::Halt;
  }
  if (r.state != State::RolledBack) {
    r.state = State::RolledBack;
    r.result = Result::RolledBack;
    if (!p.save(r)) p.log("recovery: couldn't save the record");
  }
  if (p.boot_app0()) return Outcome::Restart;
  p.log("recovery: the old build doesn't validate as an app");
  return Outcome::Halt;
}

Record staged(const Image &next, const Image &prev, const char *label) {
  Record r;
  r.state = State::Staged;
  r.next = next;
  r.prev = prev;
  snprintf(r.label, sizeof r.label, "%s", label ? label : "");
  return r;
}

BootPlan plan_boot(bool have, const Record &r, bool pending_verify, bool running_is_next, bool running_is_prev) {
  BootPlan p;
  p.rec = have ? r : Record{};
  if (!have || p.rec.state == State::Idle) {
    p.remove_files = true;
    return p;
  }
  switch (p.rec.state) {
    case State::Trial:
      if (running_is_next && pending_verify) {
        p.trial = true;
        return p;
      }
      if (running_is_next) {  // confirmed, but the cut came before the record's save
        p.rec = confirmed(p.rec);
        break;
      }
      p.rec.result = Result::Abandoned;  // a USB install replaced it
      break;
    case State::RollingBack:
    case State::RolledBack:
      p.rec.result = running_is_prev ? Result::RolledBack : Result::Abandoned;
      break;
    default:  // Staged: the recovery app never ran
      p.rec.result = Result::Abandoned;
      break;
  }
  p.rec.state = State::Idle;
  p.write = true;
  p.remove_files = true;
  return p;
}

Record confirmed(const Record &r) {
  Record c = r;
  c.state = State::Idle;
  c.result = Result::Updated;
  return c;
}

}  // namespace ota
