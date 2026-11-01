// The board drivers, for whichever board this build is for. Its profile in
// firmware/boards.json names the pins and the chips; tools/boards.py turns it
// into board_config.h (the Makefile's BOARD=). A new board is a new profile;
// code goes here only for a display, touch or expander chip not driven yet.
//
// Displays: ILI9341, ST7789 and ST7796 over SPI (Arduino_GFX). Touch: CST816 and FT6336,
// which share one register layout (status at 0x02, the first point at 0x03-0x06).
// Expander: TCA9554 on the touch chip's I2C bus, for boards that put the panel's
// reset or a hold-low pin on it.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include "board_config.h"
#include "board_profile.h"

using namespace board_config;

extern const BoardProfile kBoard = BOARD_PROFILE_INIT;

// A profile's pin: GPIO n is n, expander pin n is 100 + n, none is -1.
constexpr int16_t kExio = 100;
constexpr int8_t gpio_or_none(int16_t pin) { return pin >= 0 && pin < kExio ? pin : GFX_NOT_DEFINED; }

static Arduino_DataBus *bus = new Arduino_ESP32SPI(gpio_or_none(kLcdDc), gpio_or_none(kLcdCs), gpio_or_none(kLcdSck),
                                                   gpio_or_none(kLcdMosi), gpio_or_none(kLcdMiso));
#if BOARD_DISPLAY_ST7789
static Arduino_GFX *panel = new Arduino_ST7789(bus, gpio_or_none(kLcdRst), kRotation, kIps, kBoard.width, kBoard.height,
                                               kOffsets[0], kOffsets[1], kOffsets[2], kOffsets[3]);
#elif BOARD_DISPLAY_ST7796
static Arduino_GFX *panel = new Arduino_ST7796(bus, gpio_or_none(kLcdRst), kRotation, kIps, kBoard.width, kBoard.height,
                                               kOffsets[0], kOffsets[1], kOffsets[2], kOffsets[3]);
#elif BOARD_DISPLAY_ILI9341
static Arduino_GFX *panel = new Arduino_ILI9341(bus, gpio_or_none(kLcdRst), kRotation, kIps, kBoard.width, kBoard.height,
                                                kOffsets[0], kOffsets[1], kOffsets[2], kOffsets[3]);
#endif

// After its reset the touch chip ignores I2C for a while: FocalTech asks for
// 300 ms; the CST816 answers after 50.
#if BOARD_TOUCH_FT6336
constexpr int kTouchBootMs = 300;
#else
constexpr int kTouchBootMs = 50;
#endif

#if BOARD_EXPANDER_TCA9554
// Its registers: 0x01 the outputs, 0x03 the directions (1 = input). It powers
// up all inputs, pulled high.
static uint8_t exio_out = 0xFF, exio_dir = 0xFF;
static void exio_set(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kExpanderAddress);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}
#endif

static void pin_write(int16_t pin, bool high) {
  if (pin < 0) return;
#if BOARD_EXPANDER_TCA9554
  if (pin >= kExio) {
    const uint8_t bit = 1 << (pin - kExio);
    exio_out = high ? exio_out | bit : exio_out & ~bit;
    exio_dir &= ~bit;
    exio_set(0x01, exio_out);  // the level first, so the pin never drives the old one
    exio_set(0x03, exio_dir);
    return;
  }
#endif
  pinMode(pin, OUTPUT);
  digitalWrite(pin, high ? HIGH : LOW);
}

bool board_display_begin() {
#if BOARD_EXPANDER_TCA9554
  Wire.begin(kTpSda, kTpScl, 400000);
#endif
  // Pins wired to something Outrigger doesn't use (a buzzer, a speaker
  // amplifier) that would make noise left floating or on (boards.json, hold_why).
  for (int16_t pin : kHoldLow) pin_write(pin, false);
  for (int16_t pin : kHoldHigh) pin_write(pin, true);
  if (kLcdRst >= kExio) {  // Arduino_GFX resets the panel itself only from a GPIO
    pin_write(kLcdRst, true);
    delay(10);
    pin_write(kLcdRst, false);
    delay(10);
    pin_write(kLcdRst, true);
    delay(200);
  }
  if (!panel->begin(kSpiHz)) return false;
  panel->fillScreen(0);
  pin_write(kLcdBl, true);

  if (kTpRst >= 0) {
    pin_write(kTpRst, false);
    delay(10);
    pin_write(kTpRst, true);
    delay(kTouchBootMs);
  }
#if !BOARD_EXPANDER_TCA9554
  Wire.begin(kTpSda, kTpScl, 400000);
#endif
#if BOARD_TOUCH_CST816
  // Keep the touch chip awake; asleep it ignores I2C until touched.
  Wire.beginTransmission(kTouchAddress);
  Wire.write(0xFE);
  Wire.write(0x01);
  Wire.endTransmission();
#endif
  return true;
}

void board_display_flush(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels) {
  panel->draw16bitRGBBitmap(x, y, const_cast<uint16_t *>(pixels), w, h);
}

#if UI_TOUCH_PROBE
int16_t board_touch_raw[2] = {-1, -1};
#endif

bool board_touch_read(int16_t *x, int16_t *y) {
  uint8_t buf[7];
  Wire.beginTransmission(kTouchAddress);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(kTouchAddress, (uint8_t)sizeof buf) != sizeof buf) return false;
  for (uint8_t &b : buf) b = Wire.read();
  const uint8_t fingers = buf[2] & 0x0F;
  if (buf[2] == 0xFF || fingers == 0) return false;
  // Clamp, don't drop: a press reported a pixel past the edge (the glass's
  // bottom rim) is still a press on whatever sits there.
  const int16_t rx = ((buf[3] & 0x0F) << 8) | buf[4], ry = ((buf[5] & 0x0F) << 8) | buf[6];
#if UI_TOUCH_PROBE
  board_touch_raw[0] = rx;
  board_touch_raw[1] = ry;
#endif
  *x = rx < kBoard.width ? rx : kBoard.width - 1;
  *y = ry < kBoard.height ? ry : kBoard.height - 1;
  return true;
}
