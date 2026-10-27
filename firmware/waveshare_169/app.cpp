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
// once the host starts. Settings > Computer restarts with the host off and
// hands the port to the ESP32-S3's built-in USB-Serial-JTAG device, so a
// computer can flash it without BOOT+RST (see boot_mode.h).

#include <Arduino.h>
#include <atomic>
#include <EspUsbHost.h>
#include <Preferences.h>
#include <esp_app_desc.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/system_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include "apps.h"
#include "crumbs.h"
#include "diag.h"
#include "board_profile.h"
#include "boot_mode.h"
#include "broker.h"
#include "describe.h"
#include "host_ui.h"
#include "midi_core.h"
#include "status.h"
#if APP_LOADER
#include "loader_app.h"
#endif
#if APP_WEB_UI
#include "web.h"
#endif

extern const BoardProfile kBoard;

EspUsbHost usb;
static Preferences prefs;
#ifndef FW_VERSION
#define FW_VERSION 0  // set by the Makefile to the commit
#endif
#ifndef FW_DIRTY
#define FW_DIRTY 0    // 1 when built with uncommitted changes
#endif
static bool host_running = false;
// LVGL redraws recurse through nested objects; with the USB host up and the
// Firmware page built, 8 KB wasn't enough (checkpoint 2 core dump: loopTask
// stack overflow inside lv_obj_redraw). 16 KB leaves room.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
RTC_NOINIT_ATTR Crumbs crumbs;
// The last abnormal reset, for the Firmware page (empty after a normal boot).
char last_crash[3][40] = {};

static void read_crumbs() {
  const esp_reset_reason_t r = esp_reset_reason();
  const bool abnormal = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                        r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
  if (abnormal && crumbs.magic == kCrumbMagic) {
    snprintf(last_crash[0], sizeof last_crash[0], "Last reset: %s",
             r == ESP_RST_PANIC ? "crash" : r == ESP_RST_BROWNOUT ? "brownout" : "watchdog");
    snprintf(last_crash[1], sizeof last_crash[1], "loop:%s usb:%s", crumbs.loop, crumbs.usb);
    snprintf(last_crash[2], sizeof last_crash[2], "loader:%s", crumbs.loader);
  }
  memset(&crumbs, 0, sizeof crumbs);
  crumbs.magic = kCrumbMagic;
}

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
static uint32_t sent_count = 0, blocked_count = 0, failed_count = 0;
static uint32_t changes = 0;               // bumped on anything worth redrawing

static midi_core::Router router;           // USB task only
// Decides which interfaces EspUsbHost claims within the 8 host channels
// (docs/architecture.md). USB task only; loop() reads its reasons below.
static platform::Broker broker(platform::Mode::Play);
// The broker lives on the USB client task; the loader task reads this copy of
// whether the Loader owns the FM-1's MIDI (set after each attach and detach).
static std::atomic<bool> loader_has_fm1_midi{false};
static platform::Reason refused_why[128];  // per address, under shared_lock
constexpr int kMaxHubs = 2;
static volatile uint8_t hub_addresses[kMaxHubs] = {};

static void on_connected(const EspUsbHostDeviceInfo &d) {
  CRUMB_USB("connected");
  DLOG_USB("connected #%u %04x:%04x %s%s port %02x", d.address, d.vid, d.pid, d.product, d.isHub ? " (hub)" : "",
           d.portId);
  router.device_connected(d.address, d.vid, d.pid);
#if APP_LOADER
  loader_device_connected(d);
#endif
  if (d.isHub) {
    for (int i = 0; i < kMaxHubs; ++i)
      if (hub_addresses[i] == 0) { hub_addresses[i] = d.address; break; }
  }
  taskENTER_CRITICAL(&shared_lock);
  ++changes;
  taskEXIT_CRITICAL(&shared_lock);
}

// EspUsbHost's claim planner: runs before any of the device's interfaces is
// claimed, so the broker sees the whole device first.
static void plan_claims(const EspUsbHostDeviceInfo &d, bool is_hub, const uint8_t *config, uint16_t length) {
  CRUMB_USB("plan_claims");
  char tag[4];
  midi_core::make_tag(d.product, tag);
  platform::DeviceDesc desc;
  // A descriptor that runs past its end still plans the interfaces read so far.
  platform::describe_device(d.address, d.vid, d.pid, is_hub, tag, config, length, desc);
  const platform::DevicePlan &plan = broker.attach(desc);
  if (plan.reason.line1[0]) DLOG_USB("refused #%u %s: %s", d.address, tag, plan.reason.line1);
#if APP_LOADER
  loader_has_fm1_midi = broker.owner(platform::Fm1Part::Midi) == platform::App::Loader;
#endif
  taskENTER_CRITICAL(&shared_lock);
  if (d.address < 128) refused_why[d.address] = plan.reason;
  ++changes;
  taskEXIT_CRITICAL(&shared_lock);
}

static bool claim_filter(uint8_t address, uint8_t interface_number) {
  return broker.should_claim(address, interface_number);
}

static void on_disconnected(const EspUsbHostDeviceInfo &d) {
  CRUMB_USB("disconnected");
  DLOG_USB("disconnected #%u %04x:%04x", d.address, d.vid, d.pid);
  router.device_disconnected(d.address);
#if APP_LOADER
  loader_device_disconnected(d);
#endif
  broker.detach(d.address);
#if APP_LOADER
  loader_has_fm1_midi = broker.owner(platform::Fm1Part::Midi) == platform::App::Loader;
#endif
  for (int i = 0; i < kMaxHubs; ++i)
    if (hub_addresses[i] == d.address) hub_addresses[i] = 0;
  taskENTER_CRITICAL(&shared_lock);
  if (d.address < 128) {
    last_by_address[d.address] = {};
    refused_why[d.address] = {};
  }
  ++changes;
  taskEXIT_CRITICAL(&shared_lock);
}

static void on_midi(const EspUsbHostMidiMessage &m) {
  CRUMB_USB("midi");
  // Code index 0 and 1 are reserved: padding and cable events, not MIDI.
  if (m.codeIndex <= 1) return;

  const bool from_target = router.is_target(m.address);
#if APP_LOADER
  if (from_target) loader_midi(m);  // the identity answer, in Load mode
#endif
  char mark = ' ';
  if (!from_target) {
    uint8_t packet[4];
    char why = 'x';
    const uint8_t dest =
        router.route(m.address, m.cable, m.codeIndex, m.status, m.data1, m.data2, packet, &why);
    // midiSend() only submits the transfer, so it is safe on the USB task.
    mark = dest == 0 ? why : usb.midiSend(packet, sizeof packet, dest) ? '>' : '!';
    if (mark == '!') DLOG_USB("midiSend to #%u failed: %02x %02x %02x", dest, m.status, m.data1, m.data2);
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
    } else if (mark == '!') {
      ++failed_count;
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
    platform::Reason why;
    taskENTER_CRITICAL(&shared_lock);
    why = refused_why[d.address & 0x7F];
    taskEXIT_CRITICAL(&shared_lock);
    strlcpy(v.why1, why.line1, sizeof v.why1);
    strlcpy(v.why2, why.line2, sizeof v.why2);
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
    if (!d.midi_open && d.why1[0]) strlcpy(line.detail, d.why1, sizeof line.detail);
    else if (!d.midi_open) snprintf(line.detail, sizeof line.detail, "port %X.%X no USB channel", d.port_id >> 4, d.port_id & 0xF);
    else if (last[i].at_ms) snprintf(line.detail, sizeof line.detail, "%c %s", last[i].mark, last[i].text);
    else strlcpy(line.detail, "quiet", sizeof line.detail);
  }
  if (!have_target) strlcpy(f.fm1_in, "not found", sizeof f.fm1_in);
  else if (in.at_ms) snprintf(f.fm1_in, sizeof f.fm1_in, "in: %s %s", in_tag, in.text);
  else strlcpy(f.fm1_in, "in: -", sizeof f.fm1_in);
  f.fm1_in_fresh = fresh(in);
  if (have_target) snprintf(f.fm1_own, sizeof f.fm1_own, "itself sent: %s", own.at_ms ? own.text : "-");
}

// ---------- Modes ----------
// A claim can't be taken back without re-enumerating, so a mode change restarts
// the USB host and every device enumerates again under the new mode's rules
// (docs/architecture.md, "Resource broker"). Runs on loop(), never on a USB task.
static void restart_host(platform::Mode mode) {
  DLOG_LOOP("host: restarting in %s mode",
            mode == platform::Mode::Load ? "Load" : mode == platform::Mode::Record ? "Record" : "Play");
  usb.end();
  broker.set_mode(mode);
#if APP_LOADER
  loader_has_fm1_midi = false;
#endif
  router = midi_core::Router();
  for (int i = 0; i < kMaxHubs; ++i) hub_addresses[i] = 0;
  taskENTER_CRITICAL(&shared_lock);
  for (auto &m : last_by_address) m = {};
  for (auto &r : refused_why) r = {};
  fm1_own = fm1_in = {};
  ++changes;
  taskEXIT_CRITICAL(&shared_lock);
  usb.begin();
}

#if APP_LOADER
static bool loader_owns_fm1_midi() { return loader_has_fm1_midi.load(); }
static void device_refusal(uint8_t address, char *out, size_t len) {
  taskENTER_CRITICAL(&shared_lock);
  const platform::Reason why = refused_why[address & 0x7F];
  taskEXIT_CRITICAL(&shared_lock);
  snprintf(out, len, "%s", why.line1);
}
#endif

#if APP_WEB_UI
// What GET /status shows: the frame the screen just drew, every device with
// its channel cost and the broker's refusal, and the Firmware page. Loop only.
static void publish_diag(const UiFrame *frame) {
  static DiagState d;  // ~4 KB: kept off the loop task's stack
  d.host_running = host_running;
  d.mode = broker.mode();
  if (frame) d.frame = *frame;
  d.device_count = 0;
  if (host_running) {
    EspUsbHostDeviceInfo devices[8];
    const size_t n = usb.getDevices(devices, 8);
    for (size_t i = 0; i < n; ++i) {
      const EspUsbHostDeviceInfo &dev = devices[i];
      DiagDevice &out = d.devices[d.device_count++];
      out = {};
      out.address = dev.address;
      out.vid = dev.vid;
      out.pid = dev.pid;
      out.hub = dev.isHub;
      strlcpy(out.name, dev.product, sizeof out.name);
      out.channels = uint8_t(usb.estimatedHcdChannelCount(dev.address));
      EspUsbHostInterfaceInfo itfs[8];
      out.interfaces = uint8_t(usb.getInterfaces(dev.address, itfs, 8));
      for (uint8_t k = 0; k < out.interfaces; ++k) out.claimed += itfs[k].claimed ? 1 : 0;
    }
  }
  taskENTER_CRITICAL(&shared_lock);
  for (int i = 0; i < d.device_count; ++i) d.devices[i].refusal = refused_why[d.devices[i].address & 0x7F];
  d.failed_sends = failed_count;
  taskEXIT_CRITICAL(&shared_lock);
#if APP_LOADER
  d.has_loader = true;
  loader_ui(d.loader);
#endif
  web_publish(d);
}
#endif

static void save_settings(const UiSettings &s) {
  prefs.putUChar("text_size", s.text_size);
  prefs.putUChar("typeface", s.typeface);
}

// ---------- Boot mode ----------
// Namespace "boot", key "computer": the build ID that asked to start in
// computer mode. Kept in NVS because moving the cable usually cuts power.
// The ID is the ELF SHA-256 prefix IDF keeps in the app (9 hex characters,
// CONFIG_APP_RETRIEVE_LEN_ELF_SHA).
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
// on this board: with the host never started, the Mac still saw no device,
// even after a power cycle (verified 2026-10-26). Which of these steps is the
// one that matters wasn't pinned down.
static void hand_usb_to_serial_jtag() {
  SET_PERI_REG_MASK(SYSTEM_PERIP_CLK_EN1_REG, SYSTEM_USB_DEVICE_CLK_EN);
  CLEAR_PERI_REG_MASK(SYSTEM_PERIP_RST_EN1_REG, SYSTEM_USB_DEVICE_RST);
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL | USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
  // Drop off the bus briefly so the computer enumerates afresh.
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  delay(100);
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
}

void setup() {
  read_crumbs();
  const midi_core::BootMode mode = read_boot_mode();
  prefs.begin("ui", false);
  const UiSettings settings = {prefs.getUChar("text_size", 1), prefs.getUChar("typeface", 0)};
  static char firmware[24];
  snprintf(firmware, sizeof firmware, "v %07lx%s %s", (unsigned long)FW_VERSION, FW_DIRTY ? "+" : "",
           platform::kAppSetLabel);
  UiHooks hooks = {save_settings, connect_computer, back_to_host, nullptr, nullptr, nullptr};
#if APP_LOADER
  hooks.loader_select = loader_select;
  hooks.loader_pin = loader_pin;
  hooks.loader_act = loader_act;
#endif
  ui_begin(kBoard, settings, hooks, firmware);

  if (last_crash[0][0] && mode != midi_core::BootMode::Computer) {
    // Safe mode after a crash or watchdog reset: no USB host (a crash loop would
    // otherwise repeat every few seconds), the port goes to the computer so the
    // board can be reflashed without BOOT+RST, and the screen says where it died.
    hand_usb_to_serial_jtag();
    char info[160];
    snprintf(info, sizeof info, "%s\n%s\n%s\nRST to try again.", last_crash[0], last_crash[1], last_crash[2]);
    ui_show_computer_mode(info);
#if APP_WEB_UI
    web_begin(true);  // so /crash and /coredump can be read without the screen
#endif
    return;
  }
  if (mode == midi_core::BootMode::Computer) {
    // The USB host is never installed; the port goes to the chip's
    // USB-Serial-JTAG device so the computer can reset it into download mode.
    hand_usb_to_serial_jtag();
    char info[40];
    snprintf(info, sizeof info, "Firmware %s", firmware + 2);
    ui_show_computer_mode(info);
    return;
  }
#if APP_LOADER && LOADER_DIAG
  // Diagnostic build (make ... DIAG=1): the USB host never starts, so the port
  // stays a serial console and a crash's reason and backtrace reach the Mac;
  // the store, the loader task and the Firmware page run as usual.
  hand_usb_to_serial_jtag();
  delay(3000);
  printf("diag: loader_begin\n");
  loader_begin(usb, {loader_owns_fm1_midi, device_refusal});
  printf("diag: loader_begin returned\n");
  return;
#endif
  usb.setClaimPlanner(plan_claims);
  usb.setClaimFilter(claim_filter);
  usb.onDeviceConnected(on_connected);
  usb.onDeviceDisconnected(on_disconnected);
  usb.onMidiMessage(on_midi);
#if APP_WEB_UI
  CRUMB_LOOP("web_begin");
  web_begin(false);
#endif
#if APP_LOADER
  loader_begin(usb, {loader_owns_fm1_midi, device_refusal});
#endif
  usb.begin();
  host_running = true;
}

void loop() {
  static uint32_t last_poll = 0, last_frame = 0, framed_changes = UINT32_MAX;
  static UiFrame frame;
#if APP_WEB_UI
  CRUMB_LOOP("web_loop");
  web_loop(millis());
  static uint32_t last_net = 0;
  if (millis() - last_net > 1000) {
    last_net = millis();
    ui_set_network(web_network_line());
    if (!host_running) publish_diag(nullptr);
  }
#endif
  if (!host_running) {  // computer mode: only the UI runs
#if APP_LOADER && LOADER_DIAG
    static uint32_t last_diag = 0;
    if (millis() - last_diag > 500) {
      last_diag = millis();
      static UiLoader loader;
      loader_ui(loader);
      ui_update_loader(loader);
    }
#endif
    ui_tick();
    delay(5);
    return;
  }
#if APP_LOADER
  if (loader_wanted_mode() != broker.mode() || loader_wants_restart()) {
    CRUMB_LOOP("restart_host");
    restart_host(loader_wanted_mode());
    loader_mode_changed(broker.mode());
  }
#endif
  const uint32_t now = millis();
  if (now - last_poll > 1000) {
    last_poll = now;
    CRUMB_LOOP("poll_hub_ports");
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
    CRUMB_LOOP("build_frame");
    build_frame(frame);
    CRUMB_LOOP("ui_update");
    ui_update(frame);
#if APP_LOADER
    static UiLoader loader;  // ~900 bytes: kept off the loop task's stack
    loader_ui(loader);
    CRUMB_LOOP("ui_update_loader");
    ui_update_loader(loader);
#endif
#if APP_WEB_UI
    CRUMB_LOOP("publish_diag");
    publish_diag(&frame);
#endif
  }
  CRUMB_LOOP("ui_tick");
  ui_tick();
  delay(5);
}
