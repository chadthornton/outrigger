// The resource broker: which USB interfaces the host claims within the
// ESP32-S3's 8 host channels, which app may act on the FM-1, and the reason the
// screen gives when it refuses one. See docs/architecture.md.
//
// It follows EspUsbHost's order of work: a device enumerates (its EP0 already
// costs a channel), then the host claims its interfaces one by one. attach()
// plans the whole device at that moment; the host's claim filter then asks
// should_claim() per interface. A claim can't be taken back without
// re-enumerating, so changing mode means restarting the USB host.
//
// Plain C++ so tests/ can run it on the Mac.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace platform {

constexpr uint8_t kHostChannels = 8;      // soc/usb_dwc_cfg.h, ESP32-S3
constexpr size_t kMaxInterfaces = 8;      // per device
constexpr size_t kMaxDevices = 8;         // hubs included
constexpr size_t kReasonLen = 34;         // midi_core::kProblemLen: one screen line

// What the device is doing. Each mode belongs to one app, and only that app may
// act on the FM-1.
enum class Mode : uint8_t {
  Play,    // Router: controllers -> FM-1 MIDI. FM-1 audio unclaimed.
  Record,  // Recorder: FM-1 audio in. FM-1 MIDI unclaimed; controllers stay up.
  Load,    // Loader: FM-1 MIDI (to send the ROM-boot key), then the ROM-boot device.
           // Controllers unclaimed, so routing is off (loader safety rule 7).
};

enum class App : uint8_t { None, Router, Recorder, Loader };

// The app that owns a mode.
App mode_app(Mode mode);

enum class Kind : uint8_t {
  Hub,
  Fm1,         // an FM-1 running its firmware (stock or Felucca/Skiff)
  Fm1RomBoot,  // an FM-1 in its ROM bootloader, 4C4A:8057
  Controller,  // any other device with a MIDI interface
  Storage,     // a USB stick (mass storage that isn't an FM-1)
  Other,
};

// The FM-1's three faces, each owned by at most one app at a time.
enum class Fm1Part : uint8_t { Midi, Audio, RomBoot };

struct InterfaceDesc {
  uint8_t number;
  uint8_t cls, subclass, protocol;
  // Non-control endpoints opened when it is claimed (for audio streaming, the
  // alternate setting the app would start). Each one costs a host channel.
  uint8_t endpoints;
};

struct DeviceDesc {
  uint8_t address;
  uint16_t vid, pid;
  bool is_hub;
  char tag[4];  // short name for the screen (midi_core::make_tag)
  InterfaceDesc interfaces[kMaxInterfaces];
  uint8_t interface_count;
};

Kind classify(const DeviceDesc &device);

enum class Refusal : uint8_t {
  None,
  Unused,      // no app uses it (HID, serial, a second MIDI port). Not shown.
  NoChannels,  // all 8 channels are in use
  KeptForFm1,  // the rest are held for an FM-1 that isn't plugged in yet
  NotInMode,   // the active mode doesn't use it (FM-1 audio in Play, ...)
  Loading,     // the Loader is running: controllers are ignored
  OneFm1,      // another FM-1 (running or in ROM boot) is already attached
};

struct Claim {
  bool claim;
  App app;          // who uses it when claimed
  Refusal refusal;  // why not, when not claimed
};

struct Reason {
  char line1[kReasonLen];
  char line2[kReasonLen];
};

// Writes what the screen says about a refusal, two lines of at most
// kReasonLen - 1 characters. Both lines are empty for None and Unused.
// Returns false if the text had to be cut short.
bool make_reason(Refusal refusal, Kind kind, Mode mode, const char *tag, Reason &out);

struct DevicePlan {
  uint8_t address;
  Kind kind;
  Claim interfaces[kMaxInterfaces];  // same order as DeviceDesc::interfaces
  uint8_t channels;                  // what this device costs: EP0 plus claimed endpoints
  Refusal refusal;                   // the refusal worth showing, or None
  Reason reason;                     // what the screen says when refusal != None
};

class Broker {
 public:
  explicit Broker(Mode mode = Mode::Play);

  Mode mode() const { return mode_; }
  // The mode the host should restart in. Differs from mode() when an FM-1 in
  // ROM boot appears outside Load mode (the loader's resume entry).
  Mode wanted_mode() const { return wanted_; }
  // Switches mode and forgets every device: the caller restarts the USB host so
  // they all enumerate again under the new mode's rules.
  void set_mode(Mode mode);

  // Plans a device that has just enumerated. Its EP0 costs a channel whatever
  // is decided; a device whose EP0 doesn't fit is refused whole and not counted.
  // The returned reference is valid until the next attach(), detach() or
  // set_mode(); copy the plan to keep it.
  const DevicePlan &attach(const DeviceDesc &device);
  void detach(uint8_t address);

  const DevicePlan *plan(uint8_t address) const;
  // For EspUsbHost's claim filter. False for unknown devices and interfaces.
  bool should_claim(uint8_t address, uint8_t interface_number) const;

  uint8_t channels_used() const;
  // Which app owns that part of the FM-1 right now. App::None when the mode
  // doesn't use it or no FM-1 in that state is attached and claimed.
  App owner(Fm1Part part) const;

 private:
  struct Slot {
    bool used;
    DevicePlan plan;
    uint8_t numbers[kMaxInterfaces];  // bInterfaceNumber, same order as plan.interfaces
    uint8_t uses[kMaxInterfaces];     // what each one is for (broker.cpp's Use)
    uint8_t interface_count;
  };
  Mode mode_;
  Mode wanted_;
  Slot slots_[kMaxDevices] = {};
  DevicePlan refused_ = {};  // returned for a device that didn't fit at all
};

}  // namespace platform
