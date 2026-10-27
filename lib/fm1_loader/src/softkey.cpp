#include "softkey.h"

namespace fm1_loader {

const uint8_t kKeyTransfer[8] = {0x04, 0xF0, 0x22, 0x24, 0x07, 0x35, 0x7D, 0xF7};
const uint8_t kIdentityQuery[10] = {0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7};

void sysex_packets(const uint8_t *msg, size_t len, std::vector<uint8_t> &out) {
  size_t i = 0;
  while (len - i > 3) {  // SysEx start or continue: 3 bytes, CIN 4
    out.insert(out.end(), {0x04, msg[i], msg[i + 1], msg[i + 2]});
    i += 3;
  }
  // The end: 1, 2 or 3 bytes, CIN 5, 6 or 7.
  const size_t n = len - i;
  uint8_t p[4] = {uint8_t(0x04 + n), 0, 0, 0};
  for (size_t k = 0; k < n; ++k) p[1 + k] = msg[i + k];
  out.insert(out.end(), p, p + 4);
}

bool SysexAssembler::feed(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2,
                          std::vector<uint8_t> &msg) {
  const uint8_t b[3] = {b0, b1, b2};
  size_t n;
  switch (cin) {
    case 0x4: n = 3; break;  // start or continue
    case 0x5: n = 1; break;  // ends with 1 byte
    case 0x6: n = 2; break;
    case 0x7: n = 3; break;
    default: return false;   // not SysEx: leave a message in progress alone
  }
  if (b0 == 0xF0) {
    buf_.clear();
    active_ = true;
  }
  if (!active_) return false;
  buf_.insert(buf_.end(), b, b + n);
  if (buf_.size() > 256) {  // an identity answer is 42 bytes
    active_ = false;
    return false;
  }
  if (cin == 0x4) return false;
  active_ = false;
  if (buf_.back() != 0xF7) return false;
  msg = buf_;
  return true;
}

std::string parse_identity(const std::vector<uint8_t> &sysex) {
  if (sysex.size() < 2 || sysex.front() != 0xF0 || sysex.back() != 0xF7) return std::string();
  // unpack7: 7-bit SysEx data back to 8-bit bytes, low bits first.
  std::vector<uint8_t> d;
  uint32_t acc = 0;
  int nb = 0;
  for (size_t i = 1; i + 1 < sysex.size(); ++i) {
    acc |= uint32_t(sysex[i] & 0x7F) << nb;
    nb += 7;
    while (nb >= 8) {
      d.push_back(uint8_t(acc));
      acc >>= 8;
      nb -= 8;
    }
  }
  if (d.size() != 34 || d[0] != 0x00 || d[1] != 0x59 || d[2] != 0x11) return std::string();
  std::string text;
  for (size_t i = 6; i < 33 && d[i]; ++i) text += char(d[i]);
  return text;
}

const char *key_result_text(KeyResult r) {
  switch (r) {
    case KeyResult::Sent: return "key sent";
    case KeyResult::NoAnswer: return "the FM-1 doesn't answer: is it on its normal screen?";
    case KeyResult::UpdateMode: return "the FM-1 is mid-update: switch it off and on";
    case KeyResult::NotFm1: return "not an FM-1 firmware";
    case KeyResult::SendFailed: return "couldn't send to the FM-1";
  }
  return "?";
}

KeyResult enter_rom_boot(MidiLink &link, std::string &identity) {
  std::vector<uint8_t> query;
  sysex_packets(kIdentityQuery, sizeof kIdentityQuery, query);
  identity.clear();
  link.drain();
  for (int t = 0; t < kQueryTries && identity.empty(); ++t) {
    if (!link.send(query.data(), query.size())) return KeyResult::SendFailed;
    std::vector<uint8_t> msg;
    // Other SysEx the FM-1 sends meanwhile is skipped, a bounded number of times.
    for (int k = 0; k < 16 && identity.empty() && link.receive_sysex(msg, kQueryTimeoutMs); ++k)
      identity = parse_identity(msg);
  }
  if (identity.empty()) return KeyResult::NoAnswer;
  std::string lower = identity;
  for (char &c : lower) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  if (lower.compare(0, 4, "ota-") == 0) return KeyResult::UpdateMode;
  // fm1_unbrick.py accepts FM-1_ followed by digits.
  bool fm1 = identity.size() > 5 && identity.compare(0, 5, "FM-1_") == 0;
  for (size_t i = 5; fm1 && i < identity.size(); ++i) fm1 = identity[i] >= '0' && identity[i] <= '9';
  if (!fm1) return KeyResult::NotFm1;
  for (int i = 0; i < kKeySends; ++i) {
    if (i) link.sleep_ms(kKeyGapMs);
    if (!link.send(kKeyTransfer, sizeof kKeyTransfer)) {
      // The unit leaves the bus once it takes the key, so a later send may fail.
      if (i == 0) return KeyResult::SendFailed;
      break;
    }
  }
  return KeyResult::Sent;
}

}  // namespace fm1_loader
