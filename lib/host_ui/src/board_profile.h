// What the UI needs to know about a board's screen. Each board sketch defines
// one BoardProfile plus the three driver hooks below; host_ui does the rest.
#pragma once

#include <stdint.h>

enum class ScreenShape : uint8_t { Rect, Rounded, Circle };

// How the board is driven. Touch boards tap; the others move a focus ring
// through each screen in reading order and press to open (docs/ui-redesign.md,
// "Input without touch").
enum class InputKind : uint8_t { Touch, Encoder, Keys };

// One key of a 5-way switch or a button board, as LVGL's keypad reads it.
enum class UiKey : uint8_t { None, Next, Prev, Left, Right, Enter };

struct BoardProfile {
  const char *name;
  uint16_t width, height;
  ScreenShape shape;
  uint16_t corner_radius;  // Rounded only, in pixels
  // Extra font steps for big screens: 0 on a 1.69" panel, about 2 on 4"+.
  uint8_t font_boost;
  InputKind input = InputKind::Touch;
  int8_t input_pins[3] = {-1, -1, -1};  // the board driver's: encoder A, B, press; or its buttons
  // Encoder boards: the turns since the last call, and whether it's pressed.
  int16_t (*read_encoder)(bool *pressed) = nullptr;
  // Key boards: the key held down now, or UiKey::None.
  UiKey (*read_key)() = nullptr;
};

// Implemented by the board sketch.
bool board_display_begin();
void board_display_flush(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);
// True while a finger is down; x/y in screen pixels. Touch boards only.
bool board_touch_read(int16_t *x, int16_t *y);

#if UI_TOUCH_PROBE
// Diagnostic builds: the last touch as the panel reported it, unclamped.
extern int16_t board_touch_raw[2];
#endif
