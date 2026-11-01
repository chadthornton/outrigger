// Host-side test for firmware/waveshare_169/touch_map.h: a touch chip reports
// the panel's native portrait coordinates; the UI wants them in the screen's
// orientation (the profile's display.rotation, Arduino_GFX's numbering).
#include <cstdio>
#include <cstdlib>

#include "touch_map.h"

static int failures = 0;
static void expect(int rot, int rx, int ry, int wx, int wy) {
  int16_t x, y;
  touch_map::to_screen(rx, ry, rot, 320, 480, &x, &y);
  if (x != wx || y != wy) {
    printf("rotation %d: raw %d,%d -> %d,%d, wanted %d,%d\n", rot, rx, ry, x, y, wx, wy);
    ++failures;
  }
}

int main() {
  // Portrait (0): as reported. The 1.69 and the 3.5" portrait probe: corners land in their corners.
  expect(0, 37, 41, 37, 41);
  expect(0, 289, 450, 289, 450);
  // Rotation 1 (Arduino_GFX's ST7796: MADCTL MX|MV) is the panel turned a quarter
  // turn anticlockwise: its top-right corner shows at the screen's top-left, its
  // top-left at the bottom-left. Board-3 showed it: with the opposite turn, taps
  // on the top nav landed at the bottom, diagonally across (2026-11-01).
  expect(1, 319, 0, 0, 0);
  expect(1, 0, 0, 0, 319);
  expect(1, 0, 479, 479, 319);
  expect(1, 319, 479, 479, 0);
  // Rotation 2: upside down.
  expect(2, 0, 0, 319, 479);
  // Rotation 3: the other quarter turn.
  expect(3, 0, 0, 479, 0);
  expect(3, 0, 479, 0, 0);
  // Past the edge clamps onto the screen (the glass's rim is still a press).
  expect(0, 400, 600, 319, 479);
  expect(1, 400, -5, 0, 0);
  if (failures) return 1;
  printf("touch_map: every rotation maps the panel's corners to the screen's\n");
  return 0;
}
