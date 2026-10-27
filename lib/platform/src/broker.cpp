#include "broker.h"

#include <stdio.h>
#include <string.h>

#include "midi_core.h"

namespace platform {

namespace {

// What an interface is for, from its class triple.
enum class Use : uint8_t { Hub, Midi, AudioControl, AudioStream, Storage, Other };

Use use_of(const InterfaceDesc &i) {
  if (i.cls == 9) return Use::Hub;
  if (i.cls == 1 && i.subclass == 1) return Use::AudioControl;
  if (i.cls == 1 && i.subclass == 2) return Use::AudioStream;
  if (i.cls == 1 && i.subclass == 3) return Use::Midi;
  if (i.cls == 8 && i.subclass == 6 && i.protocol == 0x50) return Use::Storage;
  return Use::Other;
}

bool is_fm1(Kind k) { return k == Kind::Fm1 || k == Kind::Fm1RomBoot; }

// Channels an FM-1 not yet plugged in will need in this mode, EP0 included.
uint8_t fm1_share(Mode m) {
  switch (m) {
    case Mode::Play: return 3;    // EP0 + MIDI in + MIDI out
    case Mode::Record: return 2;  // EP0 + iso audio in
    case Mode::Load: return 3;    // EP0 + MIDI (key), later EP0 + bulk in + bulk out
  }
  return 3;
}

const char *mode_name(Mode m) {
  switch (m) {
    case Mode::Play: return "Play";
    case Mode::Record: return "Record";
    case Mode::Load: return "Load";
  }
  return "?";
}

constexpr Claim kUnused = {false, App::None, Refusal::Unused};

Claim use(App a) { return {true, a, Refusal::None}; }
Claim refuse(Refusal r) { return {false, App::None, r}; }

// Which app wants an interface in this mode, before counting channels.
// Audio control is decided afterwards: it goes with the device's other audio
// interfaces.
Claim want(Kind kind, Use u, Mode mode, bool first_midi) {
  switch (kind) {
    case Kind::Hub:
      return u == Use::Hub ? use(App::None) : kUnused;  // the host's own hub driver
    case Kind::Fm1:
      if (u == Use::Midi && first_midi) {
        if (mode == Mode::Play) return use(App::Router);
        if (mode == Mode::Load) return use(App::Loader);
        return refuse(Refusal::NotInMode);
      }
      if (u == Use::AudioStream) return mode == Mode::Record ? use(App::Recorder) : refuse(Refusal::NotInMode);
      return kUnused;
    case Kind::Fm1RomBoot:
      if (u == Use::Storage) return mode == Mode::Load ? use(App::Loader) : refuse(Refusal::NotInMode);
      return kUnused;
    case Kind::Controller:
      if (u == Use::Midi && first_midi) return mode == Mode::Load ? refuse(Refusal::Loading) : use(App::Router);
      return kUnused;
    case Kind::Storage:
      if (u == Use::Storage) return mode == Mode::Record ? use(App::Recorder) : refuse(Refusal::NotInMode);
      return kUnused;
    case Kind::Other:
      return kUnused;
  }
  return kUnused;
}

}  // namespace

App mode_app(Mode mode) {
  switch (mode) {
    case Mode::Play: return App::Router;
    case Mode::Record: return App::Recorder;
    case Mode::Load: return App::Loader;
  }
  return App::None;
}

Kind classify(const DeviceDesc &d) {
  if (d.is_hub) return Kind::Hub;
  if (d.vid == 0x4C4A && d.pid == 0x8057) return Kind::Fm1RomBoot;
  if (midi_core::device_role(d.vid, d.pid) == midi_core::Role::Target) return Kind::Fm1;
  bool midi = false, storage = false;
  for (uint8_t i = 0; i < d.interface_count && i < kMaxInterfaces; ++i) {
    const Use u = use_of(d.interfaces[i]);
    midi = midi || u == Use::Midi;
    storage = storage || u == Use::Storage;
  }
  if (midi) return Kind::Controller;
  if (storage) return Kind::Storage;
  return Kind::Other;
}

bool make_reason(Refusal refusal, Kind kind, Mode mode, const char *tag, Reason &out) {
  out = {};
  const char *l2 = "";
  int n = 0;
  switch (refusal) {
    case Refusal::None:
    case Refusal::Unused:
      return true;
    case Refusal::NoChannels:
      n = snprintf(out.line1, kReasonLen, "No USB channel for %s", tag);
      l2 = "8 of 8 in use: unplug a device";
      break;
    case Refusal::KeptForFm1:
      n = snprintf(out.line1, kReasonLen, "%s off: room kept for FM-1", tag);
      l2 = "Plug in the FM-1 first";
      break;
    case Refusal::Loading:
      n = snprintf(out.line1, kReasonLen, "%s ignored while loading", tag);
      l2 = "Routing is off until it's done";
      break;
    case Refusal::OneFm1:
      n = snprintf(out.line1, kReasonLen, "One FM-1 at a time");
      l2 = "Unplug the other FM-1";
      break;
    case Refusal::NotInMode:
      if (kind == Kind::Fm1RomBoot) {
        n = snprintf(out.line1, kReasonLen, "FM-1 is in ROM boot");
        l2 = "Switch the FM-1 off and on";  // until the resume entry is wired
      } else if (kind == Kind::Fm1 && mode == Mode::Play) {
        n = snprintf(out.line1, kReasonLen, "FM-1 audio off in Play mode");
        l2 = "Record mode turns it on";
      } else if (kind == Kind::Fm1 && mode == Mode::Record) {
        n = snprintf(out.line1, kReasonLen, "FM-1 MIDI off while recording");
        l2 = "Play mode turns it back on";
      } else if (kind == Kind::Fm1) {
        n = snprintf(out.line1, kReasonLen, "FM-1 audio off while loading");
        l2 = "Back on after loading";
      } else if (kind == Kind::Storage) {
        n = snprintf(out.line1, kReasonLen, "USB stick unused in %s mode", mode_name(mode));
        l2 = "Record mode saves to it";
      } else {
        n = snprintf(out.line1, kReasonLen, "%s unused in %s mode", tag, mode_name(mode));
      }
      break;
  }
  const int n2 = snprintf(out.line2, kReasonLen, "%s", l2);
  return n >= 0 && n < static_cast<int>(kReasonLen) && n2 < static_cast<int>(kReasonLen);
}

Broker::Broker(Mode mode) : mode_(mode), wanted_(mode) {}

void Broker::set_mode(Mode mode) {
  mode_ = wanted_ = mode;
  for (Slot &s : slots_) s = Slot{};
}

uint8_t Broker::channels_used() const {
  unsigned n = 0;
  for (const Slot &s : slots_)
    if (s.used) n += s.plan.channels;
  return static_cast<uint8_t>(n);
}

const DevicePlan &Broker::attach(const DeviceDesc &d) {
  detach(d.address);  // a re-enumeration replaces the old plan
  const Kind kind = classify(d);
  const uint8_t count = d.interface_count < kMaxInterfaces ? d.interface_count : kMaxInterfaces;

  Slot *slot = nullptr;
  bool fm1_attached = false;
  for (Slot &s : slots_) {
    if (!s.used && !slot) slot = &s;
    if (s.used && is_fm1(s.plan.kind) && s.plan.refusal != Refusal::OneFm1) fm1_attached = true;
  }

  unsigned used = channels_used();
  // Enumeration opened EP0. If that didn't fit, the device can't work at all.
  if (!slot || used + 1 > kHostChannels) {
    refused_ = DevicePlan{};
    refused_.address = d.address;
    refused_.kind = kind;
    for (uint8_t i = 0; i < count; ++i) refused_.interfaces[i] = refuse(Refusal::NoChannels);
    refused_.refusal = Refusal::NoChannels;
    make_reason(Refusal::NoChannels, kind, mode_, d.tag, refused_.reason);
    return refused_;
  }
  used += 1;

  // Hold back the FM-1's share for an FM-1 that isn't here yet, but only while
  // it can still fit: every device costs its EP0 whatever is decided.
  const unsigned share = fm1_share(mode_);
  const unsigned reserve = !fm1_attached && !is_fm1(kind) && used + share <= kHostChannels ? share : 0;

  DevicePlan p{};
  p.address = d.address;
  p.kind = kind;
  bool seen_midi = false;
  for (uint8_t i = 0; i < count; ++i) {
    const Use u = use_of(d.interfaces[i]);
    if (u == Use::AudioControl) continue;
    Claim c = want(kind, u, mode_, u == Use::Midi && !seen_midi);
    if (u == Use::Midi) seen_midi = true;
    if (is_fm1(kind) && fm1_attached && c.refusal != Refusal::Unused) c = refuse(Refusal::OneFm1);
    if (c.claim) {
      const unsigned cost = d.interfaces[i].endpoints;
      // The host's hub driver opens a hub's pipe whatever is decided: count it.
      if (kind != Kind::Hub && used + cost + reserve > kHostChannels)
        c = refuse(used + cost <= kHostChannels ? Refusal::KeptForFm1 : Refusal::NoChannels);
      else
        used += cost;
    }
    p.interfaces[i] = c;
  }
  // Audio control goes with the device's other audio interfaces: whoever
  // claimed MIDI or audio streaming needs it (it is driven over EP0, but some
  // devices give it an interrupt endpoint).
  App audio_app = App::None;
  for (uint8_t i = 0; i < count; ++i) {
    const Use u = use_of(d.interfaces[i]);
    if ((u == Use::Midi || u == Use::AudioStream) && p.interfaces[i].claim) audio_app = p.interfaces[i].app;
  }
  for (uint8_t i = 0; i < count; ++i) {
    if (use_of(d.interfaces[i]) != Use::AudioControl) continue;
    Claim c = audio_app != App::None ? use(audio_app) : kUnused;
    if (c.claim) {
      const unsigned cost = d.interfaces[i].endpoints;
      if (used + cost + reserve > kHostChannels)
        c = refuse(used + cost <= kHostChannels ? Refusal::KeptForFm1 : Refusal::NoChannels);
      else
        used += cost;
    }
    p.interfaces[i] = c;
  }

  // The screen explains a device only when it ends up doing nothing.
  bool any = false;
  Refusal shown = Refusal::None;
  unsigned channels = 1;
  for (uint8_t i = 0; i < count; ++i) {
    const Claim &c = p.interfaces[i];
    if (c.claim) {
      any = true;
      channels += d.interfaces[i].endpoints;
    } else if (shown == Refusal::None && c.refusal != Refusal::Unused) {
      shown = c.refusal;
    }
  }
  p.channels = static_cast<uint8_t>(channels);
  p.refusal = any ? Refusal::None : shown;
  make_reason(p.refusal, kind, mode_, d.tag, p.reason);

  // The loader's resume entry: an FM-1 already in ROM boot needs Load mode.
  if (kind == Kind::Fm1RomBoot && p.refusal == Refusal::NotInMode) wanted_ = Mode::Load;

  slot->used = true;
  slot->plan = p;
  slot->interface_count = count;
  for (uint8_t i = 0; i < count; ++i) {
    slot->numbers[i] = d.interfaces[i].number;
    slot->uses[i] = static_cast<uint8_t>(use_of(d.interfaces[i]));
  }
  return slot->plan;
}

void Broker::detach(uint8_t address) {
  for (Slot &s : slots_)
    if (s.used && s.plan.address == address) s = Slot{};
  // Only an FM-1 still waiting in ROM boot keeps asking for Load mode.
  wanted_ = mode_;
  for (const Slot &s : slots_)
    if (s.used && s.plan.kind == Kind::Fm1RomBoot && s.plan.refusal == Refusal::NotInMode) wanted_ = Mode::Load;
}

const DevicePlan *Broker::plan(uint8_t address) const {
  for (const Slot &s : slots_)
    if (s.used && s.plan.address == address) return &s.plan;
  return nullptr;
}

bool Broker::should_claim(uint8_t address, uint8_t interface_number) const {
  for (const Slot &s : slots_) {
    if (!s.used || s.plan.address != address) continue;
    for (uint8_t i = 0; i < s.interface_count; ++i)
      if (s.numbers[i] == interface_number) return s.plan.interfaces[i].claim;
  }
  return false;
}

App Broker::owner(Fm1Part part) const {
  const Kind kind = part == Fm1Part::RomBoot ? Kind::Fm1RomBoot : Kind::Fm1;
  const Use wanted = part == Fm1Part::Midi ? Use::Midi : part == Fm1Part::Audio ? Use::AudioStream : Use::Storage;
  for (const Slot &s : slots_) {
    if (!s.used || s.plan.kind != kind) continue;
    for (uint8_t i = 0; i < s.interface_count; ++i)
      if (s.uses[i] == static_cast<uint8_t>(wanted) && s.plan.interfaces[i].claim) return s.plan.interfaces[i].app;
  }
  return App::None;
}

}  // namespace platform
