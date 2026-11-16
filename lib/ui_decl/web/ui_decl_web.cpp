// The browser build of the ui_decl preview: LVGL and ui_decl compiled to
// WebAssembly with Emscripten (`make ui-decl-web`). It renders through the
// same lib/ui_decl/preview code as `make ui-shots`, so the pixels match.
//
// JavaScript calls udw_render() with the three JSON texts, then reads the
// frame with udw_rgba() (for a canvas) or writes a PNG into Emscripten's
// file system with udw_write_png() (for comparing with the Mac's).
#include <emscripten/emscripten.h>
#include <stdint.h>

#include <string>

#include "../../../tests/ui_shots/png.h"
#include "../preview/preview.h"

static ui_preview::Frame frame;
static std::string error;
static std::vector<uint8_t> rgba;

extern "C" {

EMSCRIPTEN_KEEPALIVE int udw_render(int w, int h, const char *theme, const char *screen, const char *data) {
  error.clear();
  if (!ui_preview::render(w, h, theme, screen, data && *data ? data : nullptr, frame, error)) return 0;
  rgba.resize(size_t(w) * h * 4);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    rgba[i * 4 + 0] = frame.rgb888[i * 3 + 0];
    rgba[i * 4 + 1] = frame.rgb888[i * 3 + 1];
    rgba[i * 4 + 2] = frame.rgb888[i * 3 + 2];
    rgba[i * 4 + 3] = 255;
  }
  return 1;
}

EMSCRIPTEN_KEEPALIVE const char *udw_error() { return error.c_str(); }
EMSCRIPTEN_KEEPALIVE const char *udw_report() { return frame.report.c_str(); }
EMSCRIPTEN_KEEPALIVE int udw_problems() { return frame.layout_problems; }
EMSCRIPTEN_KEEPALIVE const uint8_t *udw_rgba() { return rgba.data(); }
EMSCRIPTEN_KEEPALIVE const uint16_t *udw_rgb565() { return frame.rgb565.data(); }

EMSCRIPTEN_KEEPALIVE int udw_write_png(const char *path) {
  return png::write(path, frame.w, frame.h, frame.rgb888.data()) ? 1 : 0;
}
}
