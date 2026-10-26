// USB MIDI host on the Waveshare ESP32-S3-Touch-LCD-1.69.
//
// The board's USB-C port runs as a USB host (EspUsbHost), normally through a
// powered hub. Channel messages from any controller (e.g. a Launchkey) are
// forwarded to the FM-1. midi_core::Router decides what is forwarded; it never
// passes SysEx or any other system message, so a controller cannot put the
// FM-1 into its ROM bootloader or a firmware installer.
//
// The screen draws the signal chain Power -> Hub -> Controllers -> Router ->
// FM-1 (prototype "C2m"). midi_core::evaluate() decides each link's colour and
// the one problem worth explaining; this file only gathers facts and draws.
//
// Because the USB port is the host, the Mac loses this board's serial port
// once the sketch runs. To reflash, hold BOOT, tap RST, release BOOT.

#include <Arduino.h>
#include <U8g2lib.h>  // u8g2 font data; must come before Arduino_GFX_Library.h
#include <Arduino_GFX_Library.h>
#include <EspUsbHost.h>

#include "midi_core.h"
#include "status.h"

// Display wiring, from Waveshare's examples (board_config/pin_config.h).
constexpr int LCD_DC = 4;
constexpr int LCD_CS = 5;
constexpr int LCD_SCK = 6;
constexpr int LCD_MOSI = 7;
constexpr int LCD_RST = 8;
constexpr int LCD_BL = 15;
constexpr int LCD_WIDTH = 240;
constexpr int LCD_HEIGHT = 280;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
Arduino_GFX *panel = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */, true /* IPS */,
                                        LCD_WIDTH, LCD_HEIGHT, 0, 20, 0, 20);
// Drawn off-screen (134 KB, lands in PSRAM) and pushed in one go: no flicker.
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

EspUsbHost usb;

using midi_core::Level;

// ---------- Colours ----------
constexpr uint16_t kBlack = RGB565(0, 0, 0);
constexpr uint16_t kWhite = RGB565(242, 242, 242);
constexpr uint16_t kGrey = RGB565(128, 128, 128);
constexpr uint16_t kDim = RGB565(51, 51, 51);
constexpr uint16_t kGreen = RGB565(34, 221, 68);
constexpr uint16_t kSpine = RGB565(29, 107, 44);
constexpr uint16_t kYellow = RGB565(255, 225, 74);
constexpr uint16_t kOrange = RGB565(255, 165, 58);
constexpr uint16_t kRed = RGB565(255, 74, 74);
constexpr uint16_t kMagenta = RGB565(255, 106, 240);
constexpr uint16_t kProblemBg = RGB565(42, 21, 8);
constexpr uint16_t kProblemEdge = RGB565(106, 58, 16);

static uint16_t level_colour(Level l) {
  switch (l) {
    case Level::Ok: return kGreen;
    case Level::Warn: return kOrange;
    case Level::Bad: return kRed;
    default: return kDim;
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
static uint32_t sent_count = 0, blocked_count = 0;
static uint32_t changes = 0;               // bumped on anything worth redrawing

static midi_core::Router router;           // USB task only
constexpr int kMaxHubs = 2;
static volatile uint8_t hub_addresses[kMaxHubs] = {};

static void bump() { ++changes; }

static void on_connected(const EspUsbHostDeviceInfo &d) {
  router.device_connected(d.address, d.vid, d.pid);
  if (d.isHub) {
    for (int i = 0; i < kMaxHubs; ++i)
      if (hub_addresses[i] == 0) { hub_addresses[i] = d.address; break; }
  }
  taskENTER_CRITICAL(&shared_lock);
  bump();
  taskEXIT_CRITICAL(&shared_lock);
}

static void on_disconnected(const EspUsbHostDeviceInfo &d) {
  router.device_disconnected(d.address);
  for (int i = 0; i < kMaxHubs; ++i)
    if (hub_addresses[i] == d.address) hub_addresses[i] = 0;
  taskENTER_CRITICAL(&shared_lock);
  if (d.address < 128) last_by_address[d.address] = {};
  bump();
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
  bump();
  taskEXIT_CRITICAL(&shared_lock);
}

// ---------- Gathering facts for midi_core::evaluate() ----------
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

// ---------- Drawing ----------
// Fonts: helvB10 for node titles, 5x8 for details, helvR08 for the problem box.
static void text(int x, int top, const uint8_t *font, int ascent, uint16_t colour, const char *s) {
  gfx->setFont(font);
  gfx->setTextColor(colour);
  gfx->setCursor(x, top + ascent);  // u8g2 fonts position by baseline
  gfx->print(s);
}
static void title(int x, int top, uint16_t c, const char *s) { text(x, top, u8g2_font_helvB10_tr, 10, c, s); }
static void small(int x, int top, uint16_t c, const char *s) { text(x, top, u8g2_font_5x8_tr, 7, c, s); }
static void small_right(int right, int top, uint16_t c, const char *s) {
  small(right - static_cast<int>(strlen(s)) * 5, top, c, s);
}

static uint16_t mark_colour(char mark, bool fresh) {
  if (!fresh) return kGrey;
  return mark == '>' ? kGreen : mark == '!' ? kRed : mark == ' ' ? kWhite : kOrange;
}

static void port_tag(const midi_core::HostSnapshot &s, const midi_core::ChainView &v, int port, char out[5]) {
  for (int i = 0; i < s.device_count; ++i) {
    if (s.devices[i].port_id == (0x10 | (port + 1))) {
      midi_core::make_tag(s.devices[i].name, out);
      return;
    }
  }
  const char st = s.port_state[port];
  strlcpy(out, v.port_off_since_boot[port] ? "off" : st == 'C' ? "!" : st == 'E' ? "?" : "-", 5);
}

static void draw(const midi_core::HostSnapshot &s, const midi_core::ChainView &v) {
  // Copy the message state out of the USB task's hands first.
  LastMessage last[midi_core::kMaxDevices];
  LastMessage own, in;
  char in_tag[4];
  uint32_t sent, blocked;
  taskENTER_CRITICAL(&shared_lock);
  for (int i = 0; i < s.device_count; ++i) last[i] = last_by_address[s.devices[i].address & 0x7F];
  own = fm1_own;
  in = fm1_in;
  memcpy(in_tag, fm1_in_tag, sizeof in_tag);
  sent = sent_count;
  blocked = blocked_count;
  taskEXIT_CRITICAL(&shared_lock);
  const uint32_t now = millis();
  auto fresh = [now](const LastMessage &m) { return m.at_ms && now - m.at_ms < 400; };

  gfx->fillScreen(kBlack);
  char buf[48];

  int controllers = 0;
  const midi_core::DeviceView *target = nullptr;
  for (int i = 0; i < s.device_count; ++i) {
    if (!s.devices[i].is_midi) continue;
    if (s.devices[i].role == midi_core::Role::Target) target = &s.devices[i];
    else ++controllers;
  }

  // Node heights; the controller group grows one row per device.
  const int group_h = 18 + max(1, controllers) * 12 + 6;
  const int heights[5] = {30, 30, group_h, 30, 30};
  const Level levels[5] = {v.power, v.hub, v.controllers, v.router, v.target};

  int y = 16;
  for (int n = 0; n < 5; ++n) {
    if (n < 4) gfx->fillRect(26, y + 8, 2, heights[n] - 6, n + 1 < v.first_broken ? kSpine : kDim);
    gfx->fillCircle(27, y + 5, 3, level_colour(levels[n]));
    const uint16_t sub = levels[n] == Level::Ok ? kGrey : level_colour(levels[n]);
    switch (n) {
      case 0:
        title(38, y - 1, kWhite, "Power");
        small(38, y + 13, sub, "USB-C 5V");
        break;
      case 1: {
        title(38, y - 1, kWhite, "Hub");
        if (!s.hub_present) {
          small(38, y + 13, sub, s.device_count ? "none (direct)" : "nothing answers");
        } else {
          int pos = snprintf(buf, sizeof buf, "ports");
          for (int p = 0; p < s.hub_ports && pos < 40; ++p) {
            char tag[5];
            port_tag(s, v, p, tag);
            pos += snprintf(buf + pos, sizeof buf - pos, " %d:%s", p + 1, tag);
          }
          small(38, y + 13, sub, buf);
        }
        break;
      }
      case 2: {
        if (controllers > 1) snprintf(buf, sizeof buf, "Controllers (%d)", controllers);
        title(38, y - 1, kYellow, controllers > 1 ? buf : "Controller");
        int gy = y + 15;
        if (controllers == 0) small(40, gy, kGrey, "none");
        for (int i = 0; i < s.device_count; ++i) {
          const midi_core::DeviceView &d = s.devices[i];
          if (!d.is_midi || d.role == midi_core::Role::Target) continue;
          char tag[4];
          midi_core::make_tag(d.name, tag);
          gfx->fillCircle(43, gy + 3, 2, d.midi_open ? kGreen : kRed);
          small(50, gy, d.midi_open ? kWhite : kRed, tag);
          if (!d.midi_open) {
            snprintf(buf, sizeof buf, "port %X.%X  no USB channel", d.port_id >> 4, d.port_id & 0xF);
            small(70, gy, kRed, buf);
          } else if (last[i].at_ms) {
            snprintf(buf, sizeof buf, "%c %s", last[i].mark, last[i].text);
            small(70, gy, mark_colour(last[i].mark, fresh(last[i])), buf);
          } else {
            small(70, gy, kGrey, "quiet");
          }
          gy += 12;
        }
        break;
      }
      case 3:
        title(38, y - 1, kWhite, "Router");
        snprintf(buf, sizeof buf, "%lu passed  %lu SysEx blocked", (unsigned long)sent, (unsigned long)blocked);
        small(38, y + 13, sub, buf);
        break;
      case 4:
        title(38, y - 1, kMagenta, "FM-1");
        if (!target) {
          small(38, y + 13, sub, "not found");
        } else if (in.at_ms) {
          snprintf(buf, sizeof buf, "in: %s %s", in_tag, in.text);
          small(38, y + 13, fresh(in) ? kGreen : kGrey, buf);
        } else {
          small(38, y + 13, kGrey, "in: -");
        }
        break;
    }
    y += heights[n];
  }

  snprintf(buf, sizeof buf, "USB %u/8", s.channels_used);
  small_right(218, 16, s.channels_used >= 8 ? kYellow : kGrey, buf);

  if (v.has_problem) {
    gfx->fillRoundRect(14, 240, 212, 30, 5, kProblemBg);
    gfx->drawRoundRect(14, 240, 212, 30, 5, kProblemEdge);
    text(22, 243, u8g2_font_helvR08_tr, 8, kOrange, v.problem.line1);
    text(22, 255, u8g2_font_helvR08_tr, 8, kWhite, v.problem.line2);
  } else if (target) {
    snprintf(buf, sizeof buf, "FM-1 itself sent: %s", own.at_ms ? own.text : "-");
    small(38, min(y - 2, 250), kGrey, buf);
  }
  gfx->flush();
}

void setup() {
  gfx->begin();
  gfx->setTextWrap(false);
  gfx->fillScreen(kBlack);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  title(38, 120, kWhite, "Starting USB host");
  gfx->flush();

  usb.onDeviceConnected(on_connected);
  usb.onDeviceDisconnected(on_disconnected);
  usb.onMidiMessage(on_midi);
  if (!usb.begin()) {
    gfx->fillScreen(kBlack);
    title(38, 110, kRed, "usb.begin failed");
    small(38, 130, kRed, usb.lastErrorName());
    gfx->flush();
    while (true) delay(1000);
  }
}

void loop() {
  static uint32_t last_poll = 0, last_draw = 0, drawn_changes = UINT32_MAX;
  const uint32_t now = millis();
  if (now - last_poll > 1000) {
    last_poll = now;
    poll_hub_ports();
  }
  uint32_t c;
  taskENTER_CRITICAL(&shared_lock);
  c = changes;
  taskEXIT_CRITICAL(&shared_lock);
  // Redraw on any change (at most ~20 fps) and twice a second regardless, so
  // "fresh" highlights fade and time-based problems appear.
  if ((c != drawn_changes && now - last_draw > 50) || now - last_draw > 500) {
    drawn_changes = c;
    last_draw = now;
    const midi_core::HostSnapshot s = take_snapshot();
    draw(s, midi_core::evaluate(s));
  }
  delay(5);
}
