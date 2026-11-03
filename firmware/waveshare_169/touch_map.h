// A touch chip reports the panel's native (portrait) coordinates; the screen
// may be turned (the profile's display.rotation, numbered as Arduino_GFX's
// setRotation: 1 and 3 are the quarter turns). Pure, so it's tested on the Mac
// (tests/test_touch_map.cpp).
#pragma once

#include <stdint.h>

namespace touch_map {

// raw: the chip's x, y; native_w x native_h: the panel unturned. The axes
// follow the rotation unless the profile says otherwise (swap_xy, invert_x,
// invert_y: -1 unset, 0, 1), for touch glass mounted turned against its panel.
// Swap first, then invert, in the screen's axes. Clamps, so a press reported
// past the edge (the glass's rim) is still a press on whatever sits there.
inline void to_screen(int rx, int ry, int rotation, int native_w, int native_h, int16_t *x, int16_t *y,
                      int8_t swap_xy = -1, int8_t invert_x = -1, int8_t invert_y = -1) {
  // 1: the panel turned a quarter turn anticlockwise (its top-right at the
  // screen's top-left); 3: clockwise. Checked on Board-3, the Waveshare 3.5".
  static const bool kSwap[4] = {false, true, false, true}, kInvX[4] = {false, false, true, true},
                    kInvY[4] = {false, true, true, false};
  const int r = rotation & 3;
  const bool swap = swap_xy >= 0 ? swap_xy : kSwap[r];
  const bool inv_x = invert_x >= 0 ? invert_x : kInvX[r];
  const bool inv_y = invert_y >= 0 ? invert_y : kInvY[r];
  int sx = swap ? ry : rx, sy = swap ? rx : ry;
  const int w = swap ? native_h : native_w, h = swap ? native_w : native_h;
  if (inv_x) sx = w - 1 - sx;
  if (inv_y) sy = h - 1 - sy;
  *x = int16_t(sx < 0 ? 0 : sx >= w ? w - 1 : sx);
  *y = int16_t(sy < 0 ? 0 : sy >= h ? h - 1 : sy);
}

}  // namespace touch_map
