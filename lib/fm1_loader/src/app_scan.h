// Reads which M-VAVE stock build an FM-1 app area holds, from its plaintext.
//
// The app area (kAppStart..kAppEnd) is JieLi ENC-scrambled with the chip key,
// keyed per 32-byte block by (offset in the area >> 2). Its plaintext names the
// firmware: stock carries one bare "FM-1_0NN" (V15: "FM-1_015"); every
// Felucca-lineage build carries its own "FM-1_NNN", then "ota-FM-1_015" and
// "FELUCCA". A factory-programmed FM-1 needn't be byte-identical to M-VAVE's
// package (issue #39), so this is how the loader recognises stock that no
// package matches. Plain C++, fed one sector at a time (no 580 KiB buffer).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace fm1_loader {

// Decrypts len bytes of app area in place; offset is where buf starts, counted
// from kAppStart, and must be a multiple of 32.
void decrypt_app(uint8_t *buf, size_t len, uint32_t offset);

class StockScan {
 public:
  // The app area's bytes as stored in flash, in order, any chunk size that is a
  // multiple of 32.
  void feed(const uint8_t *data, size_t len);
  // "FM-1_0NN" when the area reads as one M-VAVE stock build, else "".
  std::string identity() const;

 private:
  void scan(const uint8_t *p, size_t len);
  uint32_t offset_ = 0;
  uint8_t tail_[16] = {};  // the last plaintext bytes before this chunk
  size_t tail_len_ = 0;
  char id_[9] = "";
  bool several_ = false;   // two different bare identities
  bool felucca_ = false;   // a "FELUCCA" or "ota-FM-1_" marker
  bool table_ = false;     // the area starts with a JieLi file table
  uint32_t end_ = 0;       // where its files end, in the area
};

}  // namespace fm1_loader
