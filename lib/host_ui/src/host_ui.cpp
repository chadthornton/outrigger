#include "host_ui.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <string.h>

using midi_core::Level;

// ---------- Fonts: one table per typeface, indexed by size step ----------
// Steps 0-6 run small to large; text size S/M/L picks a starting step and the
// board's font_boost moves it up for big screens.
static const lv_font_t *const kSansSteps[] = {
    &lv_font_montserrat_10, &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
    &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_22, &lv_font_montserrat_24,
    &lv_font_montserrat_28};
// Unscii comes in two sizes only, so Pixel's medium and large look the same.
static const lv_font_t *const kPixelSteps[] = {&lv_font_unscii_8, &lv_font_unscii_16};

static const lv_font_t *font_for(uint8_t typeface, int step) {
  if (typeface == 1) return kPixelSteps[step < 3 ? 0 : 1];
  const int n = sizeof kSansSteps / sizeof kSansSteps[0];
  return kSansSteps[step < 0 ? 0 : step >= n ? n - 1 : step];
}

// ---------- Colours ----------
static lv_color_t c(uint32_t hex) { return lv_color_hex(hex); }
static const uint32_t kWhite = 0xF2F2F2, kGrey = 0x8A8A8A, kDim = 0x333333, kGreen = 0x22DD44,
                      kSpine = 0x1D6B2C, kYellow = 0xFFE14A, kOrange = 0xFFA53A, kRed = 0xFF4A4A,
                      kMagenta = 0xFF6AF0, kProblemBg = 0x2A1508, kProblemEdge = 0x6A3A10;

static uint32_t level_hex(Level l) {
  switch (l) {
    case Level::Ok: return kGreen;
    case Level::Warn: return kOrange;
    case Level::Bad: return kRed;
    default: return kDim;
  }
}

// ---------- State ----------
static const BoardProfile *board;
static UiSettings settings;
static UiSettingsChanged on_settings_changed;

static lv_style_t plain, title_style, detail_style;

struct Node {
  lv_obj_t *row, *dot, *line, *title, *detail, *extra;
};
constexpr int kNodes = 5;  // Power, Hub, Controllers, Router, FM-1
static Node nodes[kNodes];
static lv_obj_t *controller_rows[midi_core::kMaxDevices];
static lv_obj_t *controller_dots[midi_core::kMaxDevices];
static lv_obj_t *controller_labels[midi_core::kMaxDevices];
static lv_obj_t *usb_label, *problem_box, *problem_1, *problem_2;
static lv_obj_t *chain_screen, *settings_screen, *preview_label;
static lv_obj_t *size_buttons, *face_buttons, *wifi_buttons, *net_label, *net_label2;
static lv_obj_t *banner, *banner_1, *banner_2;

// ---------- LVGL glue ----------
static uint32_t tick_ms() { return millis(); }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  board_display_flush(area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area),
                      reinterpret_cast<const uint16_t *>(px));
  lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *data) {
  int16_t x, y;
  if (board_touch_read(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ---------- Layout helpers ----------
// Insets that keep content inside the glass: rounded corners eat the edges,
// a circle needs a much bigger margin.
static void safe_padding(int *side, int *top) {
  switch (board->shape) {
    case ScreenShape::Rounded: *side = board->corner_radius * 3 / 10 + 4; *top = board->corner_radius / 3 + 4; break;
    case ScreenShape::Circle: *side = board->width * 15 / 100; *top = board->height * 8 / 100; break;
    default: *side = 6; *top = 6; break;
  }
}

static lv_obj_t *box(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_add_style(o, &plain, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);  // taps fall through to the screen
  return o;
}

static lv_obj_t *label(lv_obj_t *parent, lv_style_t *style, uint32_t colour) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_add_style(l, style, 0);
  lv_obj_set_style_text_color(l, c(colour), 0);
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_text(l, "");
  return l;
}

// Only touch LVGL when the text really changed, so idle frames cost nothing.
static void set_text(lv_obj_t *l, const char *s) {
  if (strcmp(lv_label_get_text(l), s) != 0) lv_label_set_text(l, s);
}
static void set_colour(lv_obj_t *o, uint32_t hex, bool text = true) {
  if (text) lv_obj_set_style_text_color(o, c(hex), 0);
  else lv_obj_set_style_bg_color(o, c(hex), 0);
}

static void apply_fonts() {
  const int base = settings.text_size * 2 + board->font_boost;  // S=0, M=2, L=4
  lv_style_set_text_font(&title_style, font_for(settings.typeface, base + 3));
  lv_style_set_text_font(&detail_style, font_for(settings.typeface, base));
  lv_obj_report_style_change(&title_style);
  lv_obj_report_style_change(&detail_style);
}

// ---------- Chain screen ----------
static void open_settings(lv_event_t *) { lv_screen_load(settings_screen); }

static void build_chain_screen() {
  chain_screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(chain_screen, c(0x000000), 0);
  lv_obj_remove_flag(chain_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(chain_screen, open_settings, LV_EVENT_CLICKED, nullptr);

  int side, top;
  safe_padding(&side, &top);
  lv_obj_t *column = box(chain_screen);
  lv_obj_set_size(column, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_hor(column, side, 0);
  lv_obj_set_style_pad_ver(column, top, 0);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);

  static const char *const kTitles[kNodes] = {"Power", "Hub", "Controller", "Router", "FM-1"};
  static const uint32_t kTitleColours[kNodes] = {kWhite, kWhite, kYellow, kWhite, kMagenta};
  for (int n = 0; n < kNodes; ++n) {
    Node &node = nodes[n];
    // Each node grows to share the screen's height, so the chain fills it.
    node.row = box(column);
    lv_obj_set_width(node.row, lv_pct(100));
    lv_obj_set_height(node.row, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(node.row, 1);
    lv_obj_set_flex_flow(node.row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(node.row, 8, 0);

    // Rail: the dot, then the spine segment down to the next node.
    lv_obj_t *rail = box(node.row);
    lv_obj_set_width(rail, 12);
    lv_obj_set_height(rail, lv_pct(100));
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(rail, 4, 0);
    node.dot = box(rail);
    lv_obj_set_size(node.dot, 10, 10);
    lv_obj_set_style_radius(node.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(node.dot, LV_OPA_COVER, 0);
    node.line = box(rail);
    lv_obj_set_width(node.line, 2);
    lv_obj_set_flex_grow(node.line, 1);
    lv_obj_set_style_bg_opa(node.line, LV_OPA_COVER, 0);
    if (n == kNodes - 1) lv_obj_add_flag(node.line, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *content = box(node.row);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_height(content, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(content, 4, 0);
    node.title = label(content, &title_style, kTitleColours[n]);
    lv_label_set_text(node.title, kTitles[n]);
    if (n == 2) {
      // Controller group: one row per device, created up front and shown as needed.
      for (int i = 0; i < (int)midi_core::kMaxDevices; ++i) {
        controller_rows[i] = box(content);
        lv_obj_set_width(controller_rows[i], lv_pct(100));
        lv_obj_set_height(controller_rows[i], LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(controller_rows[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(controller_rows[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(controller_rows[i], 6, 0);
        controller_dots[i] = box(controller_rows[i]);
        lv_obj_set_size(controller_dots[i], 6, 6);
        lv_obj_set_style_radius(controller_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(controller_dots[i], LV_OPA_COVER, 0);
        controller_labels[i] = label(controller_rows[i], &detail_style, kGrey);
        lv_obj_set_flex_grow(controller_labels[i], 1);
        lv_obj_add_flag(controller_rows[i], LV_OBJ_FLAG_HIDDEN);
      }
      node.detail = label(content, &detail_style, kGrey);  // "none" when empty
    } else {
      node.detail = label(content, &detail_style, kGrey);
    }
    node.extra = label(content, &detail_style, kGrey);
    lv_obj_add_flag(node.extra, LV_OBJ_FLAG_HIDDEN);
  }

  problem_box = box(column);
  lv_obj_set_width(problem_box, lv_pct(100));
  lv_obj_set_height(problem_box, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(problem_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(problem_box, c(kProblemBg), 0);
  lv_obj_set_style_bg_opa(problem_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(problem_box, c(kProblemEdge), 0);
  lv_obj_set_style_border_width(problem_box, 1, 0);
  lv_obj_set_style_radius(problem_box, 6, 0);
  lv_obj_set_style_pad_all(problem_box, 6, 0);
  problem_1 = label(problem_box, &detail_style, kOrange);
  problem_2 = label(problem_box, &detail_style, kWhite);
  lv_label_set_long_mode(problem_1, LV_LABEL_LONG_MODE_WRAP);
  lv_label_set_long_mode(problem_2, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_add_flag(problem_box, LV_OBJ_FLAG_HIDDEN);

  usb_label = lv_label_create(chain_screen);
  lv_obj_add_style(usb_label, &detail_style, 0);
  lv_obj_add_flag(usb_label, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(usb_label, LV_ALIGN_TOP_RIGHT, -side, top);
  lv_label_set_text(usb_label, "");
}

// ---------- Settings screen ----------
static void back_to_chain(lv_event_t *) { lv_screen_load(chain_screen); }

static void update_preview() {
  lv_obj_set_style_text_font(preview_label, font_for(settings.typeface, settings.text_size * 2 + board->font_boost + 3), 0);
}

static void settings_changed(lv_event_t *e) {
  lv_obj_t *bm = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const uint32_t picked = lv_buttonmatrix_get_selected_button(bm);
  if (picked == LV_BUTTONMATRIX_BUTTON_NONE) return;
  if (bm == size_buttons) settings.text_size = static_cast<uint8_t>(picked);
  else if (bm == face_buttons) settings.typeface = static_cast<uint8_t>(picked);
  else settings.wifi = static_cast<uint8_t>(picked);
  apply_fonts();
  update_preview();
  if (on_settings_changed) on_settings_changed(settings);
}

static lv_obj_t *choice(lv_obj_t *parent, const char *const *map, uint32_t selected) {
  lv_obj_t *bm = lv_buttonmatrix_create(parent);
  lv_buttonmatrix_set_map(bm, map);
  lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
  lv_buttonmatrix_set_one_checked(bm, true);
  lv_buttonmatrix_set_button_ctrl(bm, selected, LV_BUTTONMATRIX_CTRL_CHECKED);
  lv_obj_set_width(bm, lv_pct(100));
  lv_obj_set_height(bm, 44);
  lv_obj_set_style_text_font(bm, &lv_font_montserrat_16, 0);
  lv_obj_add_event_cb(bm, settings_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  return bm;
}

static lv_obj_t *heading(lv_obj_t *parent, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(l, c(kGrey), 0);
  lv_label_set_text(l, text);
  return l;
}

static void build_settings_screen() {
  settings_screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(settings_screen, c(0x000000), 0);
  int side, top;
  safe_padding(&side, &top);
  lv_obj_set_style_pad_hor(settings_screen, side, 0);
  lv_obj_set_style_pad_ver(settings_screen, top, 0);
  lv_obj_set_flex_flow(settings_screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(settings_screen, 8, 0);

  lv_obj_t *t = lv_label_create(settings_screen);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(t, c(kWhite), 0);
  lv_label_set_text(t, "Settings");

  static const char *const kSizes[] = {"S", "M", "L", ""};
  static const char *const kFaces[] = {"Sans", "Pixel", ""};
  heading(settings_screen, "Text size");
  size_buttons = choice(settings_screen, kSizes, settings.text_size);
  heading(settings_screen, "Typeface");
  face_buttons = choice(settings_screen, kFaces, settings.typeface);

  static const char *const kWifi[] = {"Off", "On", ""};
  heading(settings_screen, "Wi-Fi (web page)");
  wifi_buttons = choice(settings_screen, kWifi, settings.wifi);
  net_label = lv_label_create(settings_screen);
  lv_obj_set_style_text_font(net_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(net_label, c(kWhite), 0);
  lv_label_set_text(net_label, "");
  net_label2 = lv_label_create(settings_screen);
  lv_obj_set_style_text_font(net_label2, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(net_label2, c(kGrey), 0);
  lv_label_set_text(net_label2, "");

  preview_label = lv_label_create(settings_screen);
  lv_obj_set_style_text_color(preview_label, c(kYellow), 0);
  lv_label_set_text(preview_label, "Launchkey > FM-1");
  update_preview();

  lv_obj_t *done = lv_button_create(settings_screen);
  lv_obj_set_width(done, lv_pct(100));
  lv_obj_t *dl = lv_label_create(done);
  lv_obj_set_style_text_font(dl, &lv_font_montserrat_16, 0);
  lv_label_set_text(dl, "Done");
  lv_obj_center(dl);
  lv_obj_add_event_cb(done, back_to_chain, LV_EVENT_CLICKED, nullptr);
}

// ---------- Banner (pairing PIN), above every screen ----------
static void build_banner() {
  banner = lv_obj_create(lv_layer_top());
  lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(banner, lv_pct(80), LV_SIZE_CONTENT);
  lv_obj_center(banner);
  lv_obj_set_flex_flow(banner, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(banner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_bg_color(banner, c(0x10203A), 0);
  lv_obj_set_style_border_color(banner, c(0x3A6AB0), 0);
  lv_obj_set_style_border_width(banner, 2, 0);
  lv_obj_set_style_radius(banner, 10, 0);
  lv_obj_set_style_pad_all(banner, 10, 0);
  banner_1 = lv_label_create(banner);
  lv_obj_set_style_text_font(banner_1, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(banner_1, c(kWhite), 0);
  banner_2 = lv_label_create(banner);
  lv_obj_set_style_text_font(banner_2, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(banner_2, c(kYellow), 0);
  lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

static void check_only(lv_obj_t *bm, uint32_t selected) {
  lv_buttonmatrix_clear_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKED);
  lv_buttonmatrix_set_button_ctrl(bm, selected, LV_BUTTONMATRIX_CTRL_CHECKED);
}

void ui_set_settings(const UiSettings &s) {
  settings = s;
  if (settings.text_size > 2) settings.text_size = 1;
  if (settings.typeface > 1) settings.typeface = 0;
  if (settings.wifi > 1) settings.wifi = 1;
  check_only(size_buttons, settings.text_size);
  check_only(face_buttons, settings.typeface);
  check_only(wifi_buttons, settings.wifi);
  apply_fonts();
  update_preview();
}

// ---------- Public API ----------
bool ui_begin(const BoardProfile &b, const UiSettings &s, UiSettingsChanged on_change) {
  board = &b;
  settings = s;
  if (settings.text_size > 2) settings.text_size = 1;
  if (settings.typeface > 1) settings.typeface = 0;
  if (settings.wifi > 1) settings.wifi = 1;
  on_settings_changed = on_change;

  if (!board_display_begin()) return false;
  lv_init();
  lv_tick_set_cb(tick_ms);

  lv_display_t *disp = lv_display_create(b.width, b.height);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  // Two partial buffers of 1/8 screen each, in DMA-capable internal RAM.
  const uint32_t bytes = b.width * (b.height / 8) * 2;
  void *buf1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void *buf2 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!buf1 || !buf2) return false;
  lv_display_set_buffers(disp, buf1, buf2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);

  lv_indev_t *touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, touch_cb);

  lv_style_init(&plain);
  lv_style_set_bg_opa(&plain, LV_OPA_TRANSP);
  lv_style_set_border_width(&plain, 0);
  lv_style_set_pad_all(&plain, 0);
  lv_style_set_radius(&plain, 0);
  lv_style_set_pad_row(&plain, 0);
  lv_style_set_pad_column(&plain, 0);
  lv_style_init(&title_style);
  lv_style_init(&detail_style);
  apply_fonts();

  build_chain_screen();
  build_settings_screen();
  build_banner();
  lv_screen_load(chain_screen);
  return true;
}

void ui_tick() { lv_timer_handler(); }

void ui_update(const UiFrame &f) {
  const midi_core::ChainView &v = f.view;
  const Level levels[kNodes] = {v.power, v.hub, v.controllers, v.router, v.target};
  for (int n = 0; n < kNodes; ++n) {
    set_colour(nodes[n].dot, level_hex(levels[n]), false);
    set_colour(nodes[n].line, n + 1 < v.first_broken ? kSpine : kDim, false);
    set_colour(nodes[n].detail, levels[n] == Level::Ok ? kGrey : level_hex(levels[n]));
  }

  set_text(nodes[0].detail, "USB-C 5V");
  set_text(nodes[1].detail, f.hub_detail);

  char buf[64];
  snprintf(buf, sizeof buf, f.controller_count > 1 ? "Controllers (%u)" : "Controller", f.controller_count);
  set_text(nodes[2].title, buf);
  for (int i = 0; i < (int)midi_core::kMaxDevices; ++i) {
    if (i >= f.controller_count) {
      lv_obj_add_flag(controller_rows[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const UiDeviceLine &d = f.controllers[i];
    lv_obj_remove_flag(controller_rows[i], LV_OBJ_FLAG_HIDDEN);
    set_colour(controller_dots[i], d.open ? kGreen : kRed, false);
    snprintf(buf, sizeof buf, "%s  %s", d.tag, d.detail);
    set_text(controller_labels[i], buf);
    set_colour(controller_labels[i], !d.open ? kRed : d.fresh ? kGreen : kGrey);
  }
  if (f.controller_count == 0) {
    lv_obj_remove_flag(nodes[2].detail, LV_OBJ_FLAG_HIDDEN);
    set_text(nodes[2].detail, "none");
  } else {
    lv_obj_add_flag(nodes[2].detail, LV_OBJ_FLAG_HIDDEN);
  }

  snprintf(buf, sizeof buf, "%lu passed  %lu SysEx blocked", (unsigned long)f.sent, (unsigned long)f.blocked);
  set_text(nodes[3].detail, buf);

  set_text(nodes[4].detail, f.fm1_in);
  if (v.target == Level::Ok) set_colour(nodes[4].detail, f.fm1_in_fresh ? kGreen : kGrey);
  if (f.fm1_own[0] && !v.has_problem) {
    lv_obj_remove_flag(nodes[4].extra, LV_OBJ_FLAG_HIDDEN);
    set_text(nodes[4].extra, f.fm1_own);
  } else {
    lv_obj_add_flag(nodes[4].extra, LV_OBJ_FLAG_HIDDEN);
  }

  snprintf(buf, sizeof buf, "USB %u/8", f.snapshot.channels_used);
  set_text(usb_label, buf);
  set_colour(usb_label, f.snapshot.channels_used >= 8 ? kYellow : kGrey);

  set_text(net_label, f.net_line);
  set_text(net_label2, f.net_line2);
  if (f.banner1[0]) {
    set_text(banner_1, f.banner1);
    set_text(banner_2, f.banner2);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
  }

  if (v.has_problem) {
    lv_obj_remove_flag(problem_box, LV_OBJ_FLAG_HIDDEN);
    set_text(problem_1, v.problem.line1);
    set_text(problem_2, v.problem.line2);
  } else {
    lv_obj_add_flag(problem_box, LV_OBJ_FLAG_HIDDEN);
  }
}
