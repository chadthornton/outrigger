#include "midi_core.h"

#include <stdio.h>

namespace midi_core {

static const char *const kNoteNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                           "F#", "G",  "G#", "A",  "A#", "B"};

void note_name(uint8_t note, char out[5]) {
  note &= 0x7F;
  snprintf(out, 5, "%s%d", kNoteNames[note % 12], note / 12 - 1);
}

void format_message(uint8_t status, uint8_t data1, uint8_t data2,
                    const uint8_t *raw, size_t raw_len, char out[kLineLen]) {
  char note[5];
  const unsigned ch = (status & 0x0F) + 1;

  switch (status & 0xF0) {
    case 0x80:
      note_name(data1, note);
      snprintf(out, kLineLen, "%02u OFF %-4s v%u", ch, note, data2);
      return;
    case 0x90:
      note_name(data1, note);
      // Note-on with velocity 0 is a note-off by the MIDI spec.
      if (data2 == 0)
        snprintf(out, kLineLen, "%02u OFF %-4s v0", ch, note);
      else
        snprintf(out, kLineLen, "%02u ON  %-4s v%u", ch, note, data2);
      return;
    case 0xA0:
      note_name(data1, note);
      snprintf(out, kLineLen, "%02u AT  %-4s %u", ch, note, data2);
      return;
    case 0xB0:
      snprintf(out, kLineLen, "%02u CC  %u = %u", ch, data1 & 0x7F, data2 & 0x7F);
      return;
    case 0xC0:
      snprintf(out, kLineLen, "%02u PC  %u", ch, data1 & 0x7F);
      return;
    case 0xD0:
      snprintf(out, kLineLen, "%02u CP  %u", ch, data1 & 0x7F);
      return;
    case 0xE0: {
      const int bend = (((data2 & 0x7F) << 7) | (data1 & 0x7F)) - 8192;
      snprintf(out, kLineLen, "%02u PB  %+d", ch, bend);
      return;
    }
  }

  // System messages. SysEx arrives in 3-byte USB-MIDI chunks, so show the
  // bytes we were given rather than trying to reassemble the whole message.
  if (raw && raw_len > 0) {
    size_t pos = snprintf(out, kLineLen, "SYS");
    for (size_t i = 0; i < raw_len && pos + 3 < kLineLen; ++i)
      pos += snprintf(out + pos, kLineLen - pos, " %02X", raw[i]);
    return;
  }
  snprintf(out, kLineLen, "SYS %02X", status);
}

Role device_role(uint16_t vid, uint16_t pid) {
  if (vid == 0x4C4A && pid == 0xC755) return Role::Target;  // FM-1, stock firmware
  if (vid == 0x1209 && pid == 0x0001) return Role::Target;  // FM-1, Felucca/Skiff
  if (vid == 0x3654 && pid == 0x7756) return Role::Target;  // FM-1, M-VAVE Open / Big Screen
  return Role::Controller;
}

bool can_switch_firmware(uint16_t vid, uint16_t pid) {
  return (vid == 0x4C4A && pid == 0xC755) || (vid == 0x1209 && pid == 0x0001);
}

bool route_to_target(uint8_t cable, uint8_t code_index, uint8_t status, uint8_t data1,
                     uint8_t data2, uint8_t packet[4]) {
  if (cable != 0) return false;
  if (status < 0x80 || status >= 0xF0) return false;
  // For channel messages the code index equals the status's high nibble; any
  // other pairing is padding, a SysEx fragment or a single stray byte.
  if (code_index != status >> 4) return false;

  const uint8_t type = status & 0xF0;
  const bool two_bytes = type == 0xC0 || type == 0xD0;
  packet[0] = type >> 4;  // cable 0, code index = message type
  packet[1] = status;
  packet[2] = data1 & 0x7F;
  packet[3] = two_bytes ? 0 : data2 & 0x7F;
  return true;
}

void Router::device_connected(uint8_t address, uint16_t vid, uint16_t pid) {
  if (address == 0 || address > kMaxAddress) return;
  known_[address] = true;
  role_[address] = device_role(vid, pid);
  if (role_[address] == Role::Target) target_ = address;
}

void Router::device_disconnected(uint8_t address) {
  if (address == 0 || address > kMaxAddress) return;
  known_[address] = false;
  if (target_ == address) target_ = 0;
}

bool Router::is_target(uint8_t address) const {
  return target_ != 0 && address == target_;
}

uint8_t Router::route(uint8_t source, uint8_t cable, uint8_t code_index, uint8_t status,
                      uint8_t data1, uint8_t data2, uint8_t packet[4], char *why) const {
  char reason = 0;
  if (target_ == 0) reason = 'n';
  else if (source == 0 || source > kMaxAddress || !known_[source]) reason = 'u';
  else if (role_[source] != Role::Controller) reason = 't';
  else if (cable != 0) reason = 'c';
  else if (status < 0x80 || status >= 0xF0) reason = 's';
  else if (code_index != status >> 4) reason = 'k';
  else if (!route_to_target(cable, code_index, status, data1, data2, packet)) reason = 's';
  if (reason) {
    if (why) *why = reason;
    return 0;
  }
  return target_;
}

}  // namespace midi_core
