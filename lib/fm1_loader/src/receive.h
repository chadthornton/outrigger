// Adding and removing firmware packages from the browser: in Computer mode the
// page on outrigger.music sends a Firmware Hub package over the board's USB
// serial port, and the Receiver checks it and stores it with
// Library::add_package, the same code the Mac tools use. A remove deletes one
// stored package that no FM-1 runs and no unfinished install needs; backups and
// saves are never changed or removed (docs/firmware-loader-plan.md, "Adding
// firmware from the browser"). 'I' reads what the loader knows about the FM-1
// it saw last, and writes nothing.
//
// Frames, both ways: 'O' 'R', a type byte, the payload's length (u16 LE), the
// payload, then CRC-16/XMODEM (crypto.h, u16 LE) of type, length and payload.
// Bytes outside a valid frame are skipped: the board's log shares the port.
// One request, one reply (stop and wait), each reply under 200 bytes.
//
// The page asks:                         The board answers:
//   'H'                                    'h' protocol, packages, max packages, free bytes (u32), build
//   'L' index (u8)                         'l' index, file SHA-256 (32), package id
//   'A' size (u32), SHA-256 (32),          'k' bytes received so far (u32): send from there
//       project, version, name (each \0)   'd' 1: already in the store, nothing to send
//   'D' offset (u32), up to kDataMax bytes 'k' as above; 'd' 0 once the last byte is stored
//   'X'                                    'k' 0: the transfer is dropped
//   'R' package id (protocol 2)            'r': removed, and its sectors no other package,
//                                          backup or save uses are freed. The same 'R' again
//                                          answers 'r' (the page resends when a reply is lost).
//   'I' (protocol 2)                       'i' flags (u8: 1 an FM-1 is known, 2 it has a backup,
//                                          4 an install is unfinished), head (u8: 0 V15, 1 V16,
//                                          2 other, 255 no backup), flash id (u32, 0 unknown),
//                                          installed package id \0, the FM-1's name \0
// Any request may get 'e' code (u8), then why in words for the page.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "crypto.h"
#include "layout.h"

namespace fm1_loader {

class Library;

constexpr uint8_t kReceiveProtocol = 2;  // 2: 'R' and 'I'
constexpr size_t kDataMax = 2048;                   // package bytes per 'D' frame
constexpr size_t kFrameMax = kDataMax + 4;          // the largest payload either side sends
constexpr size_t kPackageMax = 1u << 20;            // .fwsc files are 600-700 KB
// The board's Firmware page shows this many rows (host_ui.h kLoaderItems).
constexpr size_t kMaxPackages = 10;
// Room an add must leave free: one whole FM-1 backup and its records, since
// every switch backs up first, plus the package's own sectors and manifest.
constexpr uint64_t kBackupReserve = uint64_t(kSectors + 16) * kSector;
constexpr uint64_t kPackageRoom = uint64_t(kAppSectors + 4) * kSector;
// A transfer with nothing new for this long is dropped.
constexpr uint32_t kIdleMs = 10000;

enum class ReceiveError : uint8_t {
  None = 0,
  BadRequest = 1,    // a frame the protocol doesn't have, or a malformed one
  TooBig = 2,        // empty, or larger than kPackageMax
  NoProject = 3,     // no catalog id: files of your own aren't taken yet
  Blocked = 4,       // a project the loader can't install yet (decide_data)
  IdTaken = 5,       // a different file is stored under this name
  TooMany = 6,       // kMaxPackages already stored
  StoreFull = 7,     // the add would leave less than kBackupReserve free
  NoMemory = 8,      // no room for the file in RAM
  NotReceiving = 9,  // data with no transfer started
  HashMismatch = 10, // the bytes received aren't the file the page checked
  BadPackage = 11,   // not an FM-1 package lib/fm1_loader accepts
  StoreFailed = 12,  // the store refused a write
  InUse = 13,        // remove: an FM-1 runs it, or an unfinished install needs it
  Kept = 14,         // remove: stock, which always stays
  NoSuchPackage = 15,  // remove: no package by that id
  Busy = 16,         // remove: a transfer is open
};

std::vector<uint8_t> encode_frame(uint8_t type, const uint8_t *payload, size_t len);

// Finds frames in a byte stream.
class FrameReader {
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

// What the Computer mode screen says about adding.
struct ReceiveStatus {
  enum class State { Idle, Receiving, Adding, Added, Failed } state = State::Idle;
  std::string label;  // "Felucca 1.8.0"
  std::string why;    // Failed: the reason
  uint32_t got = 0, total = 0;
};

class Receiver {
 public:
  // free_bytes: the store's free space now.
  Receiver(Library &lib, std::function<uint64_t()> free_bytes, std::string build);
  ~Receiver();
  // Called whenever the status changes (on the thread that calls feed or tick).
  void on_status(std::function<void(const ReceiveStatus &)> f) { on_status_ = std::move(f); }
  // Bytes from the computer, at now_ms; replies are appended to out.
  void feed(const uint8_t *data, size_t len, uint32_t now_ms, std::vector<uint8_t> &out);
  // Drops a transfer that has been idle for kIdleMs.
  void tick(uint32_t now_ms);

 private:
  void handle(uint8_t type, const std::vector<uint8_t> &p, uint32_t now_ms, std::vector<uint8_t> &out);
  void begin(const std::vector<uint8_t> &p, uint32_t now_ms, std::vector<uint8_t> &out);
  void data(const std::vector<uint8_t> &p, uint32_t now_ms, std::vector<uint8_t> &out);
  void store(std::vector<uint8_t> &out);
  void remove(const std::vector<uint8_t> &p, std::vector<uint8_t> &out);
  void info(std::vector<uint8_t> &out);
  void refuse(std::vector<uint8_t> &out, ReceiveError e, const std::string &why);
  void drop();
  void set_state(ReceiveStatus::State s, const std::string &why = "");

  Library &lib_;
  std::function<uint64_t()> free_bytes_;
  std::string build_;
  std::function<void(const ReceiveStatus &)> on_status_;
  FrameReader reader_;
  ReceiveStatus status_;

  // The transfer under way.
  struct Buffer;
  std::unique_ptr<Buffer> buf_;
  uint32_t last_ms_ = 0;
  // The last transfer's final reply, sent again if the page repeats its last
  // frame (the reply was lost).
  uint32_t last_size_ = 0;
  std::vector<uint8_t> last_reply_;
  // The last package removed, so a resent 'R' answers the same.
  std::string last_removed_;
};

}  // namespace fm1_loader
