// The test bench's sector writer (BENCH=1 builds only, never a release):
// writes whole 4 KiB sectors of FM-1-2's flash that the Loader's guard
// keeps out of reach on purpose. The bench uses it for test marks in every
// firmware's data areas, the Skiff-style fill of 0xEA000-0xFBFFF (test 1), and
// putting that area back from FM-1-2's pristine dump afterwards.
//
// Its own rules, separate from fm1_loader's guard:
// - never the boot head (0x0000-0x3FFF), the app (0x4000-0x92FFF: installs are
//   the Loader's job) or key_mac (0xFF000-0xFFFFF);
// - 0xE9000 (the unit ID) and 0xEA000-0xFBFFF (stock's resources) only with
//   protected_ok;
// - only on the FM-1 whose unit ID the caller names, and never FM-1-1 (Chad's main FM-1);
// - a dry run unless write is set: it reads every sector and reports which differ.
// Each written sector is read back and compared, with one retry, as the Loader does.
// Any stop after the FM-1 enters ROM boot resets it to its firmware.
//
// Pure C++ on fm1_loader's RomClient, so tests/test_bench_writer.cpp runs it
// against the Mac's mock FM-1.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "installer.h"
#include "rom.h"

namespace bench {

constexpr const char *kFm11Unit = "69bb070aaa87bd10";

struct Sector {
  uint32_t addr;
  std::vector<uint8_t> data;  // 4 KiB
};

struct WriteOptions {
  bool write = false;         // false: a dry run, nothing erased or written
  bool protected_ok = false;  // may touch 0xE9000 and 0xEA000-0xFBFFF
  std::string unit;           // FM-1-2's unit ID; anything else is refused
  bool key_sent = false;      // the FM-1 is in ROM boot already
  uint32_t rom_timeout_ms = 15000;
  uint32_t midi_timeout_ms = 20000;
};

enum class Stop : uint8_t {
  None, Refused, KeyNotSent, NoRom, NotWl82, LoaderBad, LoaderFailed, IdsWrong, WrongUnit, Fm11Unit,
  ReadFailed, EraseFailed, WriteFailed, VerifyFailed, MidiNotBack,
};
const char *stop_text(Stop s);

struct Report {
  Stop stop = Stop::None;
  std::string detail;
  std::string unit;   // the unit ID the FM-1 had
  int same = 0;       // sectors that already held the wanted bytes
  int differ = 0;     // sectors that didn't (a dry run stops here)
  int written = 0;    // sectors written and verified
  bool erased = false;  // an erase may have reached the FM-1: it isn't as it was
  bool ok() const { return stop == Stop::None; }
};

// Whether the sector at addr may be written; why says what refused it.
bool allowed(uint32_t addr, bool protected_ok, const char **why);

// Sends the key (unless key_sent), uploads JieLi's loader, checks the FM-1,
// then compares and (with write) writes the sectors, and resets the FM-1 to its
// firmware. Every sector is checked against allowed() before anything is sent.
Report write_sectors(fm1_loader::ScsiTransport &scsi, fm1_loader::LoaderHost &host,
                     const std::vector<uint8_t> &blob, const std::vector<Sector> &sectors, const WriteOptions &o);

}  // namespace bench
