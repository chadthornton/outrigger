#include "pairing.h"

#include <stdio.h>
#include <string.h>

namespace web_core {

void Pairing::start(uint32_t now_ms, uint32_t random) {
  snprintf(pin_, sizeof pin_, "%06lu", static_cast<unsigned long>(random % 1000000));
  started_ms_ = now_ms;
  tries_ = 0;
  active_ = true;
}

const char *Pairing::pin(uint32_t now_ms) const { return open(now_ms) ? pin_ : ""; }

PairResult Pairing::submit(uint32_t now_ms, const char *pin) {
  if (!active_) return PairResult::NoAttempt;
  if (!open(now_ms)) {
    active_ = false;
    return PairResult::Expired;
  }
  if (pin && strlen(pin) == 6 && strcmp(pin, pin_) == 0) {
    active_ = false;
    return PairResult::Ok;
  }
  if (++tries_ >= kMaxTries) {
    active_ = false;
    return PairResult::Locked;
  }
  return PairResult::Wrong;
}

void token_to_hex(const uint8_t token[kTokenBytes], char out[2 * kTokenBytes + 1]) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < kTokenBytes; ++i) {
    out[2 * i] = kHex[token[i] >> 4];
    out[2 * i + 1] = kHex[token[i] & 0xF];
  }
  out[2 * kTokenBytes] = 0;
}

static int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void TokenStore::add(const uint8_t token[kTokenBytes]) {
  if (count == kMax) {
    memmove(tokens[0], tokens[1], (kMax - 1) * kTokenBytes);
    --count;
  }
  memcpy(tokens[count++], token, kTokenBytes);
}

bool TokenStore::check(const char *hex) const {
  if (!hex || strlen(hex) != 2 * kTokenBytes) return false;
  uint8_t given[kTokenBytes];
  for (size_t i = 0; i < kTokenBytes; ++i) {
    const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    given[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  bool found = false;
  for (int t = 0; t < count; ++t) {
    uint8_t diff = 0;
    for (size_t i = 0; i < kTokenBytes; ++i) diff |= tokens[t][i] ^ given[i];
    found |= diff == 0;
  }
  return found;
}

}  // namespace web_core
