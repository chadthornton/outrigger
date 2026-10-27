// Putting a running FM-1 into its ROM bootloader over USB-MIDI, ported from
// the Unbricker's softkey (fm1_unbrick.py cmd_softkey, midi_handshake): ask
// the firmware's identity, refuse anything that isn't a plain FM-1 firmware,
// then send the key F0 22 24 35 7D F7 as ONE 8-byte USB transfer (stock
// compares whole transfers), three times 20 ms apart. Only the Loader calls
// this, in Load mode; the Router drops every SysEx.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace fm1_loader {

// 04 F0 22 24  07 35 7D F7 on cable 0.
extern const uint8_t kKeyTransfer[8];
// F0 00 32 45 00 00 00 40 7F F7, after ChoralRoot's tools/fm1_install.py.
extern const uint8_t kIdentityQuery[10];
constexpr int kQueryTries = 3;
constexpr uint32_t kQueryTimeoutMs = 1000;
constexpr int kKeySends = 3;
constexpr uint32_t kKeyGapMs = 20;
constexpr uint32_t kRomWaitMs = 15000;  // fm1_unbrick.py SOFT_KEY_WAIT

// A whole SysEx message (F0 ... F7) as USB-MIDI event packets on cable 0,
// appended to out.
void sysex_packets(const uint8_t *msg, size_t len, std::vector<uint8_t> &out);

// Collects one SysEx message from USB-MIDI packets (code index cin and the
// three data bytes). True when msg holds a complete F0 ... F7 message.
class SysexAssembler {
 public:
  bool feed(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2, std::vector<uint8_t> &msg);

 private:
  std::vector<uint8_t> buf_;
  bool active_ = false;
};

// The identity text of an FM-1's answer to the query ("FM-1_714",
// "ota-FM-1_015"), or empty if it isn't one.
std::string parse_identity(const std::vector<uint8_t> &sysex);

// The FM-1's MIDI port as the key entry sees it.
class MidiLink {
 public:
  virtual ~MidiLink() = default;
  virtual bool send(const uint8_t *packets, size_t len) = 0;  // one USB transfer
  // Waits up to timeout_ms for the next whole SysEx message from the FM-1.
  virtual bool receive_sysex(std::vector<uint8_t> &msg, uint32_t timeout_ms) = 0;
  virtual void sleep_ms(uint32_t ms) = 0;
  virtual void drain() = 0;  // drop anything already received
};

enum class KeyResult {
  Sent,        // the key went out at least once
  NoAnswer,    // no identity answer: not on its normal screen, or not an FM-1
  UpdateMode,  // "ota-…": an update is in progress; finish or cancel it first
  NotFm1,      // the answer isn't FM-1_NNN
  SendFailed,  // couldn't send the query or the first key
};
const char *key_result_text(KeyResult r);

// identity gets the firmware's answer when there is one.
KeyResult enter_rom_boot(MidiLink &link, std::string &identity);

}  // namespace fm1_loader
