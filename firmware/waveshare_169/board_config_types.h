// A board as board.cpp drives it at run time: the panel, touch and expander
// chips and their pins. Each build's board_config.h (tools/boards.py, from
// firmware/boards.json) fills in one kBoardConfig; later it can come from the
// board itself. Pins: GPIO n is n, expander pin n is 100 + n, none is -1.
// Tri-state fields (int8_t): -1 unset (the driver's or the rotation's own
// behaviour), 0 false, 1 true.
#pragma once

#include <stdint.h>

enum class PanelDriver : uint8_t { ST7789, ST7796, ILI9341 };
enum class TouchDriver : uint8_t { CST816, FT6336 };
enum class ExpanderDriver : uint8_t { None, TCA9554 };

constexpr int kMaxHoldPins = 4;

struct BoardConfig {
  PanelDriver panel;
  int16_t lcd_dc, lcd_cs, lcd_sck, lcd_mosi, lcd_miso, lcd_rst, lcd_bl;
  uint32_t spi_hz;
  int8_t spi_mode;  // -1: the panel class's own (ST7789/ST7796 3, ILI9341 0)
  bool ips;
  uint8_t rotation;  // Arduino_GFX's setRotation: 1 and 3 are the quarter turns
  int16_t offsets[4];
  int8_t bgr, mirror_x, mirror_y;
  TouchDriver touch;
  uint8_t touch_address;
  int16_t tp_sda, tp_scl, tp_rst, tp_int;
  int8_t touch_swap_xy, touch_invert_x, touch_invert_y;
  ExpanderDriver expander;
  uint8_t expander_address;
  int16_t hold_low[kMaxHoldPins], hold_high[kMaxHoldPins];  // -1 padded
  bool restart_on_power_on;
};

// MADCTL bits, the same on the ST7789, ST7796 and ILI9341.
constexpr uint8_t kMadctlMY = 0x80, kMadctlMX = 0x40, kMadctlMV = 0x20, kMadctlBGR = 0x08;

// Whether the profile changes the panel's MADCTL byte from Arduino_GFX's.
constexpr bool panel_overrides(int8_t bgr, int8_t mirror_x, int8_t mirror_y) {
  return bgr >= 0 || mirror_x >= 0 || mirror_y >= 0;
}

// The MADCTL byte: Arduino_GFX 1.6.9's setRotation table for the driver, then
// the profile's colour order and mirrors. Tested in tests/test_panel.cpp.
constexpr uint8_t panel_madctl(PanelDriver d, int rotation, int8_t bgr, int8_t mirror_x, int8_t mirror_y) {
  constexpr uint8_t st7789[4] = {0, kMadctlMX | kMadctlMV, kMadctlMX | kMadctlMY, kMadctlMY | kMadctlMV};
  constexpr uint8_t ili9341[4] = {kMadctlMX | kMadctlBGR, kMadctlMV | kMadctlBGR, kMadctlMY | kMadctlBGR,
                                  kMadctlMX | kMadctlMY | kMadctlMV | kMadctlBGR};
  const int r = rotation & 3;
  uint8_t m = d == PanelDriver::ILI9341 ? ili9341[r]
              : d == PanelDriver::ST7796 ? uint8_t(st7789[r] | kMadctlBGR)
                                         : st7789[r];
  if (bgr >= 0) m = bgr ? m | kMadctlBGR : m & ~kMadctlBGR;
  if (mirror_x > 0) m ^= kMadctlMX;
  if (mirror_y > 0) m ^= kMadctlMY;
  return m;
}
