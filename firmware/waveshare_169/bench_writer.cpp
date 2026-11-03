// Compiled into BENCH=1 builds and the Mac tests only.
#if !defined(ARDUINO) || BENCH
#include "bench_writer.h"

#include <stdio.h>
#include <string.h>

#include "crypto.h"
#include "layout.h"
#include "library.h"

namespace bench {

using namespace fm1_loader;

namespace {


// The loader's flash I/O size, as the Installer picks it (installer.cpp chunk_for).
uint32_t chunk_for(uint32_t io_max) {
  if (io_max < 16 || io_max > 0xFFFF) io_max = 512;
  if (io_max <= kSector && kSector % io_max == 0) return io_max;
  uint32_t c = 16;
  while (c * 2 <= io_max && c * 2 <= kSector) c *= 2;
  return c;
}

std::string at(const char *what, uint32_t addr) {
  char buf[48];
  snprintf(buf, sizeof buf, "%s 0x%05X", what, unsigned(addr));
  return buf;
}

bool read_sector(RomClient &rom, uint32_t addr, uint32_t chunk, uint8_t *out) {
  for (uint32_t off = 0; off < kSector; off += chunk)
    if (!rom.flash_read(addr + off, out + off, uint16_t(chunk))) return false;
  return true;
}

}  // namespace

const char *stop_text(Stop s) {
  switch (s) {
    case Stop::None: return "ok";
    case Stop::Refused: return "a sector is out of bounds";
    case Stop::KeyNotSent: return "couldn't send the ROM-boot key";
    case Stop::NoRom: return "the FM-1 didn't enter ROM boot";
    case Stop::NotWl82: return "not a WL82 in ROM boot";
    case Stop::LoaderBad: return "the loader blob is damaged";
    case Stop::LoaderFailed: return "the loader didn't start";
    case Stop::IdsWrong: return "chip key or flash ID isn't the FM-1's";
    case Stop::WrongUnit: return "not FM-1-2";
    case Stop::Fm11Unit: return "that is FM-1-1, Chad's main FM-1";
    case Stop::ReadFailed: return "a read failed";
    case Stop::EraseFailed: return "an erase failed";
    case Stop::WriteFailed: return "a write failed";
    case Stop::VerifyFailed: return "a sector didn't read back as written";
    case Stop::MidiNotBack: return "the FM-1 didn't come back as MIDI";
  }
  return "?";
}

bool allowed(uint32_t addr, bool protected_ok, const char **why) {
  const char *dummy;
  if (!why) why = &dummy;
  *why = "";
  if (addr % kSector) return *why = "not on a sector boundary", false;
  if (addr >= kFlashSize) return *why = "past the flash", false;
  if (addr < kHeadEnd) return *why = "the boot head", false;
  if (addr < kAppEnd) return *why = "the app (installs are the Loader's job)", false;
  if (addr >= kKeyMacStart) return *why = "key_mac", false;
  if (addr >= kUnitSector && addr < kFactoryEnd && !protected_ok)
    return *why = addr == kUnitSector ? "the unit ID sector (needs protected_ok)" : "stock's resources (needs protected_ok)",
           false;
  return true;
}

Report write_sectors(ScsiTransport &scsi, LoaderHost &host, const std::vector<uint8_t> &blob,
                     const std::vector<Sector> &sectors, const WriteOptions &o) {
  Report r;
  auto stop = [&r](Stop s, std::string detail = "") {
    r.stop = s;
    r.detail = std::move(detail);
    return r;
  };
  // Every sector is judged before the FM-1 hears anything.
  for (const Sector &s : sectors) {
    const char *why;
    if (s.data.size() != kSector) return stop(Stop::Refused, at("not 4 KiB:", s.addr));
    if (!allowed(s.addr, o.protected_ok, &why)) return stop(Stop::Refused, at(why, s.addr));
  }
  if (o.unit.empty()) return stop(Stop::WrongUnit, "no bench unit named");
  if (o.unit == kFm11Unit) return stop(Stop::Fm11Unit);
  if (!kWritesEnabled && o.write) return stop(Stop::WriteFailed, "this build can't write (FM1_LOADER_WRITES)");
  if (hex(sha256(blob.data(), blob.size())) != kLoaderSha256) return stop(Stop::LoaderBad);
  if (loader_needs_host_scramble(true, true)) return stop(Stop::LoaderBad, "the loader would need scrambling");

  RomClient rom(scsi);
  if (!o.key_sent && !host.send_rom_boot_key()) return stop(Stop::KeyNotSent);
  if (!host.wait_for_rom_device(o.rom_timeout_ms)) return stop(Stop::NoRom);
  // From here the FM-1 is in ROM boot: every way out sends it back to its
  // firmware. The writer never touches the app, so even a sector left
  // half-written leaves a firmware that starts.
  auto home = [&](Stop s, std::string detail = "") {
    rom.run_app(kResetArg);  // the device may reset before it answers
    return stop(s, std::move(detail));
  };
  std::string vendor, product, revision;
  if (!rom.inquiry(vendor, product, revision)) return home(Stop::NoRom, "inquiry");
  for (char &c : vendor) c = char(c >= 'a' && c <= 'z' ? c - 32 : c);
  if (vendor != "WL82") return home(Stop::NotWl82, vendor);
  uint8_t type;
  uint32_t id;
  if (product == "UBOOT1.00") {
    for (size_t off = 0; off < blob.size(); off += kLoaderBlock) {
      const size_t n = blob.size() - off < kLoaderBlock ? blob.size() - off : kLoaderBlock;
      if (!rom.mem_write(kLoaderAddr + uint32_t(off), blob.data() + off, uint16_t(n)))
        return home(Stop::LoaderFailed, at("upload", uint32_t(off)));
    }
    if (!rom.mem_jump(kLoaderAddr, kLoaderArg)) return home(Stop::LoaderFailed, "jump");
  } else if (!rom.online_device(type, id)) {
    return home(Stop::LoaderFailed, "no loader answers");
  }
  uint32_t io_max = 512;
  if (!rom.usb_buffer_size(io_max)) io_max = 512;
  const uint32_t chunk = chunk_for(io_max);
  uint16_t key;
  if (!rom.chip_key(key) || !rom.online_device(type, id)) return home(Stop::ReadFailed, "chip key / flash ID");
  if (key != kChipKey || id != kFlashId) return home(Stop::IdsWrong);

  std::vector<uint8_t> buf(kSector);
  if (!read_sector(rom, kUnitSector, chunk, buf.data())) return home(Stop::ReadFailed, at("read", kUnitSector));
  r.unit = unit_id_of(buf.data());
  if (r.unit == kFm11Unit || r.unit != o.unit)
    return home(r.unit == kFm11Unit ? Stop::Fm11Unit : Stop::WrongUnit, "FM-1 unit " + r.unit);

  // One sector at a time, compare then write, so the first erase comes early:
  // in checkpoint 3 the FM-1 restarted itself ~8.5 s after the loader started
  // when no erase came. (A long dry run can still run into that.)
  for (const Sector &s : sectors) {
    if (!read_sector(rom, s.addr, chunk, buf.data())) return home(Stop::ReadFailed, at("read", s.addr));
    if (buf == s.data) {
      ++r.same;
      continue;
    }
    ++r.differ;
    if (!o.write) continue;
    bool good = false;
    for (int attempt = 0; attempt < 2 && !good; ++attempt) {
      const bool erased = rom.erase_sector(s.addr);
      if (erased || rom.reached_device()) r.erased = true;
      if (!erased) return home(Stop::EraseFailed, at("erase", s.addr));
      for (uint32_t off = 0; off < kSector; off += chunk)
        if (!rom.flash_write(s.addr + off, s.data.data() + off, uint16_t(chunk)))
          return home(Stop::WriteFailed, at("write", s.addr + off));
      if (!read_sector(rom, s.addr, chunk, buf.data())) return home(Stop::ReadFailed, at("read back", s.addr));
      good = buf == s.data;
    }
    if (!good) return home(Stop::VerifyFailed, at("verify", s.addr));
    ++r.written;
  }
  rom.run_app(kResetArg);  // the FM-1 may reset before it answers
  if (!host.wait_for_midi_return(o.midi_timeout_ms)) return stop(Stop::MidiNotBack);
  return r;
}

}  // namespace bench

#endif  // BENCH
