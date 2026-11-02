// A touch chip reports the panel's native (portrait) coordinates; the screen
// may be turned (the profile's display.rotation, numbered as Arduino_GFX's
// setRotation: 1 and 3 are the quarter turns). Pure, so it's tested on the Mac
// (tests/test_touch_map.cpp).
#pragma once

#include <stdint.h>

namespace touch_map {

// raw: the chip's x, y; native_w x native_h: the panel unturned. Clamps, so a
// press reported past the edge (the glass's rim) is still a press on whatever
// sits there.
inline void to_screen(int rx, int ry, int rotation, int native_w, int native_h, int16_t *x, int16_t *y) {
  int sx, sy, w = native_w, h = native_h;
  switch (rotation & 3) {
    // 1: the panel turned a quarter turn anticlockwise (its top-right at the
    // screen's top-left); 3: clockwise. Checked on Board-3, the Waveshare 3.5".
    case 1: sx = ry; sy = native_w - 1 - rx; w = native_h; h = native_w; break;
    case 2: sx = native_w - 1 - rx; sy = native_h - 1 - ry; break;
    case 3: sx = native_h - 1 - ry; sy = rx; w = native_h; h = native_w; break;
    default: sx = rx; sy = ry; break;
  }
  *x = int16_t(sx < 0 ? 0 : sx >= w ? w - 1 : sx);
  *y = int16_t(sy < 0 ? 0 : sy >= h ? h - 1 : sy);
}

}  // namespace touch_map
