// USB MIDI host on the Waveshare ESP32-S3-Touch-LCD-1.69.
//
// The board's USB-C port runs as a USB host (EspUsbHost), normally through a
// powered hub. Channel messages from any controller (e.g. a Launchkey) are
// forwarded to the FM-1. midi_core::Router decides what is forwarded; it never
// passes SysEx or any other system message, so a controller cannot put the
// FM-1 into its ROM bootloader or a firmware installer.
//
// This file gathers what the host knows into a UiFrame; host_ui (LVGL) draws
// it and runs the settings page. Board specifics live in board.cpp, from the
// build's profile in firmware/boards.json.
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
#include <esp_heap_caps.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/system_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include "apps.h"
#include "build_tag.h"
#include "crumbs.h"
#include "diag.h"
#include "board_config.h"
#include "board_profile.h"
#include "board_store.h"
#include "computer_link.h"
#include "boot_mode.h"
#include "broker.h"
#include "describe.h"
#include "host_ui.h"
#include "midi_core.h"
#include "status.h"
#if APP_LOADER
#include "add_firmware.h"
#include "loader_app.h"
#endif
#if APP_WEB_UI
#include "web.h"
#endif


EspUsbHost usb;
static Preferences prefs;
// fw_version.h: written by make build (FW_VERSION, the commit; FW_DIRTY, 1 with
// uncommitted changes). A header, not a -D flag, so a new commit recompiles
// only the files that include it.
#if __has_include("fw_version.h")
#include "fw_version.h"
#endif
#ifndef FW_VERSION
#define FW_VERSION 0
#endif
#ifndef FW_DIRTY
#define FW_DIRTY 0
#endif
// Read off the board by the web installer (build_tag.h). Settings prints it, so
// the linker keeps it.
extern "C" const platform::BuildTag kBuildTag __attribute__((section(".rodata_custom_desc"))) = {
    platform::kBuildTagMagic, platform::kBuildTagLayout, FW_DIRTY, BOARD_TAG_ID, FW_VERSION, APP_SET_LABEL};
static bool host_running = false;
#if APP_LOADER && LOADER_SOAK
static uint32_t soak_restart_at = 0;  // safe mode in a soak build: carry on after this
#endif
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
  if (crumbs.magic == kCrumbMagic) {
    // The tail carries on (crumbs.h); only the steps start over.
    crumbs.loop[0] = crumbs.loader[0] = crumbs.usb[0] = 0;
  } else {  // power-on: RTC memory holds noise
    memset(&crumbs, 0, sizeof crumbs);
    crumbs.magic = kCrumbMagic;
  }
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

// The USB task's crumb names the device too: "conn #2 1209:0001".
static void crumb_device(const char *what, const EspUsbHostDeviceInfo &d) {
  char buf[24];
  snprintf(buf, sizeof buf, "%s #%u %04x:%04x", what, d.address, d.vid, d.pid);
  CRUMB_USB(buf);
}

static void on_connected(const EspUsbHostDeviceInfo &d) {
  crumb_device("conn", d);
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
  crumb_device("disc", d);
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

// fm1_firmware: what the FM-1 runs, as the Loader knows it (null without one);
// fm1_unfinished: an install stopped part-way.
static void build_frame(UiFrame &f, const char *fm1_firmware, bool fm1_unfinished) {
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

  // Play's one status per stage: a controller playing now, the count sent.
  const char *playing = nullptr;
  for (int i = 0; i < f.controller_count && !playing; ++i)
    if (f.controllers[i].open && f.controllers[i].fresh) playing = f.controllers[i].tag;
  midi_core::stage_status(f.snapshot, f.view, {playing, f.sent, fm1_firmware, fm1_unfinished}, f.stages);
}

// ---------- Modes ----------
// A claim can't be taken back without re-enumerating, so a mode change restarts
// the USB host and every device enumerates again under the new mode's rules
// (docs/architecture.md, "Resource broker"). Runs on loop(), never on a USB task.
// When the USB host last started: the hub's ports aren't polled for a while
// after, while devices come and go.
static uint32_t host_started_ms = 0;

static void restart_host(platform::Mode mode) {
  DLOG_LOOP("host: restarting in %s mode",
            mode == platform::Mode::Load ? "Load" : mode == platform::Mode::Record ? "Record" : "Play");
  CRUMB_LOOP("usb.end");
  usb.end();
  CRUMB_LOOP("usb.end done");
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
  CRUMB_LOOP("usb.begin");
  usb.begin();
  CRUMB_LOOP("usb.begin done");
  host_started_ms = millis();
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
  if (!web_running()) return;  // Remote diagnostics off: nobody reads it
  static uint32_t last = 0;
  if (frame && millis() - last < 250) return;  // /status is polled once a second
  last = millis();
  static DiagState *copy = static_cast<DiagState *>(heap_caps_calloc(1, sizeof(DiagState), MALLOC_CAP_SPIRAM));
  if (!copy) return;
  DiagState &d = *copy;
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
#if APP_LOADER
  // Not while a job runs: a restart mid-install leaves the FM-1's app half-written.
  if (loader_busy()) return;
#endif
  Preferences boot;
  boot.begin("boot", false);
  boot.putString("computer", this_build());
  boot.end();
  esp_restart();
}

#if APP_WEB_UI
// Remote diagnostics (Setup): the Web UI's Wi-Fi page, off unless turned on.
// `everything` and DIAG_WEB=1 builds start with it on.
#if DIAG_DEFAULT_ON || APP_RECORDER
constexpr bool kDiagDefault = true;
#else
constexpr bool kDiagDefault = false;
#endif

static void set_diagnostics(bool on) {
#if APP_LOADER
  if (loader_busy()) return;  // a restart mid-install leaves the FM-1's app half-written
#endif
  prefs.putBool("diag", on);
  esp_restart();
}
#endif

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
  // The board's profile: the one stored on it, else this build's (board_store.h).
  const char *board_from = board_select();
  // BOOT held 3 s soon after a start: forget it, then Computer mode.
  board_watch_escape(connect_computer);
  const midi_core::BootMode mode = read_boot_mode();
  prefs.begin("ui", false);
  // An old "typeface" key from builds with a Pixel font stays in NVS, unread.
  UiSettings settings = {prefs.getUChar("text_size", 1)};
#if APP_WEB_UI
  settings.diagnostics = prefs.getBool("diag", kDiagDefault);
  web_set_enabled(settings.diagnostics);
#endif
  static char firmware[24];
  snprintf(firmware, sizeof firmware, "v %07lx%s %s", (unsigned long)kBuildTag.commit, kBuildTag.dirty ? "+" : "",
           kBuildTag.app_set);
  UiHooks hooks = {save_settings, connect_computer, back_to_host, nullptr, nullptr};
#if APP_WEB_UI
  hooks.set_diagnostics = set_diagnostics;
#endif
#if APP_LOADER
  hooks.loader_select = loader_select;
  hooks.loader_act = loader_act;
#endif
  ui_begin(board_profile(), settings, hooks, firmware);
#if APP_LOADER
  // A job's boot, or the boot after it: the job's screen from the start, not Play.
  if (mode != midi_core::BootMode::Computer)
    if (const char *title = loader_boot_title()) ui_show_switching(title);
#endif

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
#if APP_LOADER && LOADER_SOAK
    loader_soak_crashed();
    soak_restart_at = millis() + 90000;  // time for the Mac to fetch /crash and /coredump
#endif
    return;
  }
  if (mode == midi_core::BootMode::Computer) {
    // The USB host is never installed; the port goes to the chip's
    // USB-Serial-JTAG device so the computer can reset it into download mode.
    hand_usb_to_serial_jtag();
    // The page on outrigger.music can set the board's profile and add firmwares.
    const bool linked = computer_link_begin(firmware + 2, back_to_host);
    char info[160];
    snprintf(info, sizeof info, "Firmware %s\nBoard profile: %s%s", firmware + 2, board_from,
             linked ? "" : "\nUSB serial didn't start: the installer can't reach this board");
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
  broker.set_mode(loader_boot_mode());  // a job's boot runs in Load mode from the start
  if (loader_boot_mode() == platform::Mode::Load) {
    // The job's page opens with the first ui_update_loader (the job is active).
    // EspUsbHost's hub tracking opens and closes every unknown device every
    // 500 ms on the client task. A job doesn't need it, and it races the FM-1
    // leaving for ROM boot and the ROM device arriving. (It raised the rate of
    // checkpoint 3's hcd_urb_dequeue assert; the cause, a transfer freed while
    // IDF held it, is fixed in EspUsbHost's disconnect teardown.)
    usb.setHubTrackingEnabled(false);
  }
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
#if WIFI_BOOTLOOP
  // Test build only: restart 100 s after boot, as Back to MIDI does (Makefile).
#if APP_LOADER
  if (millis() > 100000 && !loader_busy()) esp_restart();
#else
  if (millis() > 100000) esp_restart();
#endif
#endif
#if BENCH && APP_LOADER
  // The bench's commands press what the screen would (web.cpp, Bench API).
  web_core::BenchCommand bench;
  if (web_take_bench_command(bench)) {
    switch (bench.kind) {
      case web_core::BenchKind::Select: loader_select(bench.index); break;
      case web_core::BenchKind::Switch: loader_act(UiAct::Switch, bench.index); break;
      case web_core::BenchKind::Backup: loader_act(UiAct::Backup, -1); break;
      case web_core::BenchKind::Retry: loader_act(UiAct::Retry, -1); break;
      default: break;
    }
  }
#endif
  static uint32_t last_net = 0;
  if (millis() - last_net > 1000) {
    last_net = millis();
#if !HEAP_ON_SCREEN  // the heap line has the spot (below)
    ui_set_network(web_network_line());
#endif
    if (!host_running) publish_diag(nullptr);
  }
#endif
  if (!host_running) {  // computer mode: only the UI runs
#if APP_LOADER
    static uint32_t last_add = 0;
    if (millis() - last_add > 250) {
      last_add = millis();
      char add_line[64];
      add_firmware_line(add_line, sizeof add_line);
      ui_set_computer_status(add_line);
    }
#endif
#if APP_LOADER && LOADER_SOAK
    if (soak_restart_at && int32_t(millis() - soak_restart_at) > 0) esp_restart();
#endif
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
  const uint32_t now = millis();
  // Hub port polls are control transfers on the hub, sent from this task. Not
  // in Load mode or around a host restart: checkpoint 3 crashed in ESP-IDF
  // (hcd_urb_dequeue assert) with a poll in flight as the FM-1 left for ROM
  // boot, and a timed-out poll abandons its transfer still queued.
  bool poll_ok = now - host_started_ms > 3000 && broker.mode() == platform::Mode::Play;
#if APP_LOADER
  // Loader builds don't poll yet: the polls ran in Play mode right up to
  // checkpoint 3's hcd_urb_dequeue crashes. That crash's cause is fixed in
  // EspUsbHost's disconnect teardown; turning the polls back on waits for the
  // hub-poll timeout fix (PR #2). Only the port letters on the chain screen are lost.
  poll_ok = false;
#endif
  if (poll_ok && now - last_poll > 1000) {
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
#if APP_LOADER
    static UiLoader loader;  // ~2 KB: kept off the loop task's stack
    loader_ui(loader);
    CRUMB_LOOP("build_frame");
    build_frame(frame, loader.running, loader.unfinished);
#else
    CRUMB_LOOP("build_frame");
    build_frame(frame, nullptr, false);
#endif
    CRUMB_LOOP("ui_update");
    ui_update(frame);
#if APP_LOADER
    CRUMB_LOOP("ui_update_loader");
    ui_update_loader(loader);
#endif
#if APP_WEB_UI
    CRUMB_LOOP("publish_diag");
    publish_diag(&frame);
#endif
  }
#if HEAP_ON_SCREEN
  // Diagnostic builds only (Makefile HEAP_ON_SCREEN=1): free RAM on Setup.
  static uint32_t last_heap = 0;
  if (now - last_heap > 1000) {
    last_heap = now;
    char heap[48];
    snprintf(heap, sizeof heap, "RAM free: int %u KB (min %u), DMA %u KB",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024));
    ui_set_network(heap);
  }
#endif
  CRUMB_LOOP("ui_tick");
  ui_tick();
  delay(5);
}
