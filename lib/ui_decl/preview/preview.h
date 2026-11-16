// Off-board rendering of a ui_decl screen: the same steps on the Mac
// (tests/ui_decl, `make ui-shots`) and in the browser (lib/ui_decl/web), so the
// two can be compared pixel for pixel.
//
// LVGL draws into an RGB565 framebuffer, like the board's ST7796, with a fake
// clock ticked well past the theme's transitions; the framebuffer is then
// widened to 8-bit RGB the same way ui_mocks does.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace ui_preview {

struct Frame {
  int w = 0, h = 0;
  std::vector<uint16_t> rgb565;  // w * h, row by row
  std::vector<uint8_t> rgb888;   // w * h * 3
  int layout_problems = 0;       // labels cut off by a parent or the screen
  std::string report;            // one line per problem
};

// Starts LVGL (once per process), renders the screen and fills out. Returns
// false with err set if the description doesn't build.
bool render(int w, int h, const char *theme, const char *screen, const char *data, Frame &out, std::string &err);

}  // namespace ui_preview
