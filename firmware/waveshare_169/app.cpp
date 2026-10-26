// USB MIDI host on the Waveshare ESP32-S3-Touch-LCD-1.69.
//
// The board's USB-C port runs as a USB host (EspUsbHost), normally through a
// powered hub. Every incoming MIDI message is shown on the screen, and channel
// messages from any controller (e.g. a Launchkey) are forwarded to the FM-1.
// midi_core::route_to_target() decides what is forwarded; it never passes
// SysEx or any other system message, so a controller cannot put the FM-1 into
// its ROM bootloader or a firmware installer.
//
// Log prefixes: '>' forwarded to the FM-1, 'x' from a controller but not
// forwarded, ' ' from the FM-1 itself.
//
// Because the USB port is the host, the Mac loses this board's serial port
// once the sketch runs. To reflash, hold BOOT, tap RST, release BOOT.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <EspUsbHost.h>

#include "midi_core.h"

// Display wiring, from Waveshare's examples (board_config/pin_config.h).
constexpr int LCD_DC = 4;
constexpr int LCD_CS = 5;
constexpr int LCD_SCK = 6;
constexpr int LCD_MOSI = 7;
constexpr int LCD_RST = 8;
constexpr int LCD_BL = 15;
constexpr int LCD_WIDTH = 240;
constexpr int LCD_HEIGHT = 280;

// Text size 2 is a 12x16 px cell: 20 columns by 17 rows on this panel.
constexpr int kCharH = 16;
constexpr int kHeaderRows = 3;
constexpr int kLogRows = LCD_HEIGHT / kCharH - kHeaderRows;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */, true /* IPS */,
                                      LCD_WIDTH, LCD_HEIGHT, 0, 20, 0, 20);

EspUsbHost usb;

// Shared between the USB client task (writer) and loop() (reader).
static portMUX_TYPE shared_lock = portMUX_INITIALIZER_UNLOCKED;
static midi_core::LineLog<kLogRows> midi_log;
static char device_line[2][midi_core::kLineLen] = {"No USB device", ""};
static uint32_t device_changes = 0;
static uint32_t message_count = 0;

static void set_device_lines(const char *a, const char *b) {
  taskENTER_CRITICAL(&shared_lock);
  strlcpy(device_line[0], a, midi_core::kLineLen);
  strlcpy(device_line[1], b, midi_core::kLineLen);
  ++device_changes;
  taskEXIT_CRITICAL(&shared_lock);
}

static int device_count = 0;

// Routing state, touched only on the USB client task.
static midi_core::Role roles[128];
static bool role_known[128] = {};
static uint8_t target_address = 0;  // 0 = no FM-1 connected

// Hubs seen so far, polled from loop() so the screen shows what each hub port
// is doing. Written by the USB task, read by loop().
constexpr int kMaxHubs = 2;
static volatile uint8_t hub_addresses[kMaxHubs] = {};

// Connects and disconnects also go into the scrolling log, so every device on
// a hub shows up, not just the most recent one.
static void push_log(const char *line) {
  taskENTER_CRITICAL(&shared_lock);
  midi_log.push(line);
  taskEXIT_CRITICAL(&shared_lock);
}

static void on_connected(const EspUsbHostDeviceInfo &d) {
  char ids[midi_core::kLineLen];
  snprintf(ids, sizeof ids, "+%04X:%04X p%02X", d.vid, d.pid, d.portId);
  push_log(ids);
  if (d.address < 128) {
    roles[d.address] = midi_core::device_role(d.vid, d.pid);
    role_known[d.address] = true;
    if (roles[d.address] == midi_core::Role::Target) target_address = d.address;
  }
  if (d.isHub) {
    for (int i = 0; i < kMaxHubs; ++i)
      if (hub_addresses[i] == 0) { hub_addresses[i] = d.address; break; }
  }
  char count[midi_core::kLineLen];
  snprintf(count, sizeof count, "%d USB device(s)", ++device_count);
  set_device_lines(d.product[0] ? d.product : "(no name)", count);
}

static void on_disconnected(const EspUsbHostDeviceInfo &d) {
  char ids[midi_core::kLineLen];
  snprintf(ids, sizeof ids, "-%04X:%04X p%02X", d.vid, d.pid, d.portId);
  push_log(ids);
  if (d.address < 128) role_known[d.address] = false;
  if (d.address == target_address) target_address = 0;
  for (int i = 0; i < kMaxHubs; ++i)
    if (hub_addresses[i] == d.address) hub_addresses[i] = 0;
  char count[midi_core::kLineLen];
  snprintf(count, sizeof count, "%d USB device(s)", --device_count);
  set_device_lines("Disconnected", count);
}

// Forwards a controller's message to the FM-1. Returns true if it was sent.
// Unknown sources and the FM-1 itself are never forwarded, and the explicit
// target address keeps midiSend() from picking "any" MIDI device.
static bool forward(const EspUsbHostMidiMessage &m) {
  const uint8_t dest = target_address;
  if (dest == 0 || dest == ESP_USB_HOST_ANY_ADDRESS) return false;
  if (m.address >= 128 || !role_known[m.address]) return false;
  if (roles[m.address] != midi_core::Role::Controller) return false;
  uint8_t packet[4];
  if (!midi_core::route_to_target(m.cable, m.status, m.data1, m.data2, packet)) return false;
  // midiSend() only submits the transfer, so it is safe on the USB task.
  return usb.midiSend(packet, sizeof packet, dest);
}

static void on_midi(const EspUsbHostMidiMessage &m) {
  const bool from_target = m.address == target_address && target_address != 0;
  const char mark = from_target ? ' ' : forward(m) ? '>' : 'x';
  char text[midi_core::kLineLen];
  midi_core::format_message(m.status, m.data1, m.data2, m.raw, m.length, text);
  char line[midi_core::kLineLen];
  snprintf(line, sizeof line, "%c%s", mark, text);
  taskENTER_CRITICAL(&shared_lock);
  midi_log.push(line);
  ++message_count;
  taskEXIT_CRITICAL(&shared_lock);
}

// The panel's corners are rounded, so text is inset, more on the first and
// last rows where the curve cuts deepest.
static void draw_row(int row, const char *text, uint16_t color) {
  const int y = row * kCharH;
  const bool edge_row = row == 0 || row == LCD_HEIGHT / kCharH - 1;
  gfx->fillRect(0, y, LCD_WIDTH, kCharH, RGB565_BLACK);
  gfx->setCursor(edge_row ? 24 : 8, y);
  gfx->setTextColor(color);
  gfx->print(text);
}

void setup() {
  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextSize(2);
  gfx->setTextWrap(false);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  draw_row(0, "USB MIDI host", RGB565_CYAN);
  draw_row(1, "Starting USB host", RGB565_WHITE);

  usb.onDeviceConnected(on_connected);
  usb.onDeviceDisconnected(on_disconnected);
  usb.onMidiMessage(on_midi);
  if (!usb.begin()) {
    char err[midi_core::kLineLen];
    snprintf(err, sizeof err, "ERR %s", usb.lastErrorName());
    draw_row(1, "usb.begin failed", RGB565_RED);
    draw_row(2, err, RGB565_RED);
    return;
  }
  set_device_lines("No USB device", "");
}

// One character per hub port: '.' unpowered, 'p' powered and empty,
// 'C' device connected, 'E' connected and enabled by the hub.
static void draw_hub_status() {
  char line[midi_core::kLineLen] = "";
  size_t pos = 0;
  for (int i = 0; i < kMaxHubs; ++i) {
    const uint8_t addr = hub_addresses[i];
    if (addr == 0) continue;
    pos += snprintf(line + pos, sizeof line - pos, "h%u:", addr);
    for (uint8_t port = 1; port <= 4 && pos + 2 < sizeof line; ++port) {
      uint16_t status = 0, change = 0;
      char c = '?';
      if (usb.getHubPortStatus(addr, port, status, change))
        c = (status & 0x0002) ? 'E' : (status & 0x0001) ? 'C' : (status & 0x0100) ? 'p' : '.';
      line[pos++] = c;
    }
    line[pos++] = ' ';
    line[pos] = '\0';
  }
  if (pos == 0) snprintf(line, sizeof line, "%s", usb.lastErrorName());
  draw_row(0, line, RGB565_CYAN);
}

void loop() {
  static uint32_t last_hub_poll = 0;
  if (millis() - last_hub_poll > 2000) {
    last_hub_poll = millis();
    draw_hub_status();
  }

  static uint32_t drawn_device_changes = UINT32_MAX;
  static uint32_t drawn_total = UINT32_MAX;

  char dev[2][midi_core::kLineLen];
  char rows[kLogRows][midi_core::kLineLen];
  uint32_t dev_changes, total;

  taskENTER_CRITICAL(&shared_lock);
  dev_changes = device_changes;
  total = midi_log.total();
  memcpy(dev, device_line, sizeof dev);
  for (int i = 0; i < kLogRows; ++i)
    strlcpy(rows[i], midi_log.newest(i), midi_core::kLineLen);
  taskEXIT_CRITICAL(&shared_lock);

  if (dev_changes != drawn_device_changes) {
    draw_row(1, dev[0], RGB565_YELLOW);
    draw_row(2, dev[1], RGB565_DARKGREY);
    drawn_device_changes = dev_changes;
  }
  if (total != drawn_total) {
    // Newest line at the top of the log area.
    for (int i = 0; i < kLogRows; ++i)
      draw_row(kHeaderRows + i, rows[i], i == 0 ? RGB565_GREEN : RGB565_WHITE);
    drawn_total = total;
  }
  delay(20);
}
