// Board-independent MIDI logic: formatting messages for a small display and a
// fixed-size log that the USB callback fills and the UI loop drains.
// Plain C++ with no Arduino or ESP-IDF headers, so tests/ builds it on the Mac.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace midi_core {

// Longest line format_message() produces, plus the terminator. Sized for a
// 240 px wide screen at 12 px per character (20 columns).
constexpr size_t kLineLen = 21;

// Writes a short, human-readable description of one MIDI message into out,
// for example "01 ON  C4  v100" or "01 CC  74 = 63". Channel numbers are 1-based.
// raw/raw_len are the message bytes (used for SysEx and other system messages);
// they may be null when status/data1/data2 say everything.
void format_message(uint8_t status, uint8_t data1, uint8_t data2,
                    const uint8_t *raw, size_t raw_len,
                    char out[kLineLen]);

// Writes a note name such as "C4" or "F#-1" (MIDI note 60 = C4).
void note_name(uint8_t note, char out[5]);

// Which side of the routing a USB device sits on. The FM-1 (stock or a
// Felucca-based firmware) is the target; every other MIDI device is a controller.
enum class Role { Controller, Target };
Role device_role(uint16_t vid, uint16_t pid);

// Decides whether a message from a controller goes to the target and, if so,
// writes the 4-byte USB-MIDI packet to send (cable 0). Only channel voice
// messages (0x80-0xEF) from the controller's first cable pass, and only when
// the packet's code index matches the status byte. Every system message,
// SysEx included, is dropped: the FM-1's ROM-boot key and firmware installers
// arrive as SysEx, and nothing a controller sends should reach them.
bool route_to_target(uint8_t cable, uint8_t code_index, uint8_t status, uint8_t data1,
                     uint8_t data2, uint8_t packet[4]);

// Tracks which USB address is the FM-1 and which are controllers, and decides
// where each incoming message goes. Call it from one task only (the USB task).
class Router {
 public:
  void device_connected(uint8_t address, uint16_t vid, uint16_t pid);
  void device_disconnected(uint8_t address);
  bool is_target(uint8_t address) const;
  // True when address is connected, with its role (what /status shows).
  bool knows(uint8_t address, Role &role) const;
  // Returns the address to send packet to, or 0 to send nothing. Never returns
  // an address that isn't a connected FM-1, so a caller can't fall through to
  // a library's "any device" default.
  // When it returns 0 and why is non-null, *why names the reason for the screen:
  // 'n' no FM-1 connected, 'u' unknown source, 't' source is the FM-1,
  // 'c' cable other than 0, 'k' code index doesn't match, 's' system message.
  uint8_t route(uint8_t source, uint8_t cable, uint8_t code_index, uint8_t status,
                uint8_t data1, uint8_t data2, uint8_t packet[4], char *why = nullptr) const;
  // Outrigger Synth builds: whether the board's own synth plays this message.
  // The same rules as route(), less the FM-1: a connected controller's channel
  // voice message on cable 0, whether an FM-1 is connected or not.
  bool to_synth(uint8_t source, uint8_t cable, uint8_t code_index, uint8_t status,
                uint8_t data1, uint8_t data2) const;

 private:
  // route()'s reasons other than 'n', or 0 when a controller's message passes.
  char refuse(uint8_t source, uint8_t cable, uint8_t code_index, uint8_t status,
              uint8_t data1, uint8_t data2, uint8_t packet[4]) const;
  static constexpr int kMaxAddress = 127;  // USB device addresses are 1-127
  bool known_[kMaxAddress + 1] = {};
  Role role_[kMaxAddress + 1] = {};
  uint8_t target_ = 0;  // 0 = no FM-1 connected
};

// Single-producer, single-consumer ring of formatted lines. When full, the
// oldest line is dropped so the display always shows the latest activity.
template <size_t N>
class LineLog {
 public:
  void push(const char *line) {
    size_t i = 0;
    for (; i + 1 < kLineLen && line[i]; ++i) lines_[head_][i] = line[i];
    lines_[head_][i] = '\0';
    head_ = (head_ + 1) % N;
    if (count_ < N) ++count_;
    ++total_;
  }
  size_t count() const { return count_; }
  // Total lines ever pushed, so a UI can tell whether anything changed.
  uint32_t total() const { return total_; }
  // age 0 is the newest line.
  const char *newest(size_t age) const {
    if (age >= count_) return "";
    return lines_[(head_ + N - 1 - age) % N];
  }

 private:
  char lines_[N][kLineLen] = {};
  size_t head_ = 0;
  size_t count_ = 0;
  uint32_t total_ = 0;
};

}  // namespace midi_core
