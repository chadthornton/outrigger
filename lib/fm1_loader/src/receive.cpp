#include "receive.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "library.h"

namespace fm1_loader {

namespace {

constexpr uint8_t kMagic0 = 'O', kMagic1 = 'R';
constexpr size_t kHeader = 5;  // magic, type, length

void put32(std::vector<uint8_t> &p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p.push_back(uint8_t(v >> (8 * i)));
}

uint32_t get32(const std::vector<uint8_t> &p, size_t at) {
  return p[at] | p[at + 1] << 8 | p[at + 2] << 16 | uint32_t(p[at + 3]) << 24;
}

void reply(std::vector<uint8_t> &out, uint8_t type, const std::vector<uint8_t> &payload) {
  const std::vector<uint8_t> f = encode_frame(type, payload.data(), payload.size());
  out.insert(out.end(), f.begin(), f.end());
}

}  // namespace

std::vector<uint8_t> encode_frame(uint8_t type, const uint8_t *payload, size_t len) {
  std::vector<uint8_t> f = {kMagic0, kMagic1, type, uint8_t(len), uint8_t(len >> 8)};
  f.insert(f.end(), payload, payload + len);
  const uint16_t crc = crc16(f.data() + 2, f.size() - 2);
  f.push_back(uint8_t(crc));
  f.push_back(uint8_t(crc >> 8));
  return f;
}

bool FrameReader::feed(uint8_t b) {
  buf_.push_back(b);
  for (;;) {
    // Resynchronise on the magic: drop bytes until the buffer starts with it.
    size_t skip = 0;
    while (skip < buf_.size() && !(buf_[skip] == kMagic0 && (skip + 1 == buf_.size() || buf_[skip + 1] == kMagic1)))
      ++skip;
    buf_.erase(buf_.begin(), buf_.begin() + skip);
    if (buf_.size() < kHeader) return false;
    const size_t len = buf_[3] | buf_[4] << 8;
    if (len > kFrameMax) {  // not a frame of ours: look for the next magic
      buf_.erase(buf_.begin());
      continue;
    }
    if (buf_.size() < kHeader + len + 2) return false;
    const uint16_t crc = crc16(buf_.data() + 2, kHeader - 2 + len);
    if ((buf_[kHeader + len] | buf_[kHeader + len + 1] << 8) != crc) {
      buf_.erase(buf_.begin());
      continue;
    }
    type_ = buf_[2];
    payload_.assign(buf_.begin() + kHeader, buf_.begin() + kHeader + len);
    buf_.clear();
    return true;
  }
}

struct Receiver::Buffer {
  uint8_t *data = nullptr;
  uint32_t size = 0, got = 0;
  Hash sha256{};
  std::string id, project, version, name;
  ~Buffer() { free(data); }
};

Receiver::Receiver(Library &lib, std::function<uint64_t()> free_bytes, std::string build)
    : lib_(lib), free_bytes_(std::move(free_bytes)), build_(std::move(build)) {}

Receiver::~Receiver() = default;

void Receiver::feed(const uint8_t *data, size_t len, uint32_t now_ms, std::vector<uint8_t> &out) {
  for (size_t i = 0; i < len; ++i)
    if (reader_.feed(data[i])) handle(reader_.type(), reader_.payload(), now_ms, out);
}

void Receiver::tick(uint32_t now_ms) {
  if (buf_ && now_ms - last_ms_ > kIdleMs) {
    drop();
    set_state(ReceiveStatus::State::Failed, "The computer stopped sending");
  }
}

void Receiver::handle(uint8_t type, const std::vector<uint8_t> &p, uint32_t now_ms, std::vector<uint8_t> &out) {
  if (type == 'H' && p.empty()) {
    const std::vector<std::string> ids = lib_.package_ids();
    const uint64_t free = free_bytes_();
    std::vector<uint8_t> r = {kReceiveProtocol, uint8_t(ids.size()), uint8_t(kMaxPackages)};
    put32(r, free > UINT32_MAX ? UINT32_MAX : uint32_t(free));
    r.insert(r.end(), build_.begin(), build_.end());
    reply(out, 'h', r);
  } else if (type == 'L' && p.size() == 1) {
    std::vector<std::string> ids = lib_.package_ids();
    PackageInfo info;
    if (p[0] >= ids.size() || !lib_.package(ids[p[0]], info)) return refuse(out, ReceiveError::BadRequest, "No such package");
    std::vector<uint8_t> r = {p[0]};
    r.insert(r.end(), info.file_sha256.begin(), info.file_sha256.end());
    r.insert(r.end(), info.id.begin(), info.id.end());
    reply(out, 'l', r);
  } else if (type == 'A') {
    begin(p, now_ms, out);
  } else if (type == 'D' && p.size() >= 4) {
    data(p, now_ms, out);
  } else if (type == 'X' && p.empty()) {
    if (buf_) {
      drop();
      set_state(ReceiveStatus::State::Idle);
    }
    std::vector<uint8_t> r;
    put32(r, 0);
    reply(out, 'k', r);
  } else {
    refuse(out, ReceiveError::BadRequest, "Outrigger didn't understand the page");
  }
}

void Receiver::refuse(std::vector<uint8_t> &out, ReceiveError e, const std::string &why) {
  std::vector<uint8_t> r = {uint8_t(e)};
  r.insert(r.end(), why.begin(), why.end());
  reply(out, 'e', r);
}

void Receiver::begin(const std::vector<uint8_t> &p, uint32_t now_ms, std::vector<uint8_t> &out) {
  drop();
  last_reply_.clear();
  // size, SHA-256, then three NUL-terminated strings.
  std::vector<std::string> strs;
  size_t at = 36;
  while (p.size() >= 36 && at < p.size() && strs.size() < 3) {
    const size_t end = std::find(p.begin() + at, p.end(), uint8_t(0)) - p.begin();
    if (end == p.size()) break;
    strs.emplace_back(p.begin() + at, p.begin() + end);
    at = end + 1;
  }
  if (p.size() < 36 || strs.size() != 3 || at != p.size())
    return refuse(out, ReceiveError::BadRequest, "Outrigger didn't understand the page");
  auto b = std::unique_ptr<Buffer>(new Buffer());
  b->size = get32(p, 0);
  memcpy(b->sha256.data(), p.data() + 4, 32);
  b->project = strs[0];
  b->version = strs[1];
  b->name = strs[2];
  // As tools/store_add.py names catalog packages.
  b->id = b->version.empty() || (b->project.size() > b->version.size() &&
                                  b->project.compare(b->project.size() - b->version.size(), std::string::npos, b->version) == 0)
              ? b->project
              : b->project + "-" + b->version;
  status_ = ReceiveStatus();
  status_.label = b->version.empty() ? b->name : b->name + " " + b->version;
  status_.total = b->size;
  if (b->size == 0 || b->size > kPackageMax)
    return refuse(out, ReceiveError::TooBig, "That file is too big to be an FM-1 firmware");
  if (b->project.empty())
    return refuse(out, ReceiveError::NoProject, "Only firmwares from the Firmware Hub's list can be added for now");
  if (!key_safe(b->project) || !key_safe(b->id))
    return refuse(out, ReceiveError::BadRequest, "Outrigger didn't understand the page");
  if (const char *why = blocked_reason(b->project)) return refuse(out, ReceiveError::Blocked, why);
  // Already there, under this name or the one the Mac gave it: nothing to send.
  const std::vector<std::string> ids = lib_.package_ids();
  for (const std::string &id : ids) {
    PackageInfo info;
    if (!lib_.package(id, info)) continue;
    if (info.file_sha256 == b->sha256) {
      const std::vector<uint8_t> r = {1};
      return reply(out, 'd', r);
    }
    if (id == b->id) return refuse(out, ReceiveError::IdTaken, "Outrigger has a different file under this name");
  }
  if (ids.size() >= kMaxPackages)
    return refuse(out, ReceiveError::TooMany, "Outrigger's firmware list is full (10)");
  if (free_bytes_() < kBackupReserve + kPackageRoom)
    return refuse(out, ReceiveError::StoreFull, "Outrigger's store is too full: it keeps room for a backup");
  b->data = static_cast<uint8_t *>(malloc(b->size));
  if (!b->data) {
    set_state(ReceiveStatus::State::Failed, "Not enough memory");
    return refuse(out, ReceiveError::NoMemory, "Outrigger hasn't the memory for it. Restart it and try again.");
  }
  buf_ = std::move(b);
  last_ms_ = now_ms;
  set_state(ReceiveStatus::State::Receiving);
  std::vector<uint8_t> r;
  put32(r, 0);
  reply(out, 'k', r);
}

void Receiver::data(const std::vector<uint8_t> &p, uint32_t now_ms, std::vector<uint8_t> &out) {
  const uint32_t offset = get32(p, 0);
  const size_t n = p.size() - 4;
  if (!buf_) {
    // The page repeated the last frame of a finished transfer: its reply was lost.
    if (!last_reply_.empty() && uint64_t(offset) + n == last_size_) {
      out.insert(out.end(), last_reply_.begin(), last_reply_.end());
      return;
    }
    return refuse(out, ReceiveError::NotReceiving, "Outrigger wasn't expecting data. Try again.");
  }
  // Anything but the next bytes is ignored: the ack says where to go on from.
  if (offset == buf_->got && n > 0 && n <= kDataMax && uint64_t(offset) + n <= buf_->size) {
    memcpy(buf_->data + offset, p.data() + 4, n);
    buf_->got += uint32_t(n);
    last_ms_ = now_ms;
    status_.got = buf_->got;
    if (on_status_) on_status_(status_);
  }
  if (buf_->got < buf_->size) {
    std::vector<uint8_t> r;
    put32(r, buf_->got);
    return reply(out, 'k', r);
  }
  std::vector<uint8_t> r;
  store(r);
  last_size_ = buf_->size;
  last_reply_ = r;
  drop();
  out.insert(out.end(), r.begin(), r.end());
}

void Receiver::store(std::vector<uint8_t> &out) {
  Buffer &b = *buf_;
  set_state(ReceiveStatus::State::Adding);
  if (sha256(b.data, b.size) != b.sha256) {
    set_state(ReceiveStatus::State::Failed, "It arrived damaged");
    return refuse(out, ReceiveError::HashMismatch, "The file arrived damaged. Try again.");
  }
  const PackageError e = lib_.add_package(b.id, b.data, b.size, &b.sha256, b.name, b.version, b.project);
  if (e != PackageError::None) {
    const bool bad = e != PackageError::StoreFailed;
    set_state(ReceiveStatus::State::Failed, bad ? package_error_text(e) : "The store refused a write");
    return refuse(out, bad ? ReceiveError::BadPackage : ReceiveError::StoreFailed,
                  bad ? std::string("Outrigger can't use this file: ") + package_error_text(e)
                      : "Outrigger's store refused a write. Nothing you had was changed.");
  }
  set_state(ReceiveStatus::State::Added);
  reply(out, 'd', {0});
}

void Receiver::drop() { buf_.reset(); }

void Receiver::set_state(ReceiveStatus::State s, const std::string &why) {
  status_.state = s;
  status_.why = why;
  if (on_status_) on_status_(status_);
}

}  // namespace fm1_loader
