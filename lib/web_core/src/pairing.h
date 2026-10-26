// Pairing a browser with the hub: the device shows a PIN, the browser sends it
// back, and gets a token that every write must carry. Plain C++; the firmware
// supplies time and randomness.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace web_core {

enum class PairResult : uint8_t { Ok, Wrong, Expired, Locked, NoAttempt };

class Pairing {
 public:
  static constexpr uint32_t kLifetimeMs = 60000;
  static constexpr uint8_t kMaxTries = 3;

  // Starts (or restarts) an attempt with a PIN made from random's low six digits.
  void start(uint32_t now_ms, uint32_t random);
  // The PIN to show on screen, or "" when no attempt is open.
  const char *pin(uint32_t now_ms) const;
  // Ok ends the attempt; the third Wrong ends it as Locked.
  PairResult submit(uint32_t now_ms, const char *pin);

 private:
  bool open(uint32_t now_ms) const { return active_ && now_ms - started_ms_ < kLifetimeMs; }
  char pin_[7] = "";
  uint32_t started_ms_ = 0;
  uint8_t tries_ = 0;
  bool active_ = false;
};

constexpr size_t kTokenBytes = 16;

// Formats a token as 32 lower-case hex digits.
void token_to_hex(const uint8_t token[kTokenBytes], char out[2 * kTokenBytes + 1]);

// Tokens of paired browsers; the oldest is dropped when a fifth pairs.
struct TokenStore {
  static constexpr int kMax = 4;
  uint8_t tokens[kMax][kTokenBytes];
  uint8_t count = 0;

  void add(const uint8_t token[kTokenBytes]);
  // True for a stored token given as 32 hex digits. Compares in constant time.
  bool check(const char *hex) const;
};

}  // namespace web_core
