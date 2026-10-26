// Host-side tests for lib/platform broker.h: which USB interfaces are claimed
// within 8 host channels in each mode, who owns the FM-1, and what the screen
// says when something is refused. Run with `make test`.
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "broker.h"

using namespace platform;

static int failures = 0;

#define EXPECT(cond, what)                                   \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s (line %d)\n", what, __LINE__);         \
      ++failures;                                            \
    }                                                        \
  } while (0)

// ---------- Devices as they enumerate ----------
// Interface classes: 1 audio (subclass 1 control, 2 streaming, 3 MIDI),
// 2 CDC control, 3 HID, 8 mass storage (6 SCSI, 0x50 bulk-only), 9 hub, 10 CDC data.

static DeviceDesc device(uint8_t address, uint16_t vid, uint16_t pid, const char *tag) {
  DeviceDesc d{};
  d.address = address;
  d.vid = vid;
  d.pid = pid;
  strncpy(d.tag, tag, sizeof d.tag - 1);
  return d;
}

static void add(DeviceDesc &d, uint8_t cls, uint8_t subclass, uint8_t protocol, uint8_t endpoints) {
  InterfaceDesc &i = d.interfaces[d.interface_count];
  i.number = d.interface_count;
  i.cls = cls;
  i.subclass = subclass;
  i.protocol = protocol;
  i.endpoints = endpoints;
  ++d.interface_count;
}

static DeviceDesc hub(uint8_t address) {
  DeviceDesc d = device(address, 0x05E3, 0x0610, "USB");
  d.is_hub = true;
  add(d, 9, 0, 0, 1);  // interrupt IN: port changes
  return d;
}

// Skiff on Chad's unit: audio control, MIDI, audio streaming (iso IN on alt 1),
// then the CDC serial pair.
static DeviceDesc skiff(uint8_t address) {
  DeviceDesc d = device(address, 0x1209, 0x0001, "FEL");
  add(d, 1, 1, 0, 0);
  add(d, 1, 3, 0, 2);
  add(d, 1, 2, 0, 1);
  add(d, 2, 2, 1, 1);
  add(d, 10, 0, 0, 2);
  return d;
}

static DeviceDesc stock_fm1(uint8_t address) {
  DeviceDesc d = device(address, 0x4C4A, 0xC755, "FM-");
  add(d, 1, 1, 0, 0);
  add(d, 1, 3, 0, 2);
  add(d, 1, 2, 0, 1);
  return d;
}

static DeviceDesc rom_boot(uint8_t address) {
  DeviceDesc d = device(address, 0x4C4A, 0x8057, "UBO");
  add(d, 8, 6, 0x50, 2);
  return d;
}

static DeviceDesc controller(uint8_t address, const char *tag) {
  DeviceDesc d = device(address, 0x1235, 0x0141, tag);
  add(d, 1, 1, 0, 0);
  add(d, 1, 3, 0, 2);
  return d;
}

static DeviceDesc stick(uint8_t address) {
  DeviceDesc d = device(address, 0x0781, 0x5567, "SAN");
  add(d, 8, 6, 0x50, 2);
  return d;
}

static DeviceDesc keyboard(uint8_t address) {
  DeviceDesc d = device(address, 0x05AC, 0x024F, "KEY");
  add(d, 3, 1, 1, 1);
  return d;
}

static bool claimed(const DevicePlan &p, int interface) { return p.interfaces[interface].claim; }
static Refusal refusal(const DevicePlan &p, int interface) { return p.interfaces[interface].refusal; }

// ---------- Tests ----------

static void test_classify() {
  EXPECT(classify(hub(1)) == Kind::Hub, "hub");
  EXPECT(classify(skiff(2)) == Kind::Fm1, "Skiff is an FM-1");
  EXPECT(classify(stock_fm1(2)) == Kind::Fm1, "stock firmware is an FM-1");
  EXPECT(classify(rom_boot(2)) == Kind::Fm1RomBoot, "4C4A:8057 is an FM-1 in ROM boot");
  EXPECT(classify(controller(3, "LAU")) == Kind::Controller, "MIDI device is a controller");
  EXPECT(classify(stick(4)) == Kind::Storage, "mass storage is a stick");
  EXPECT(classify(keyboard(5)) == Kind::Other, "HID keyboard is other");
}

static void test_hub_costs_two() {
  Broker b;
  const DevicePlan &p = b.attach(hub(1));
  EXPECT(p.channels == 2 && b.channels_used() == 2, "hub: EP0 + interrupt IN");
  EXPECT(claimed(p, 0) && p.refusal == Refusal::None, "hub interface is claimed");
}

// The rig on the bench: hub + FM-1 (Skiff) + Launchkey is exactly 8 once the
// FM-1's audio and serial port stay unclaimed.
static void test_play_rig_fits_in_eight() {
  Broker b(Mode::Play);
  b.attach(hub(1));
  const DevicePlan fm1 = b.attach(skiff(2));
  EXPECT(fm1.channels == 3, "FM-1 in Play: EP0 + MIDI in + MIDI out");
  EXPECT(claimed(fm1, 0) && claimed(fm1, 1), "audio control and MIDI claimed");
  EXPECT(fm1.interfaces[1].app == App::Router, "Router uses the FM-1's MIDI");
  EXPECT(!claimed(fm1, 2) && refusal(fm1, 2) == Refusal::NotInMode, "audio is for Record mode");
  EXPECT(!claimed(fm1, 3) && !claimed(fm1, 4) && refusal(fm1, 4) == Refusal::Unused, "serial port unused");
  EXPECT(fm1.refusal == Refusal::None, "nothing to show: the FM-1 is doing its job");

  const DevicePlan lk = b.attach(controller(3, "LAU"));
  EXPECT(claimed(lk, 1) && lk.interfaces[1].app == App::Router, "controller MIDI to the Router");
  EXPECT(lk.channels == 3, "controller: EP0 + MIDI in + MIDI out");
  EXPECT(b.channels_used() == 8, "8 of 8");
  EXPECT(b.owner(Fm1Part::Midi) == App::Router, "Router owns FM-1 MIDI");
  EXPECT(b.owner(Fm1Part::Audio) == App::None, "nobody owns FM-1 audio in Play");
}

static void test_arrival_order_doesnt_matter_for_the_rig() {
  Broker b(Mode::Play);
  b.attach(hub(1));
  b.attach(controller(2, "LAU"));
  const DevicePlan &fm1 = b.attach(skiff(3));
  EXPECT(claimed(fm1, 1), "FM-1 MIDI fits after the Launchkey");
  EXPECT(b.channels_used() == 8, "still 8 of 8");
}

static void test_should_claim_answers_the_filter() {
  Broker b;
  b.attach(hub(1));
  b.attach(skiff(2));
  EXPECT(b.should_claim(2, 1), "FM-1 MIDI interface");
  EXPECT(!b.should_claim(2, 2), "FM-1 audio streaming in Play");
  EXPECT(!b.should_claim(2, 7), "interface the device doesn't have");
  EXPECT(!b.should_claim(9, 0), "device the broker never saw");
}

static void test_device_whose_ep0_doesnt_fit() {
  Broker b;
  b.attach(hub(1));
  b.attach(skiff(2));
  b.attach(controller(3, "LAU"));
  const DevicePlan &p = b.attach(controller(4, "MPK"));
  EXPECT(p.refusal == Refusal::NoChannels && !claimed(p, 1), "fourth device refused");
  EXPECT(p.channels == 0 && b.channels_used() == 8, "and not counted");
  EXPECT(strcmp(p.reason.line1, "No USB channel for MPK") == 0, "screen names it");
  EXPECT(b.plan(4) == nullptr, "not remembered");
}

// A second controller costs its EP0 whatever is decided, so once it's plugged in
// the FM-1 can no longer fit; refusing its MIDI then gains nothing.
static void test_second_controller_without_fm1_still_plays() {
  Broker b(Mode::Play);
  b.attach(hub(1));
  b.attach(controller(2, "LAU"));
  const DevicePlan mpk = b.attach(controller(3, "MPK"));
  EXPECT(claimed(mpk, 1), "the FM-1's share is out of reach, so MPK gets MIDI");
  EXPECT(b.channels_used() == 8, "8 of 8");
  const DevicePlan &fm1 = b.attach(skiff(4));
  EXPECT(fm1.refusal == Refusal::NoChannels, "FM-1 can't enumerate now");
  EXPECT(b.owner(Fm1Part::Midi) == App::None, "and nobody owns it");
}

// Where holding the FM-1's share does help: a USB stick that arrives in Record
// mode before the FM-1.
static void test_stick_waits_for_fm1_in_record() {
  Broker b(Mode::Record);
  b.attach(hub(1));
  b.attach(controller(2, "LAU"));
  const DevicePlan s = b.attach(stick(3));
  EXPECT(!claimed(s, 0) && refusal(s, 0) == Refusal::KeptForFm1, "stick would eat the FM-1's 2");
  EXPECT(s.refusal == Refusal::KeptForFm1, "shown: the stick does nothing");
  EXPECT(strcmp(s.reason.line1, "SAN off: room kept for FM-1") == 0, "screen line 1");
  EXPECT(b.channels_used() == 6, "hub 2 + LAU 3 + stick EP0 1");

  const DevicePlan fm1 = b.attach(skiff(4));
  EXPECT(claimed(fm1, 2) && fm1.interfaces[2].app == App::Recorder, "Recorder takes FM-1 audio");
  EXPECT(!claimed(fm1, 1) && refusal(fm1, 1) == Refusal::NotInMode, "FM-1 MIDI stays off");
  EXPECT(fm1.channels == 2, "FM-1 in Record: EP0 + iso IN");
  EXPECT(b.channels_used() == 8, "8 of 8");
}

static void test_stick_fits_in_record_without_controller() {
  Broker b(Mode::Record);
  b.attach(hub(1));
  const DevicePlan s = b.attach(stick(2));
  EXPECT(claimed(s, 0) && s.interfaces[0].app == App::Recorder, "stick for the Recorder");
  b.attach(skiff(3));
  EXPECT(b.channels_used() == 7, "hub 2 + stick 3 + FM-1 audio 2");
}

static void test_record_mode_rules() {
  Broker b(Mode::Record);
  b.attach(hub(1));
  b.attach(skiff(2));
  const DevicePlan &lk = b.attach(controller(3, "LAU"));
  EXPECT(claimed(lk, 1) && lk.interfaces[1].app == App::Router, "controllers stay up in Record");
  EXPECT(b.owner(Fm1Part::Audio) == App::Recorder, "Recorder owns FM-1 audio");
  EXPECT(b.owner(Fm1Part::Midi) == App::None, "FM-1 MIDI unclaimed while recording");
  EXPECT(b.channels_used() == 7, "hub 2 + FM-1 audio 2 + LAU 3");
}

static void test_stick_unused_in_play() {
  Broker b(Mode::Play);
  b.attach(hub(1));
  const DevicePlan &s = b.attach(stick(2));
  EXPECT(!claimed(s, 0) && s.refusal == Refusal::NotInMode, "stick is for Record mode");
  EXPECT(strcmp(s.reason.line1, "USB stick unused in Play mode") == 0, "screen says so");
}

static void test_load_mode_turns_routing_off() {
  Broker b(Mode::Load);
  b.attach(hub(1));
  const DevicePlan lk = b.attach(controller(2, "LAU"));
  EXPECT(!claimed(lk, 1) && lk.refusal == Refusal::Loading, "controller ignored while loading");
  EXPECT(lk.channels == 1, "it still costs its EP0");
  EXPECT(strcmp(lk.reason.line1, "LAU ignored while loading") == 0, "screen line 1");

  const DevicePlan fm1 = b.attach(skiff(3));
  EXPECT(claimed(fm1, 1) && fm1.interfaces[1].app == App::Loader, "Loader takes FM-1 MIDI for the key");
  EXPECT(!claimed(fm1, 2), "no audio while loading");
  EXPECT(b.owner(Fm1Part::Midi) == App::Loader, "Loader owns FM-1 MIDI");

  // The key sent, the FM-1 comes back as the ROM-boot device.
  b.detach(3);
  EXPECT(b.owner(Fm1Part::Midi) == App::None, "gone with the device");
  const DevicePlan &rom = b.attach(rom_boot(3));
  EXPECT(claimed(rom, 0) && rom.interfaces[0].app == App::Loader, "Loader claims the ROM-boot device");
  EXPECT(b.owner(Fm1Part::RomBoot) == App::Loader, "Loader owns ROM boot");
  EXPECT(b.channels_used() == 2 + 1 + 3, "hub + LAU EP0 + ROM boot");
}

// The loader's resume entry: an FM-1 already in ROM boot when the host starts.
static void test_rom_boot_outside_load_asks_for_load_mode() {
  Broker b(Mode::Play);
  b.attach(hub(1));
  b.attach(controller(2, "LAU"));
  const DevicePlan rom = b.attach(rom_boot(3));
  EXPECT(!claimed(rom, 0), "not claimed in Play");
  EXPECT(b.wanted_mode() == Mode::Load, "host must restart in Load mode");
  EXPECT(b.mode() == Mode::Play, "until it does");
  EXPECT(strcmp(rom.reason.line1, "FM-1 is in ROM boot") == 0, "screen line 1");
  EXPECT(strcmp(rom.reason.line2, "Restarting in Load mode") == 0, "screen line 2");
  EXPECT(b.owner(Fm1Part::RomBoot) == App::None, "nobody acts on it yet");

  b.set_mode(Mode::Load);
  EXPECT(b.wanted_mode() == Mode::Load && b.mode() == Mode::Load, "restarted");
  EXPECT(b.channels_used() == 0 && b.plan(2) == nullptr, "every device enumerates again");
}

// The ROM-boot device left before the host restarted: no reason to go to Load.
static void test_rom_boot_gone_cancels_load() {
  Broker b(Mode::Play);
  b.attach(rom_boot(3));
  EXPECT(b.wanted_mode() == Mode::Load, "asks for Load");
  b.detach(3);
  EXPECT(b.wanted_mode() == Mode::Play, "and stops asking once it's gone");
}

// The host's hub driver opens a hub's interrupt pipe whatever the broker says,
// so the broker must count it, never refuse it.
static void test_hub_is_always_counted() {
  Broker b(Mode::Play);
  b.attach(hub(1));     // 2
  b.attach(stick(2));   // EP0 only in Play: 3
  b.attach(keyboard(3));  // EP0 only: 4
  // EP0 makes 5; the FM-1's 3 would still fit, and the hub's interrupt IN
  // crosses into them. It is opened anyway, so it must be counted.
  const DevicePlan &h = b.attach(hub(4));
  EXPECT(claimed(h, 0) && h.channels == 2, "second hub: EP0 + interrupt IN");
  EXPECT(b.channels_used() == 6, "counted");
}

// An audio control interface with an interrupt endpoint costs a channel too,
// and may not take the FM-1's share any more than MIDI may.
static void test_audio_control_respects_fm1_share() {
  Broker b(Mode::Play);
  b.attach(hub(1));
  DeviceDesc x = device(2, 0x1235, 0x0200, "AUD");
  add(x, 1, 1, 0, 1);  // audio control with an interrupt endpoint
  add(x, 1, 3, 0, 2);
  const DevicePlan p = b.attach(x);
  EXPECT(claimed(p, 1), "MIDI fits beside the FM-1's share");
  EXPECT(!claimed(p, 0) && refusal(p, 0) == Refusal::KeptForFm1, "AC's endpoint would eat into it");
  const DevicePlan &fm1 = b.attach(skiff(3));
  EXPECT(claimed(fm1, 1) && b.channels_used() == 8, "FM-1 still fits");
}

static void test_one_fm1_at_a_time() {
  Broker b(Mode::Play);
  b.attach(hub(1));
  b.attach(skiff(2));
  const DevicePlan &second = b.attach(stock_fm1(3));
  EXPECT(!claimed(second, 1) && second.refusal == Refusal::OneFm1, "second FM-1 refused");
  EXPECT(strcmp(second.reason.line1, "One FM-1 at a time") == 0, "screen line 1");
  // Unplug both, plug the stock one back in.
  b.detach(2);
  b.detach(3);
  const DevicePlan &again = b.attach(stock_fm1(4));
  EXPECT(claimed(again, 1), "after the first leaves, another FM-1 works");

  // A refused FM-1 isn't in charge: once the first leaves, a new one may claim.
  Broker c(Mode::Play);
  c.attach(skiff(2));
  c.attach(stock_fm1(3));
  c.detach(2);
  EXPECT(claimed(c.attach(skiff(4)), 1), "the refused one doesn't block the next");

  Broker r(Mode::Record);
  r.attach(skiff(2));
  EXPECT(r.attach(stock_fm1(3)).refusal == Refusal::OneFm1, "Record mode says the same");
}

// Only one app may act on the FM-1 at a time, whatever the mode.
static void test_one_owner_per_mode() {
  const Mode modes[] = {Mode::Play, Mode::Record, Mode::Load};
  for (Mode m : modes) {
    Broker b(m);
    b.attach(hub(1));
    b.attach(skiff(2));
    App seen = App::None;
    int owners = 0;
    for (Fm1Part part : {Fm1Part::Midi, Fm1Part::Audio, Fm1Part::RomBoot}) {
      const App a = b.owner(part);
      if (a == App::None) continue;
      if (a != seen) ++owners;
      seen = a;
    }
    EXPECT(owners == 1, "exactly one app on the FM-1");
    EXPECT(seen == mode_app(m), "and it's the mode's app");
  }
}

static void test_detach_frees_channels() {
  Broker b;
  b.attach(hub(1));
  b.attach(skiff(2));
  b.attach(controller(3, "LAU"));
  b.detach(3);
  EXPECT(b.channels_used() == 5, "LAU's 3 freed");
  b.detach(42);
  EXPECT(b.channels_used() == 5, "unknown address ignored");
}

static void test_unused_interfaces_stay_quiet() {
  Broker b;
  const DevicePlan &k = b.attach(keyboard(2));
  EXPECT(!claimed(k, 0) && refusal(k, 0) == Refusal::Unused, "no app wants a keyboard");
  EXPECT(k.refusal == Refusal::None, "and the screen doesn't complain");
  EXPECT(k.channels == 1, "EP0 only");

  DeviceDesc two_ports = controller(3, "LAU");
  add(two_ports, 1, 3, 0, 2);  // a second MIDI interface (DAW port)
  const DevicePlan &p = b.attach(two_ports);
  EXPECT(claimed(p, 1) && !claimed(p, 2) && refusal(p, 2) == Refusal::Unused, "first MIDI interface only");
}

static void test_direct_fm1_without_hub() {
  Broker b;
  const DevicePlan &fm1 = b.attach(skiff(1));
  EXPECT(claimed(fm1, 1) && b.channels_used() == 3, "FM-1 alone");
}

// Every reason must fit one screen line (33 characters) with the longest tag.
static void test_every_reason_fits() {
  const Refusal refusals[] = {Refusal::NoChannels, Refusal::KeptForFm1, Refusal::NotInMode,
                              Refusal::Loading, Refusal::OneFm1};
  const Kind kinds[] = {Kind::Hub, Kind::Fm1, Kind::Fm1RomBoot, Kind::Controller, Kind::Storage, Kind::Other};
  const Mode modes[] = {Mode::Play, Mode::Record, Mode::Load};
  for (Refusal r : refusals)
    for (Kind k : kinds)
      for (Mode m : modes) {
        Reason out;
        EXPECT(make_reason(r, k, m, "WWW", out), "reason fits");
        EXPECT(out.line1[0] != '\0', "reason has a first line");
      }
  Reason none;
  EXPECT(make_reason(Refusal::None, Kind::Fm1, Mode::Play, "FEL", none) && none.line1[0] == '\0',
         "no refusal, no text");
}

int main() {
  test_classify();
  test_hub_costs_two();
  test_play_rig_fits_in_eight();
  test_arrival_order_doesnt_matter_for_the_rig();
  test_should_claim_answers_the_filter();
  test_device_whose_ep0_doesnt_fit();
  test_second_controller_without_fm1_still_plays();
  test_stick_waits_for_fm1_in_record();
  test_stick_fits_in_record_without_controller();
  test_record_mode_rules();
  test_stick_unused_in_play();
  test_load_mode_turns_routing_off();
  test_rom_boot_outside_load_asks_for_load_mode();
  test_rom_boot_gone_cancels_load();
  test_hub_is_always_counted();
  test_audio_control_respects_fm1_share();
  test_one_fm1_at_a_time();
  test_one_owner_per_mode();
  test_detach_frees_channels();
  test_unused_interfaces_stay_quiet();
  test_direct_fm1_without_hub();
  test_every_reason_fits();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all broker tests passed\n");
  return 0;
}
