// What the UI needs to know about a board's screen. Each board sketch defines
// one BoardProfile plus the three driver hooks below; host_ui does the rest.
#pragma once

#include <stdint.h>

enum class ScreenShape : uint8_t { Rect, Rounded, Circle };

struct BoardProfile {
  const char *name;
  uint16_t width, height;
  ScreenShape shape;
  uint16_t corner_radius;  // Rounded only, in pixels
  // Extra font steps for big screens: 0 on a 1.69" panel, about 2 on 4"+.
  uint8_t font_boost;
};

// Implemented by the board sketch.
bool board_display_begin();
void board_display_flush(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);
// True while a finger is down; x/y in screen pixels.
bool board_touch_read(int16_t *x, int16_t *y);
