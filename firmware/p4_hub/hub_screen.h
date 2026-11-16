// Big-P4's 320x480 ST7796 (SPI) through an Arduino_GFX canvas in PSRAM. The
// panel has no MISO wired, so it can't be read back: GET /screen.bmp serves the
// canvas, which is exactly what the last flush sent to the panel.
// Pins: Waveshare's ESP32-P4-WIFI6-Touch-LCD-3.5 board layer (MOSI 20, SCLK 21,
// CS 23, DC 26, RST 27, backlight 28).
#pragma once
#include <stddef.h>
#include <stdint.h>

bool hub_screen_begin();
// Draws the status screen from these lines and flushes it to the panel.
void hub_screen_draw(const char *const *lines, int count);
// Copies the last flushed frame (RGB565, 320x480, top-down) into out. False
// when there is no screen.
bool hub_screen_snapshot(uint16_t *out);
constexpr int kScreenW = 320, kScreenH = 480;
uint32_t hub_screen_frames();
uint32_t hub_screen_flush_us();
