// Host-side tests for firmware/waveshare_169/board_config_types.h's pure parts:
// the panel's MADCTL byte (Arduino_GFX's per-driver table, then the profile's
// bgr and mirror fields) and the touch axes (touch_map.h, rotation-derived unless
// the profile sets swap_xy / invert_x / invert_y).
#include <cstdio>

#include "board_config_types.h"
#include "touch_map.h"

static int failures = 0;
#define EXPECT(cond, ...)       \
  do {                          \
    if (!(cond)) {              \
      printf(__VA_ARGS__);      \
      printf("\n");             \
      ++failures;               \
    }                           \
  } while (0)

static void madctl(PanelDriver d, int rot, int8_t bgr, int8_t mx, int8_t my, uint8_t want, const char *what) {
  const uint8_t got = panel_madctl(d, rot, bgr, mx, my);
  EXPECT(got == want, "madctl %s: 0x%02x, wanted 0x%02x", what, got, want);
}

static void touch(int rx, int ry, int rot, int8_t sw, int8_t ix, int8_t iy, int wx, int wy, const char *what) {
  int16_t x, y;
  touch_map::to_screen(rx, ry, rot, 320, 480, &x, &y, sw, ix, iy);
  EXPECT(x == wx && y == wy, "touch %s: %d,%d, wanted %d,%d", what, x, y, wx, wy);
}

int main() {
  // Arduino_GFX 1.6.9's own tables (setRotation), unchanged when the profile sets nothing.
  madctl(PanelDriver::ST7789, 0, -1, -1, -1, 0x00, "st7789 r0");
  madctl(PanelDriver::ST7789, 1, -1, -1, -1, 0x60, "st7789 r1");
  madctl(PanelDriver::ST7796, 0, -1, -1, -1, 0x08, "st7796 r0");
  madctl(PanelDriver::ST7796, 1, -1, -1, -1, 0x68, "st7796 r1");
  madctl(PanelDriver::ST7796, 3, -1, -1, -1, 0xA8, "st7796 r3");
  madctl(PanelDriver::ILI9341, 0, -1, -1, -1, 0x48, "ili9341 r0");
  madctl(PanelDriver::ILI9341, 1, -1, -1, -1, 0x28, "ili9341 r1");
  madctl(PanelDriver::ILI9341, 3, -1, -1, -1, 0xE8, "ili9341 r3");
  // The profile's fields: bgr sets or clears 0x08; a mirror flips MX or MY.
  madctl(PanelDriver::ST7789, 0, 1, -1, -1, 0x08, "st7789 bgr");
  madctl(PanelDriver::ST7796, 0, 0, -1, -1, 0x00, "st7796 rgb");
  madctl(PanelDriver::ST7796, 1, -1, 1, -1, 0x28, "st7796 r1 mirror_x");
  madctl(PanelDriver::ILI9341, 0, -1, -1, 1, 0xC8, "ili9341 mirror_y");
  madctl(PanelDriver::ILI9341, 0, -1, 0, 0, 0x48, "explicit false changes nothing");
  EXPECT(!panel_overrides(-1, -1, -1), "nothing set: Arduino_GFX's byte stays");
  EXPECT(panel_overrides(-1, 0, -1), "a field set to false still counts as set");

  // Touch: unset fields follow the rotation (touch_map's own cases)...
  touch(319, 0, 1, -1, -1, -1, 0, 0, "r1 follows rotation");
  touch(37, 41, 0, -1, -1, -1, 37, 41, "r0 as reported");
  // ...and a set field overrides that axis: a glass mounted turned against the panel.
  touch(37, 41, 0, -1, 1, -1, 282, 41, "r0 invert_x");
  touch(37, 41, 0, 1, -1, -1, 41, 37, "r0 swap_xy");
  touch(319, 0, 1, -1, -1, 0, 0, 319, "r1 invert_y off");
  if (failures) return 1;
  printf("panel: MADCTL and touch axes follow Arduino_GFX and the profile's fields\n");
  return 0;
}
