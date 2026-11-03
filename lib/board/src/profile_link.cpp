#include "profile_link.h"

#include <string.h>

namespace {

uint16_t crc16_xmodem(const uint8_t *p, size_t n) {
  uint16_t crc = 0;
  while (n--) {
    crc ^= uint16_t(*p++) << 8;
    for (int i = 0; i < 8; ++i) crc = crc & 0x8000 ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
  }
  return crc;
}

void refuse(std::vector<uint8_t> &out, uint8_t code, const char *why) {
  std::vector<uint8_t> p{code};
  p.insert(p.end(), why, why + strlen(why));
  const std::vector<uint8_t> f = profile_frame('e', p.data(), p.size());
  out.insert(out.end(), f.begin(), f.end());
}

void reply(std::vector<uint8_t> &out, uint8_t type, const uint8_t *payload = nullptr, size_t len = 0) {
  const std::vector<uint8_t> f = profile_frame(type, payload, len);
  out.insert(out.end(), f.begin(), f.end());
}

}  // namespace

std::vector<uint8_t> profile_frame(uint8_t type, const uint8_t *payload, size_t len) {
  std::vector<uint8_t> f{'O', 'P', type, uint8_t(len), uint8_t(len >> 8)};
  if (len) f.insert(f.end(), payload, payload + len);
  const uint16_t crc = crc16_xmodem(f.data() + 2, f.size() - 2);
  f.push_back(uint8_t(crc));
  f.push_back(uint8_t(crc >> 8));
  return f;
}

bool ProfileFrameReader::feed(uint8_t b) {
  buf_.push_back(b);
  for (;;) {
    // Resync on the magic: drop bytes until buf_ starts with 'O' 'P' (or could).
    if (!buf_.empty() && buf_[0] != 'O') {
      buf_.erase(buf_.begin());
      continue;
    }
    if (buf_.size() >= 2 && buf_[1] != 'P') {
      buf_.erase(buf_.begin());
      continue;
    }
    if (buf_.size() < 5) return false;
    const size_t len = buf_[3] | size_t(buf_[4]) << 8;
    if (len > kProfilePayloadMax) {  // not ours (or noise): look for the next 'O'
      buf_.erase(buf_.begin());
      continue;
    }
    if (buf_.size() < 5 + len + 2) return false;
    const uint16_t want = uint16_t(buf_[5 + len] | buf_[6 + len] << 8);
    if (crc16_xmodem(buf_.data() + 2, 3 + len) != want) {
      buf_.erase(buf_.begin());
      continue;
    }
    type_ = buf_[2];
    payload_.assign(buf_.begin() + 5, buf_.begin() + 5 + len);
    buf_.erase(buf_.begin(), buf_.begin() + 7 + len);
    return true;
  }
}

void ProfileLink::feed(const uint8_t *data, size_t len, std::vector<uint8_t> &out) {
  for (size_t i = 0; i < len; ++i)
    if (reader_.feed(data[i])) handle(reader_.type(), reader_.payload(), out);
}

void ProfileLink::handle(uint8_t type, const std::vector<uint8_t> &p, std::vector<uint8_t> &out) {
  switch (type) {
    case 'G': {
      uint8_t blob[kBlobSize];
      const size_t n = hooks_.load ? hooks_.load(blob, sizeof blob) : 0;
      return reply(out, 'g', blob, n);
    }
    case 'S': {
      BoardBlob b;
      char why[96] = "";
      BlobError e = board_blob_decode(p.data(), p.size(), b);
      if (e == BlobError::Format) return refuse(out, uint8_t(e), "not a board profile this firmware reads");
      if (e == BlobError::Crc) return refuse(out, uint8_t(e), "the profile was damaged on the way: send it again");
      if (e == BlobError::Ok) e = board_blob_check(b, why, sizeof why);
      if (e != BlobError::Ok) return refuse(out, uint8_t(e), why[0] ? why : "the profile breaks a rule");
      if (b.quad_psram != hooks_.build_quad_psram)
        return refuse(out, kPsramMismatch, b.quad_psram ? "the profile is for quad PSRAM; this build runs octal"
                                                        : "the profile is for octal PSRAM; this build runs quad");
      if (!hooks_.save || !hooks_.save(p.data(), p.size()))
        return refuse(out, kStoreFailed, "the board couldn't save the profile");
      return reply(out, 's');
    }
    case 'F':
      if (hooks_.forget) hooks_.forget();
      return reply(out, 'f');
    case 'B':
      reply(out, 'b');
      restart_due_ = true;  // the caller restarts once the reply is out
      return;
    default:
      return refuse(out, kBadRequest, "a request this board doesn't have");
  }
}
