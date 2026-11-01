// `make probe BOARD=<id>`: checks a board profile on the board itself, before
// Outrigger runs on it (docs/boards.md, "Adding a board"). It runs the same
// drivers (firmware/waveshare_169/board.cpp, from the build's board_config.h)
// but keeps the USB port a serial device, so the Mac can read what it finds:
//
// - the reset reason, PSRAM and flash;
// - every I2C address that answers on the touch bus, and the AXP2101's power
//   rails if one is there;
// - the screen: four corner squares (top-left red, top-right green,
//   bottom-left blue, bottom-right white) and a grey cross, so a photo or a
//   sentence says whether it's upright, mirrored or in the wrong colour order;
// - touch: each press is logged and drawn as a yellow dot under the finger.
//
// Nothing here is Outrigger; it never acts as a USB host.
#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>

#include "board.cpp"  // the drivers themselves, so the probe checks exactly what Outrigger runs

static uint16_t line[480 * 8];

static void fill(int x, int y, int w, int h, uint16_t colour) {
  for (int i = 0; i < w * 8; ++i) line[i] = colour;
  for (int row = y; row < y + h; row += 8) board_display_flush(x, row, w, min(8, y + h - row), line);
}

static const char *reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_USB: return "USB";
    case ESP_RST_EXT: return "external pin";
    default: return "other";
  }
}

static int read_reg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(addr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

static void pattern() {
  const int W = kBoard.width, H = kBoard.height, S = W / 4;
  fill(0, 0, W, H, 0x0000);
  fill(0, 0, S, S, 0xF800);          // red, top-left
  fill(W - S, 0, S, S, 0x07E0);      // green, top-right
  fill(0, H - S, S, S, 0x001F);      // blue, bottom-left
  fill(W - S, H - S, S, S, 0xFFFF);  // white, bottom-right
  fill(W / 2 - 1, 0, 2, H, 0x8410);  // grey cross
  fill(0, H / 2 - 1, W, 2, 0x8410);
}

static bool display_ok;

// Repeated every 5 s: the Mac's reader may open the port after the first one.
static void report() {
  Serial.printf("probe: %s (tag %d)\n", kBoard.name, BOARD_TAG_ID);
  Serial.printf("probe: reset %s; PSRAM %u bytes; flash %u bytes\n", reset_reason(), (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFlashChipSize());
  Serial.printf("probe: board_display_begin %s\n", display_ok ? "ok" : "FAILED");
  Serial.print("probe: I2C answers at");
  for (uint8_t a = 0x08; a < 0x78; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02x", a);
  }
  Serial.println();
  if (read_reg(0x34, 0x03) >= 0) {
    Serial.printf("probe: AXP2101 status 0x00=%02x 0x01=%02x; DCDC on 0x80=%02x; LDO on 0x90=%02x 0x91=%02x; wdt 0x18=%02x\n",
                  read_reg(0x34, 0x00), read_reg(0x34, 0x01), read_reg(0x34, 0x80), read_reg(0x34, 0x90),
                  read_reg(0x34, 0x91), read_reg(0x34, 0x18));
  }
}

// Prints from boot, in its own task: if the drivers hang, the log still says
// the board is alive and how far setup got.
static volatile const char *stage = "boot";
static void heartbeat(void *) {
  for (;;) {
    Serial.printf("probe: alive %lu ms, at %s\n", (unsigned long)millis(), (const char *)stage);
    delay(2000);
  }
}

void setup() {
  Serial.begin(115200);
  xTaskCreate(heartbeat, "probe", 4096, nullptr, 1, nullptr);
  stage = "board_display_begin";
  display_ok = board_display_begin();
  stage = "pattern";
  pattern();
  stage = "running";
  report();
}

void loop() {
  int16_t x, y;
  static uint32_t last = 0;
  if (board_touch_read(&x, &y)) {
    fill(max(0, x - 3), max(0, y - 3), 7, 7, 0xFFE0);
    if (millis() - last > 150) Serial.printf("probe: touch %d,%d\n", x, y);
    last = millis();
  }
  static uint32_t last_report = 0;
  if (millis() - last_report > 5000) {
    last_report = millis();
    report();
  }
  delay(10);
}
