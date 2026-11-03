// A board profile on the wire and in the board's settings: the page sends it in
// Computer mode, the board checks it and keeps it (board_store.h). A fixed
// little-endian layout, so the board reads it with no parser:
//
//   'O' 'B' 'P' version(1)   u16 length (the whole blob)   u8 tag_id
//   u8 flags: bit 0 ips, 1 restart on power-on, 2 quad PSRAM, 3-4 screen shape
//   u16 width, height, corner radius   u8 font boost
//   u8 panel, touch, expander drivers (board_config_types.h's enums)
//   i16 lcd dc, cs, sck, mosi, miso, rst, backlight   u32 SPI Hz
//   i8 spi mode   u8 rotation   i8 bgr, mirror x, mirror y   i16 offsets x4
//   u8 touch address   i16 touch sda, scl, rst, int   i8 swap xy, invert x, invert y
//   u8 expander address   i16 hold low x4, hold high x4   char name[40]
//   u16 CRC-16/XMODEM of everything before it
//
// tools/boards.py writes the same bytes (blob), and both run the cases in
// tests/boards/blobs.tsv. Pure C++: tested on the Mac (tests/test_board_blob.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_config_types.h"
#include "board_profile.h"

constexpr uint8_t kBlobVersion = 1;
constexpr size_t kBlobNameMax = 40;
constexpr size_t kBlobSize = 120;  // the layout above; board_blob.cpp checks it

struct BoardBlob {
  uint8_t tag_id = 0;
  bool quad_psram = false;
  char name[kBlobNameMax] = {};
  uint16_t width = 0, height = 0, corner_radius = 0;
  ScreenShape shape = ScreenShape::Rect;
  uint8_t font_boost = 0;
  BoardConfig config = {};
  // The screen as host_ui wants it; name points into this blob.
  BoardProfile profile() const;
};

// Codes shared with tools/boards.py (BLOB_ERRORS): change them together.
enum class BlobError : uint8_t {
  Ok = 0,
  Format = 1,      // not a profile: magic, version or length
  Crc = 2,         // damaged on the way
  Driver = 3,      // a panel, touch or expander this firmware doesn't drive
  PinMissing = 4,  // a pin the drivers need is none
  PinNotOnChip = 5,
  PinUsb = 6,      // GPIO19/20: the USB host's
  PinFlash = 7,    // GPIO26-32
  PinPsram = 8,    // GPIO33-37 with octal PSRAM
  PinTwice = 9,
  ExioNoExpander = 10,
  Screen = 11,     // size, shape or corner radius
  Field = 12,      // rotation, SPI mode, a tri-state or the font boost out of range
  I2cAddress = 13,
  Name = 14,       // empty, or not NUL-terminated
};

// CRC-16/XMODEM: the blob's own check, and the profile link's frames'.
uint16_t board_crc16(const uint8_t *data, size_t len);

BlobError board_blob_decode(const uint8_t *data, size_t len, BoardBlob &out);
// The rules tools/boards.py check() applies; why gets a sentence for the page.
BlobError board_blob_check(const BoardBlob &b, char *why, size_t why_len);
// The blob's bytes, kBlobSize of them; returns 0 if out is too small.
size_t board_blob_encode(const BoardBlob &b, uint8_t *out, size_t out_len);
