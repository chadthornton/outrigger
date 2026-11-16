#include "preview.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "../src/ui_decl.h"

namespace ui_preview {
namespace {

constexpr int kMaxW = 800, kMaxH = 800;
uint32_t now_ms = 1000;
uint32_t tick() { return now_ms; }
uint16_t fb[kMaxW * kMaxH];
int fb_w;

void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  const uint16_t *p = reinterpret_cast<const uint16_t *>(px);
  const int w = lv_area_get_width(a);
  for (int y = a->y1; y <= a->y2; ++y) memcpy(&fb[y * fb_w + a->x1], p + (y - a->y1) * w, size_t(w) * 2);
  lv_display_flush_ready(d);
}

lv_display_t *display;

// A label is cut off if any ancestor (or the screen) doesn't contain it, or
// if it's narrower than its text on one line and doesn't wrap.
void check(lv_obj_t *o, int w, int h, Frame &out) {
  const uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t *c = lv_obj_get_child(o, int32_t(i));
    if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_check_type(c, &lv_label_class)) {
      lv_area_t a;
      lv_obj_get_coords(c, &a);
      const char *why = nullptr;
      if (a.x1 < 0 || a.y1 < 0 || a.x2 >= w || a.y2 >= h) why = "off the screen";
      for (lv_obj_t *p = lv_obj_get_parent(c); p && !why; p = lv_obj_get_parent(p)) {
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        if (a.x1 < pa.x1 || a.y1 < pa.y1 || a.x2 > pa.x2 || a.y2 > pa.y2) why = "outside its parent";
      }
      if (!why && lv_label_get_long_mode(c) != LV_LABEL_LONG_MODE_WRAP) {
        lv_point_t sz;
        const lv_font_t *f = lv_obj_get_style_text_font(c, LV_PART_MAIN);
        lv_text_get_size(&sz, lv_label_get_text(c), f, lv_obj_get_style_text_letter_space(c, LV_PART_MAIN), 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x > lv_obj_get_content_width(c)) why = "narrower than its text";
      }
      if (why) {
        char b[160];
        snprintf(b, sizeof b, "label \"%s\" at %d,%d %dx%d: %s\n", lv_label_get_text(c), int(a.x1), int(a.y1),
                 int(lv_area_get_width(&a)), int(lv_area_get_height(&a)), why);
        out.report += b;
        ++out.layout_problems;
      }
    }
    check(c, w, h, out);
  }
}

}  // namespace

bool render(int w, int h, const char *theme, const char *screen, const char *data, Frame &out, std::string &err) {
  if (w > kMaxW || h > kMaxH) {
    err = "screen too big for the preview";
    return false;
  }
  if (!lv_is_initialized()) {
    lv_init();
    lv_tick_set_cb(tick);
  }
  if (display) lv_display_delete(display);
  fb_w = w;
  display = lv_display_create(w, h);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  static uint16_t buf[kMaxW * kMaxH / 4];
  lv_display_set_buffers(display, buf, nullptr, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush);
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_screen_load(scr);
  lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

  const ui_decl::Result r = ui_decl::build(scr, theme, screen, data);
  if (!r.ok) {
    err = r.error;
    return false;
  }
  // Well past LV_THEME_DEFAULT_TRANSITION_TIME (80 ms) and any layout passes.
  for (int t = 0; t < 400; t += 5) {
    now_ms += 5;
    lv_timer_handler();
  }
  lv_obj_invalidate(scr);
  lv_refr_now(display);

  out.w = w;
  out.h = h;
  out.rgb565.assign(fb, fb + size_t(w) * h);
  out.rgb888.resize(size_t(w) * h * 3);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    const uint16_t p = fb[i];
    out.rgb888[i * 3 + 0] = uint8_t(((p >> 11) & 31) * 255 / 31);
    out.rgb888[i * 3 + 1] = uint8_t(((p >> 5) & 63) * 255 / 63);
    out.rgb888[i * 3 + 2] = uint8_t((p & 31) * 255 / 31);
  }
  out.layout_problems = 0;
  out.report.clear();
  check(scr, w, h, out);
  return true;
}

}  // namespace ui_preview
