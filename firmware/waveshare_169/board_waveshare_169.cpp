// Board profile and drivers for the Waveshare ESP32-S3-Touch-LCD-1.69:
// ST7789V2 240x280 SPI panel with rounded corners, CST816T touch on I2C.
// Pins from Waveshare's examples (board_config/pin_config.h).

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include "board_profile.h"

extern const BoardProfile kBoard = {"Waveshare ESP32-S3-Touch-LCD-1.69", 240, 280, ScreenShape::Rounded, 40, 0};

constexpr int LCD_DC = 4, LCD_CS = 5, LCD_SCK = 6, LCD_MOSI = 7, LCD_RST = 8, LCD_BL = 15;
constexpr int TP_SDA = 11, TP_SCL = 10, TP_RST = 13, TP_INT = 14;
constexpr uint8_t kTouchAddress = 0x15;

static Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
static Arduino_GFX *panel = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */, true /* IPS */,
                                               240, 280, 0, 20, 0, 20);

bool board_display_begin() {
  if (!panel->begin(80000000)) return false;
  panel->fillScreen(0);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(50);
  Wire.begin(TP_SDA, TP_SCL, 400000);
  // Keep the touch chip awake; asleep it ignores I2C until touched.
  Wire.beginTransmission(kTouchAddress);
  Wire.write(0xFE);
  Wire.write(0x01);
  Wire.endTransmission();
  return true;
}

void board_display_flush(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels) {
  panel->draw16bitRGBBitmap(x, y, const_cast<uint16_t *>(pixels), w, h);
}

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
  *x = rx < kBoard.width ? rx : kBoard.width - 1;
  *y = ry < kBoard.height ? ry : kBoard.height - 1;
  return true;
}
