// The board's profile over Computer mode's USB serial link: a page or
// tools/board_profile.py reads it, sends a new one, or has the board forget it.
//
// Frames, both ways: 'O' 'P', a type byte, the payload's length (u16 LE), the
// payload, then CRC-16/XMODEM (u16 LE) of type, length and payload. The same
// shape as the add-firmware frames (fm1_loader/receive.h, 'O' 'R'), on the same
// port: each side skips the other's frames, and the board's log.
//
// The page asks:          The board answers:
//   'G'                     'g' the stored profile's bytes (board_blob.h), or nothing when none is stored
//   'S' a profile           's' once it's checked and stored; it applies at the next start
//   'F'                     'f' once the stored profile is gone: the next start uses the build's own
//   'B'                     'b', then the board restarts for normal use
// Any request may get 'e': an error code (BlobError, or kPsramMismatch / kStoreFailed /
// kBadRequest), then why in words.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <vector>

#include "board_blob.h"

constexpr size_t kProfilePayloadMax = 128;

std::vector<uint8_t> profile_frame(uint8_t type, const uint8_t *payload, size_t len);

// Finds 'O' 'P' frames in a byte stream.
class ProfileFrameReader {
 public:
  // True when b completes a valid frame; type() and payload() then hold it.
  bool feed(uint8_t b);
  uint8_t type() const { return type_; }
  const std::vector<uint8_t> &payload() const { return payload_; }

 private:
  std::vector<uint8_t> buf_;
  uint8_t type_ = 0;
  std::vector<uint8_t> payload_;
};

class ProfileLink {
 public:
  // Codes past BlobError's.
  static constexpr uint8_t kPsramMismatch = 100;  // the profile's PSRAM mode isn't this build's
  static constexpr uint8_t kStoreFailed = 101;    // the settings refused the write
  static constexpr uint8_t kBadRequest = 102;     // a request this board doesn't have

  struct Hooks {
    std::function<size_t(uint8_t *out, size_t max)> load;  // the stored blob's bytes, or 0
    std::function<bool(const uint8_t *data, size_t len)> save;
    std::function<void()> forget;
    bool build_quad_psram;
  };
  explicit ProfileLink(Hooks hooks) : hooks_(std::move(hooks)) {}
  // Bytes from the computer; replies are appended to out.
  void feed(const uint8_t *data, size_t len, std::vector<uint8_t> &out);
  // True once a 'b' reply has been queued: write out, then restart.
  bool restart_due() const { return restart_due_; }

 private:
  void handle(uint8_t type, const std::vector<uint8_t> &p, std::vector<uint8_t> &out);
  Hooks hooks_;
  ProfileFrameReader reader_;
  bool restart_due_ = false;
};
