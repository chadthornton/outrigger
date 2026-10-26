// A mock FM-1 in ROM boot at the SCSI level: a port of the Unbricker's MockScsi
// (tests/test_unbrick.py) and MockUboot (fm1_unbrick.py) plus the golden
// generator's additions (reset, loader-mode inquiry product). Flash has NOR
// semantics: erase sets 0xFF, programming can only clear bits. Every command is
// logged in the golden-vector line format of tools/gen_golden.py.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "rom.h"

namespace fm1_loader {

class MockRom : public ScsiTransport {
 public:
  MockRom(const std::vector<uint8_t> &flash, const std::vector<uint8_t> &loader_blob);
  bool execute(const uint8_t *cdb, size_t cdb_len, Dir dir, uint8_t *data, size_t len) override;

  std::vector<uint8_t> flash;
  uint16_t key = 0x980F;
  uint32_t id = 0x856014;
  uint32_t io_max = 512;
  int flaky_read = -1;    // the Nth flash read (1-based) returns one corrupted byte
  int flaky_write = -1;   // the Nth flash write programs data[0] ^ 1
  long bad_cell = -1;     // this byte never programs correctly
  int disconnect_after = -1;  // commands after this many are refused: the device is gone
  std::string loader_product = "UBOOT1.00";
  bool loader_running = false;

  std::vector<std::string> log;
  std::vector<std::string> violations;  // what the Python mock would have asserted on
  int reads = 0, writes = 0, commands = 0;
  bool was_reset = false;
  uint16_t jump_arg = 0;
  long lowest_touched = -1;

 private:
  bool handle(const uint8_t *cdb, size_t cdb_len, Dir dir, uint8_t *data, size_t len);
  void touched(uint32_t addr);
  std::vector<uint8_t> blob_;
  std::map<uint32_t, std::vector<uint8_t>> ram_;
};

}  // namespace fm1_loader
