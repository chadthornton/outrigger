// USB MIDI host on the Waveshare ESP32-S3-Touch-LCD-1.69.
//
// The board's USB-C port runs as a USB host (EspUsbHost), normally through a
// powered hub. Channel messages from any controller (e.g. a Launchkey) are
// forwarded to the FM-1. midi_core::Router decides what is forwarded; it never
// passes SysEx or any other system message, so a controller cannot put the
// FM-1 into its ROM bootloader or a firmware installer.
//
// This file gathers what the host knows into a UiFrame; host_ui (LVGL) draws
// it and runs the settings page. Board specifics live in board_*.cpp.
//
// Because the USB port is the host, the Mac loses this board's serial port
// once the host starts. Settings > Computer restarts with the host off, which
// leaves the port as the ESP32-S3's built-in USB-Serial-JTAG device, so a
// computer can flash it without BOOT+RST (see boot_mode.h).

#include <Arduino.h>
#include <EspUsbHost.h>
#include <Preferences.h>
#include <esp_app_desc.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/system_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include "board_profile.h"
#include "boot_mode.h"
#include "host_ui.h"
#include "midi_core.h"
#include "status.h"

extern const BoardProfile kBoard;

EspUsbHost usb;
static Preferences prefs;
#ifndef FW_VERSION
#define FW_VERSION 0  // set by the Makefile to the commit
#endif
static bool host_running = false;

// ---------- State shared between the USB task and loop() ----------
// Every EspUsbHost callback runs on the USB client task; loop() copies what it
// needs under shared_lock.
static portMUX_TYPE shared_lock = portMUX_INITIALIZER_UNLOCKED;

struct LastMessage {
  char text[midi_core::kLineLen];
  char mark;      // '>' sent to the FM-1, '!' send failed, or a Router drop letter
  uint32_t at_ms;
};
static LastMessage last_by_address[128];   // per source device
static LastMessage fm1_own;                // latest message the FM-1 itself sent
static LastMessage fm1_in;                 // latest message delivered to the FM-1
static char fm1_in_tag[4] = "";
static uint32_t sent_count = 0, blocked_count = 0;
static uint32_t changes = 0;               // bumped on anything worth redrawing

static midi_core::Router router;           // USB task only
constexpr int kMaxHubs = 2;
static volatile uint8_t hub_addresses[kMaxHubs] = {};

static void on_connected(const EspUsbHostDeviceInfo &d) {
  router.device_connected(d.address, d.vid, d.pid);
  if (d.isHub) {
    for (int i = 0; i < kMaxHubs; ++i)
      if (hub_addresses[i] == 0) { hub_addresses[i] = d.address; break; }
  }
  taskENTER_CRITICAL(&shared_lock);
  ++changes;
  taskEXIT_CRITICAL(&shared_lock);
}

static void on_disconnected(const EspUsbHostDeviceInfo &d) {
  router.device_disconnected(d.address);
  for (int i = 0; i < kMaxHubs; ++i)
    if (hub_addresses[i] == d.address) hub_addresses[i] = 0;
  taskENTER_CRITICAL(&shared_lock);
  if (d.address < 128) last_by_address[d.address] = {};
  ++changes;
  taskEXIT_CRITICAL(&shared_lock);
}

static void on_midi(const EspUsbHostMidiMessage &m) {
  // Code index 0 and 1 are reserved: padding and cable events, not MIDI.
  if (m.codeIndex <= 1) return;

  const bool from_target = router.is_target(m.address);
  char mark = ' ';
  if (!from_target) {
    uint8_t packet[4];
    char why = 'x';
    const uint8_t dest =
        router.route(m.address, m.cable, m.codeIndex, m.status, m.data1, m.data2, packet, &why);
    // midiSend() only submits the transfer, so it is safe on the USB task.
    mark = dest == 0 ? why : usb.midiSend(packet, sizeof packet, dest) ? '>' : '!';
  }
  LastMessage msg{};
  midi_core::format_message(m.status, m.data1, m.data2, m.raw, m.length, msg.text);
  msg.mark = mark;
  msg.at_ms = millis();

  char tag[4] = "";
  EspUsbHostDeviceInfo info;
  if (mark == '>' && usb.getDevice(m.address, info)) midi_core::make_tag(info.product, tag);

  taskENTER_CRITICAL(&shared_lock);
  if (from_target) {
    fm1_own = msg;
  } else {
    if (m.address < 128) last_by_address[m.address] = msg;
    if (mark == '>') {
      ++sent_count;
      fm1_in = msg;
      memcpy(fm1_in_tag, tag, sizeof fm1_in_tag);
    } else if (mark == 's') {
      ++blocked_count;
    }
  }
  ++changes;
  taskEXIT_CRITICAL(&shared_lock);
}

// ---------- Gathering facts ----------
struct HubPorts {
  uint8_t address = 0;
  uint8_t count = 0;
  char state[4] = {'.', '.', '.', '.'};
  uint32_t stuck_since[4] = {};  // millis() when a port turned 'C', 0 if not stuck
};
static HubPorts hub;

// Hub port status costs a control transfer per port, so poll it slowly.
static void poll_hub_ports() {
  uint8_t addr = 0;
  for (int i = 0; i < kMaxHubs && !addr; ++i) addr = hub_addresses[i];
  if (addr != hub.address) hub = HubPorts{};
  hub.address = addr;
  if (!addr) return;
  if (hub.count == 0) {
    EspUsbHostHubInfo info;
    hub.count = usb.getHubInfo(addr, info) ? min<uint8_t>(info.portCount, 4) : 4;
  }
  for (uint8_t p = 0; p < hub.count; ++p) {
    uint16_t status = 0, change = 0;
    if (!usb.getHubPortStatus(addr, p + 1, status, change)) continue;
    const char s = (status & 0x0002) ? 'E' : (status & 0x0001) ? 'C' : (status & 0x0100) ? 'p' : '.';
    if (s == 'C' && hub.state[p] != 'C') hub.stuck_since[p] = millis();
    if (s != 'C') hub.stuck_since[p] = 0;
    hub.state[p] = s;
  }
}

static midi_core::HostSnapshot take_snapshot() {
  midi_core::HostSnapshot s{};
  s.uptime_ms = millis();
  s.hub_present = hub.address != 0;
  s.hub_ports = hub.count;
  for (int p = 0; p < 4; ++p) {
    s.port_state[p] = hub.state[p];
    s.port_stuck_ms[p] = hub.stuck_since[p] ? s.uptime_ms - hub.stuck_since[p] : 0;
  }
  s.channels_used = static_cast<uint8_t>(usb.estimatedHcdChannelCount());

  EspUsbHostDeviceInfo devices[8];
  const size_t n = usb.getDevices(devices, 8);
  for (size_t i = 0; i < n && s.device_count < midi_core::kMaxDevices; ++i) {
    const EspUsbHostDeviceInfo &d = devices[i];
    if (d.isHub) continue;
    midi_core::DeviceView &v = s.devices[s.device_count++];
    v.address = d.address;
    strlcpy(v.name, d.product[0] ? d.product : "(no name)", sizeof v.name);
    v.port_id = d.portId;
    v.role = midi_core::device_role(d.vid, d.pid);
    EspUsbHostInterfaceInfo itfs[8];
    const size_t ni = usb.getInterfaces(d.address, itfs, 8);
    for (size_t k = 0; k < ni; ++k) {
      if (itfs[k].interfaceClass == 1 && itfs[k].interfaceSubClass == 3) {  // Audio / MIDI streaming
        v.is_midi = true;
        v.midi_open = v.midi_open || itfs[k].claimed;
      }
    }
  }
  return s;
}

// What each hub port holds: a device tag, "-" empty, "off" stuck since boot
// (this hub's own inner hub chip), "!" stuck since later.
static void hub_detail(const midi_core::HostSnapshot &s, const midi_core::ChainView &v, char *out, size_t len) {
  if (!s.hub_present) {
    strlcpy(out, s.device_count ? "none (direct)" : "nothing answers", len);
    return;
  }
  size_t pos = strlcpy(out, "ports", len);
  for (int p = 0; p < s.hub_ports && pos + 8 < len; ++p) {
    char tag[5] = "-";
    bool found = false;
    for (int i = 0; i < s.device_count && !found; ++i) {
      if (s.devices[i].port_id == (0x10 | (p + 1))) {
        midi_core::make_tag(s.devices[i].name, tag);
        found = true;
      }
    }
    if (!found) strlcpy(tag, v.port_off_since_boot[p] ? "off" : s.port_state[p] == 'C' ? "!" : "-", sizeof tag);
    pos += snprintf(out + pos, len - pos, " %d:%s", p + 1, tag);
  }
}

static void build_frame(UiFrame &f) {
  f = UiFrame{};
  f.snapshot = take_snapshot();
  f.view = midi_core::evaluate(f.snapshot);
  hub_detail(f.snapshot, f.view, f.hub_detail, sizeof f.hub_detail);

  LastMessage last[midi_core::kMaxDevices], own, in;
  char in_tag[4];
  taskENTER_CRITICAL(&shared_lock);
  for (int i = 0; i < f.snapshot.device_count; ++i) last[i] = last_by_address[f.snapshot.devices[i].address & 0x7F];
  own = fm1_own;
  in = fm1_in;
  memcpy(in_tag, fm1_in_tag, sizeof in_tag);
  f.sent = sent_count;
  f.blocked = blocked_count;
  taskEXIT_CRITICAL(&shared_lock);

  const uint32_t now = millis();
  auto fresh = [now](const LastMessage &m) { return m.at_ms && now - m.at_ms < 400; };
  bool have_target = false;
  for (int i = 0; i < f.snapshot.device_count; ++i) {
    const midi_core::DeviceView &d = f.snapshot.devices[i];
    if (!d.is_midi) continue;
    if (d.role == midi_core::Role::Target) {
      have_target = true;
      continue;
    }
    UiDeviceLine &line = f.controllers[f.controller_count++];
    midi_core::make_tag(d.name, line.tag);
    line.open = d.midi_open;
    line.fresh = fresh(last[i]);
    if (!d.midi_open) snprintf(line.detail, sizeof line.detail, "port %X.%X no USB channel", d.port_id >> 4, d.port_id & 0xF);
    else if (last[i].at_ms) snprintf(line.detail, sizeof line.detail, "%c %s", last[i].mark, last[i].text);
    else strlcpy(line.detail, "quiet", sizeof line.detail);
  }
  if (!have_target) strlcpy(f.fm1_in, "not found", sizeof f.fm1_in);
  else if (in.at_ms) snprintf(f.fm1_in, sizeof f.fm1_in, "in: %s %s", in_tag, in.text);
  else strlcpy(f.fm1_in, "in: -", sizeof f.fm1_in);
  f.fm1_in_fresh = fresh(in);
  if (have_target) snprintf(f.fm1_own, sizeof f.fm1_own, "itself sent: %s", own.at_ms ? own.text : "-");
}

static void save_settings(const UiSettings &s) {
  prefs.putUChar("text_size", s.text_size);
  prefs.putUChar("typeface", s.typeface);
}

// ---------- Boot mode ----------
// Namespace "boot", key "computer": the build ID that asked to start in
// computer mode. Kept in NVS because moving the cable usually cuts power.
static const char *this_build() {
  static char id[17] = "";
  if (!id[0]) esp_app_get_elf_sha256(id, sizeof id);
  return id;
}

static void connect_computer() {
  Preferences boot;
  boot.begin("boot", false);
  boot.putString("computer", this_build());
  boot.end();
  esp_restart();
}

static void back_to_host() {
  Preferences boot;
  boot.begin("boot", false);
  boot.remove("computer");
  boot.end();
  esp_restart();
}

static midi_core::BootMode read_boot_mode() {
  Preferences boot;
  boot.begin("boot", false);
  const String requested = boot.getString("computer", "");
  const midi_core::BootDecision d = midi_core::decide_boot_mode(requested.c_str(), this_build());
  if (d.clear_request) boot.remove("computer");
  boot.end();
  return d.mode;
}

// Gives the USB pins to the chip's USB-Serial-JTAG device, the same steps as
// the Arduino core's usb_switch_to_cdc_jtag(). Plain boot alone doesn't do it
// on this board: with the host never started, the Mac still saw no device.
// Writes the registers before and after into `out` for the screen.
static void hand_usb_to_serial_jtag(char *out, size_t len) {
  const uint32_t clk0 = REG_READ(SYSTEM_PERIP_CLK_EN1_REG), rst0 = REG_READ(SYSTEM_PERIP_RST_EN1_REG),
                 rtc0 = REG_READ(RTC_CNTL_USB_CONF_REG), usj0 = REG_READ(USB_SERIAL_JTAG_CONF0_REG);
  SET_PERI_REG_MASK(SYSTEM_PERIP_CLK_EN1_REG, SYSTEM_USB_DEVICE_CLK_EN);
  CLEAR_PERI_REG_MASK(SYSTEM_PERIP_RST_EN1_REG, SYSTEM_USB_DEVICE_RST);
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL | USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
  // Drop off the bus briefly so the computer enumerates afresh.
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  delay(100);
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  snprintf(out, len, "clk %lx>%lx rst %lx>%lx\nrtc %lx>%lx usj %lx>%lx", (unsigned long)(clk0 >> 8 & 0xF),
           (unsigned long)(REG_READ(SYSTEM_PERIP_CLK_EN1_REG) >> 8 & 0xF), (unsigned long)(rst0 >> 8 & 0xF),
           (unsigned long)(REG_READ(SYSTEM_PERIP_RST_EN1_REG) >> 8 & 0xF), (unsigned long)rtc0,
           (unsigned long)REG_READ(RTC_CNTL_USB_CONF_REG), (unsigned long)usj0,
           (unsigned long)REG_READ(USB_SERIAL_JTAG_CONF0_REG));
}

void setup() {
  const midi_core::BootMode mode = read_boot_mode();
  prefs.begin("ui", false);
  const UiSettings settings = {prefs.getUChar("text_size", 1), prefs.getUChar("typeface", 0)};
  static char firmware[12];
  snprintf(firmware, sizeof firmware, "v %07lx", (unsigned long)FW_VERSION);
  ui_begin(kBoard, settings, {save_settings, connect_computer, back_to_host}, firmware);

  if (mode == midi_core::BootMode::Computer) {
    // The USB host is never installed, so the port stays the chip's
    // USB-Serial-JTAG device and the computer can reset it into download mode.
    char regs[64], info[96];
    hand_usb_to_serial_jtag(regs, sizeof regs);
    snprintf(info, sizeof info, "Firmware %07lx\n%s", (unsigned long)FW_VERSION, regs);
    ui_show_computer_mode(info);
    return;
  }
  usb.onDeviceConnected(on_connected);
  usb.onDeviceDisconnected(on_disconnected);
  usb.onMidiMessage(on_midi);
  usb.begin();
  host_running = true;
}

void loop() {
  static uint32_t last_poll = 0, last_frame = 0, framed_changes = UINT32_MAX;
  static UiFrame frame;
  if (!host_running) {  // computer mode: only the UI runs
    ui_tick();
    delay(5);
    return;
  }
  const uint32_t now = millis();
  if (now - last_poll > 1000) {
    last_poll = now;
    poll_hub_ports();
  }
  uint32_t c;
  taskENTER_CRITICAL(&shared_lock);
  c = changes;
  taskEXIT_CRITICAL(&shared_lock);
  // Refresh on any change (at most ~20 fps) and twice a second regardless, so
  // "fresh" highlights fade and time-based problems appear.
  if ((c != framed_changes && now - last_frame > 50) || now - last_frame > 500) {
    framed_changes = c;
    last_frame = now;
    build_frame(frame);
    ui_update(frame);
  }
  ui_tick();
  delay(5);
}
