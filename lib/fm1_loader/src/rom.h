// The JieLi ROM-boot protocol (UBOOT1.00 plus the wl82 USB loader), as vendor
// SCSI commands over USB mass storage. Command layouts are jl-uboot-tool's
// (jltech/uboot.py, JL_UBOOT and JL_LoaderV2). Only the commands the FM-1 loader
// needs exist here: there is deliberately no chip erase and no block erase.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

#ifndef FM1_LOADER_WRITES
#define FM1_LOADER_WRITES 0
#endif

namespace fm1_loader {

// The lowest-level lock on writing the FM-1's flash. Off unless the build
// defines FM1_LOADER_WRITES=1: the Mac tests do; every firmware build stays
// read-only until hardware checkpoint 3. Locked, RomClient refuses erase and
// write without sending anything, and the installer refuses installs and
// resumes before it sends the key.
constexpr bool kWritesEnabled = FM1_LOADER_WRITES != 0;

// One SCSI command with an optional data phase. The firmware implements it on
// EspUsbHost's mass-storage path; the tests use a mock device.
class ScsiTransport {
 public:
  enum class Dir { None, In, Out };
  virtual ~ScsiTransport() = default;
  // False when the device refuses the command or is gone.
  virtual bool execute(const uint8_t *cdb, size_t cdb_len, Dir dir, uint8_t *data, size_t len) = 0;
  // False when the last failed command never reached the device (it was gone
  // before it was sent), so it can't have done anything.
  virtual bool reached_device() const { return true; }
};

constexpr uint32_t kLoaderAddr = 0x1C02000;  // usb-loaders.yaml, wl82
constexpr size_t kLoaderBlock = 512;
constexpr uint16_t kLoaderArg = 1;           // SPI NOR target, jl-uboot-tool's default
constexpr uint32_t kChipKeyArg = 0xAC6900;
constexpr uint32_t kResetArg = 1;            // jl-uboot-tool `reset`
constexpr size_t kResponseLen = 16;

enum Cmd : uint16_t {
  kEraseSector = 0xFB01,
  kWriteFlash = 0xFB04,
  kWriteMemory = 0xFB06,  // UBOOT1.00 and the loader
  kJump = 0xFB08,
  kReadKey = 0xFC09,
  kOnlineDevice = 0xFC0A,
  kRunApp = 0xFC0C,       // run app / reset
  kUsbBufferSize = 0xFC14,
  kReadFlash = 0xFD05,
};

class RomClient {
 public:
  explicit RomClient(ScsiTransport &t) : t_(t) {}
  // After a failed command: false when it never reached the device.
  bool reached_device() const { return t_.reached_device(); }

  bool inquiry(std::string &vendor, std::string &product, std::string &revision);
  // UBOOT1.00: write RAM (with the data's CRC-16) and jump there with an argument.
  bool mem_write(uint32_t addr, const uint8_t *data, uint16_t len);
  bool mem_jump(uint32_t addr, uint16_t arg);
  // Loader commands.
  bool usb_buffer_size(uint32_t &out);
  bool chip_key(uint16_t &out);
  bool online_device(uint8_t &type, uint32_t &id);
  bool flash_read(uint32_t addr, uint8_t *out, uint16_t len);
  bool erase_sector(uint32_t addr);
  bool flash_write(uint32_t addr, const uint8_t *data, uint16_t len);
  bool run_app(uint32_t arg);

 private:
  // cmd_exec: 16-byte data-in response whose first two bytes echo the command.
  bool exec(uint16_t cmd, const uint8_t *args, size_t args_len, uint8_t resp[kResponseLen]);
  ScsiTransport &t_;
};

// jl-uboot-tool's rule for scrambling the loader on the host before upload:
// the wl82 ROM unscrambles MengLi itself (quirk memory-rw-mengli-crypt), and its
// loader is MengLi-encrypted, so the host sends it as is.
bool loader_needs_host_scramble(bool rom_mengli_quirk, bool loader_is_mengli);

}  // namespace fm1_loader
