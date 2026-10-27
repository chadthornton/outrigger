#include "host_ui.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <string.h>

using midi_core::Level;

// Build with -DUI_TOUCH_PROBE=1 to draw a dot where the touch lands.
#ifndef UI_TOUCH_PROBE
#define UI_TOUCH_PROBE 0
#endif

// ---------- Fonts: Montserrat, indexed by size step ----------
// Steps 0-8 run small to large; text size S/M/L picks a starting step and the
// board's font_boost moves it up for big screens.
static const lv_font_t *const kSansSteps[] = {
    &lv_font_montserrat_10, &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
    &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_22, &lv_font_montserrat_24,
    &lv_font_montserrat_28};

static const lv_font_t *font_for(int step) {
  const int n = sizeof kSansSteps / sizeof kSansSteps[0];
  return kSansSteps[step < 0 ? 0 : step >= n ? n - 1 : step];
}

// ---------- Colours ----------
static lv_color_t c(uint32_t hex) { return lv_color_hex(hex); }
// kGrey is the detail text: near white, so it reads on the panel at a glance.
static const uint32_t kWhite = 0xF2F2F2, kGrey = 0xCFCFCF, kDim = 0x333333, kGreen = 0x22DD44,
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
static UiHooks hooks;
static const char *firmware = "";

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
static lv_obj_t *chain_screen, *chain_column, *settings_screen;
static lv_obj_t *size_buttons[3];
static lv_obj_t *computer_button, *computer_label;
// Steps the chain gave up to fit the screen (see fit_chain), and what it was
// fitted for, so it refits only when the screen's shape changes.
static int chain_shrink;
static uint32_t fitted_shape = UINT32_MAX;
static lv_obj_t *firmware_screen, *firmware_title, *firmware_lines[kLoaderLines], *hold_button, *hold_label,
    *hold_bar;
static uint32_t hold_started;  // millis() when the hold began, 0 when not held
static bool hold_fired, hold_enabled;
static lv_timer_t *computer_disarm;
#if UI_TOUCH_PROBE
static lv_obj_t *probe_label;
#endif

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
  // One line, cut with "...": a content-height label would wrap instead.
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_max_lines(l, 1);
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

static int base_step() { return settings.text_size * 2 + board->font_boost; }  // S=0, M=2, L=4

// shrink 0 is the chosen size. When the chain doesn't fit, it gives way in
// this order: the constant Power line goes (1), titles come down to one step
// above the details (2, 3), then everything gets smaller (4 on).
constexpr int kMaxShrink = 3 + 4;
static void apply_fonts(int shrink) {
  const int gap = shrink >= 3 ? 1 : shrink == 2 ? 2 : 3;
  const int detail = base_step() - (shrink > 3 ? shrink - 3 : 0);
  lv_style_set_text_font(&title_style, font_for(detail + gap));
  lv_style_set_text_font(&detail_style, font_for(detail));
  lv_obj_report_style_change(&title_style);
  lv_obj_report_style_change(&detail_style);
  if (nodes[0].detail) {
    if (shrink >= 1) lv_obj_add_flag(nodes[0].detail, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(nodes[0].detail, LV_OBJ_FLAG_HIDDEN);
  }
}

// Rows never get less than their content (min_height), so a chain taller
// than the screen runs off the bottom; shrink until nothing does.
static bool chain_overflows() {
  // Two passes: the column reads each row's minimum from the rows' children,
  // which only take their new heights in the first pass.
  lv_obj_update_layout(chain_screen);
  lv_obj_mark_layout_as_dirty(chain_column);
  lv_obj_update_layout(chain_screen);
  return lv_obj_get_scroll_bottom(chain_column) > 0;
}

static void fit_chain() {
  for (chain_shrink = 0; chain_shrink < kMaxShrink; ++chain_shrink) {
    apply_fonts(chain_shrink);
    if (!chain_overflows()) return;
  }
  apply_fonts(chain_shrink);
}

// ---------- Chain screen ----------
static void open_settings(lv_event_t *) { lv_screen_load(settings_screen); }
static void open_firmware(lv_event_t *);

static void build_chain_screen() {
  chain_screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(chain_screen, c(0x000000), 0);
  lv_obj_remove_flag(chain_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(chain_screen, open_settings, LV_EVENT_CLICKED, nullptr);

  int side, top;
  safe_padding(&side, &top);
  lv_obj_t *column = chain_column = box(chain_screen);
  lv_obj_set_size(column, lv_pct(100), lv_pct(100));
  lv_obj_set_style_pad_hor(column, side, 0);
  lv_obj_set_style_pad_ver(column, top, 0);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);

  static const char *const kTitles[kNodes] = {"Power", "Hub", "Controller", "Router", "FM-1"};
  static const uint32_t kTitleColours[kNodes] = {kWhite, kWhite, kYellow, kWhite, kMagenta};
  for (int n = 0; n < kNodes; ++n) {
    Node &node = nodes[n];
    // Each node grows to share the screen's spare height, so the chain fills
    // it, but never gets less than its own lines (flex grow alone would give
    // every node an equal share and clip the tall ones).
    node.row = box(column);
    lv_obj_set_width(node.row, lv_pct(100));
    lv_obj_set_height(node.row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(node.row, LV_SIZE_CONTENT, 0);
    lv_obj_set_flex_grow(node.row, 1);
    lv_obj_set_flex_flow(node.row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_left(node.row, 12 + 8, 0);

    // Rail: the dot, then the spine segment down to the next node. Floating,
    // so its full-row height doesn't count as content: otherwise each row's
    // minimum would ratchet up to whatever height it last grew to.
    lv_obj_t *rail = box(node.row);
    lv_obj_add_flag(rail, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_translate_x(rail, -(12 + 8), 0);
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
  lv_obj_t *const problem_lines[] = {problem_1, problem_2};
  for (lv_obj_t *l : problem_lines) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_max_lines(l, 0);
  }
  lv_obj_add_flag(problem_box, LV_OBJ_FLAG_HIDDEN);

  usb_label = lv_label_create(chain_screen);
  lv_obj_add_style(usb_label, &detail_style, 0);
  lv_obj_add_flag(usb_label, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(usb_label, LV_ALIGN_TOP_RIGHT, -side, top);
  lv_label_set_text(usb_label, "");
}

// ---------- Settings screen ----------
// Everything here must fit without scrolling: on a scrollable page a finger
// that drifts a few pixels starts a scroll and the tap is lost.
static constexpr int kButtonH = 44;
static void disarm_computer(lv_timer_t *);

static void back_to_chain(lv_event_t *) {
  if (computer_disarm) {  // leaving Settings cancels a half-done Computer tap
    lv_timer_delete(computer_disarm);
    disarm_computer(nullptr);
  }
  lv_screen_load(chain_screen);
}

static void show_size_choice() {
  for (int i = 0; i < 3; ++i) lv_obj_set_state(size_buttons[i], LV_STATE_CHECKED, i == settings.text_size);
}

static void size_tapped(lv_event_t *e) {
  const uint8_t picked = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
  if (picked == settings.text_size) return;
  settings.text_size = picked;
  show_size_choice();
  fitted_shape = UINT32_MAX;  // refit the chain at the new size
  fit_chain();
  if (hooks.settings_changed) hooks.settings_changed(settings);
}

// Pressed buttons go lighter, so a tap shows the moment the finger lands.
static void style_pressed(lv_obj_t *b) {
  lv_obj_set_style_bg_color(b, c(0x5A5A5A), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(b, c(0x2F8FE8), LV_STATE_CHECKED | LV_STATE_PRESSED);
}

// Restarting stops MIDI, so the first tap only arms the button; a second tap
// within a few seconds restarts.
static const char kComputerIdle[] = LV_SYMBOL_USB " Computer";
static const char kComputerArmed[] = "Tap again to restart";

static void disarm_computer(lv_timer_t *) {
  lv_label_set_text(computer_label, kComputerIdle);
  lv_obj_set_style_bg_color(computer_button, c(kDim), 0);
  computer_disarm = nullptr;
}

static void computer_tapped(lv_event_t *) {
  if (!computer_disarm) {
    lv_label_set_text(computer_label, kComputerArmed);
    lv_obj_set_style_bg_color(computer_button, c(0x8A5A10), 0);
    computer_disarm = lv_timer_create(disarm_computer, 3000, nullptr);
    lv_timer_set_repeat_count(computer_disarm, 1);
    return;
  }
  if (hooks.connect_computer) hooks.connect_computer();
}

// A full-width button, kButtonH tall.
static lv_obj_t *wide_button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click, lv_obj_t **label_out) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, lv_pct(100), kButtonH);
  lv_obj_set_ext_click_area(b, 3);
  style_pressed(b);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, nullptr);
  if (label_out) *label_out = l;
  return b;
}

static void build_settings_screen() {
  settings_screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(settings_screen, c(0x000000), 0);
  lv_obj_remove_flag(settings_screen, LV_OBJ_FLAG_SCROLLABLE);
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

  lv_obj_t *ver = lv_label_create(settings_screen);
  lv_obj_add_flag(ver, LV_OBJ_FLAG_FLOATING);
  // 10 px: "v 1234567 hub-loader" beside the title fills the width at 12.
  lv_obj_set_style_text_font(ver, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(ver, c(kGrey), 0);
  lv_label_set_text(ver, firmware);
  lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, 0, 7);

  // Text size: three buttons sharing the width, each lettered in the size it
  // picks, so the letters are the preview.
  lv_obj_t *sizes = box(settings_screen);
  lv_obj_set_size(sizes, lv_pct(100), kButtonH + 4);
  lv_obj_set_flex_flow(sizes, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(sizes, 6, 0);
  static const char *const kSizes[] = {"S", "M", "L"};
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *b = size_buttons[i] = lv_button_create(sizes);
    lv_obj_set_height(b, lv_pct(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_bg_color(b, c(kDim), 0);
    lv_obj_set_style_bg_color(b, c(0x2196F3), LV_STATE_CHECKED);
    style_pressed(b);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font_for(i * 2 + board->font_boost + 3), 0);
    lv_label_set_text(l, kSizes[i]);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, size_tapped, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
  }
  show_size_choice();

  lv_obj_t *spacer = box(settings_screen);
  lv_obj_set_width(spacer, lv_pct(100));
  lv_obj_set_flex_grow(spacer, 1);

  // Firmware (Loader builds), Computer, then Done at the very bottom, each the
  // full width.
  lv_obj_t *buttons = box(settings_screen);
  lv_obj_set_width(buttons, lv_pct(100));
  lv_obj_set_height(buttons, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(buttons, 8, 0);
  if (hooks.back_up_fm1) {
    lv_obj_t *fw = wide_button(buttons, LV_SYMBOL_DRIVE " Firmware", open_firmware, nullptr);
    lv_obj_set_style_bg_color(fw, c(kDim), 0);
  }
  computer_button = wide_button(buttons, kComputerIdle, computer_tapped, &computer_label);
  lv_obj_set_style_bg_color(computer_button, c(kDim), 0);
  wide_button(buttons, "Done", back_to_chain, nullptr);
}

// ---------- Firmware screen (the Loader) ----------
// Anything that acts on the FM-1 needs a 2-second hold, never a single tap
// (loader safety rule 7). The bar fills while the finger stays down.
static constexpr uint32_t kHoldMs = 2000;
static const char kHoldIdle[] = "Hold: back up FM-1";

static void hold_event(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    hold_started = hold_enabled ? millis() : 0;
    hold_fired = false;
  } else if (code == LV_EVENT_PRESSING && hold_started && !hold_fired) {
    const uint32_t held = millis() - hold_started;
    lv_bar_set_value(hold_bar, held >= kHoldMs ? 100 : int32_t(held * 100 / kHoldMs), LV_ANIM_OFF);
    if (held >= kHoldMs) {
      hold_fired = true;
      hold_started = 0;
      lv_label_set_text(hold_label, "Starting...");
      if (hooks.back_up_fm1) hooks.back_up_fm1();
    }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    hold_started = 0;
    if (!hold_fired) lv_bar_set_value(hold_bar, 0, LV_ANIM_OFF);
  }
}

static void close_firmware(lv_event_t *) { lv_screen_load(settings_screen); }

static void build_firmware_screen() {
  firmware_screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(firmware_screen, c(0x000000), 0);
  lv_obj_remove_flag(firmware_screen, LV_OBJ_FLAG_SCROLLABLE);  // drift would cancel the hold
  int side, top;
  safe_padding(&side, &top);
  lv_obj_set_style_pad_hor(firmware_screen, side, 0);
  lv_obj_set_style_pad_ver(firmware_screen, top, 0);
  lv_obj_set_flex_flow(firmware_screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(firmware_screen, 4, 0);

  firmware_title = lv_label_create(firmware_screen);
  lv_obj_set_width(firmware_title, lv_pct(100));
  lv_label_set_long_mode(firmware_title, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(firmware_title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(firmware_title, c(kWhite), 0);
  lv_label_set_text(firmware_title, "Firmware");
  for (int i = 0; i < kLoaderLines; ++i) {
    firmware_lines[i] = lv_label_create(firmware_screen);
    lv_obj_set_width(firmware_lines[i], lv_pct(100));
    lv_label_set_long_mode(firmware_lines[i], LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(firmware_lines[i], &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(firmware_lines[i], c(kGrey), 0);
    lv_label_set_text(firmware_lines[i], "");
  }

  lv_obj_t *spacer = box(firmware_screen);
  lv_obj_set_width(spacer, lv_pct(100));
  lv_obj_set_flex_grow(spacer, 1);

  hold_button = lv_button_create(firmware_screen);
  lv_obj_set_width(hold_button, lv_pct(100));
  lv_obj_set_height(hold_button, 44);
  lv_obj_set_style_bg_color(hold_button, c(kDim), 0);
  lv_obj_set_style_pad_all(hold_button, 0, 0);
  hold_bar = lv_bar_create(hold_button);
  lv_obj_set_size(hold_bar, lv_pct(100), lv_pct(100));
  lv_obj_set_style_radius(hold_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(hold_bar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_color(hold_bar, c(kYellow), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(hold_bar, LV_OPA_50, LV_PART_INDICATOR);
  lv_obj_remove_flag(hold_bar, LV_OBJ_FLAG_CLICKABLE);
  lv_bar_set_range(hold_bar, 0, 100);
  hold_label = lv_label_create(hold_button);
  lv_obj_set_style_text_font(hold_label, &lv_font_montserrat_16, 0);
  lv_label_set_text(hold_label, kHoldIdle);
  lv_obj_center(hold_label);
  lv_obj_add_event_cb(hold_button, hold_event, LV_EVENT_ALL, nullptr);

  lv_obj_t *row = box(firmware_screen);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  wide_button(row, "Done", close_firmware, nullptr);
}

static void open_firmware(lv_event_t *) { lv_screen_load(firmware_screen); }

void ui_update_loader(const UiLoader &l) {
  if (!firmware_screen) return;
  set_text(firmware_title, l.title);
  set_colour(firmware_title, l.title_colour);
  for (int i = 0; i < kLoaderLines; ++i) set_text(firmware_lines[i], l.lines[i]);
  hold_enabled = l.can_back_up && !l.busy;
  if (l.busy) {
    char buf[24];
    snprintf(buf, sizeof buf, "Working... %u%%", l.percent);
    set_text(hold_label, buf);
    lv_bar_set_value(hold_bar, l.percent, LV_ANIM_OFF);
  } else if (!hold_started) {
    set_text(hold_label, l.can_back_up ? kHoldIdle : "Not ready");
    lv_bar_set_value(hold_bar, 0, LV_ANIM_OFF);
  }
  set_colour(hold_label, hold_enabled || l.busy ? kWhite : kGrey);
}

// ---------- Computer-mode screen ----------
static void back_to_host_tapped(lv_event_t *) {
  if (hooks.back_to_host) hooks.back_to_host();
}

static lv_obj_t *build_computer_screen(const char *info) {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, c(0x000000), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  int side, top;
  safe_padding(&side, &top);
  lv_obj_set_style_pad_hor(scr, side, 0);
  lv_obj_set_style_pad_ver(scr, top, 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 10, 0);

  lv_obj_t *t = lv_label_create(scr);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(t, c(kYellow), 0);
  lv_label_set_text(t, LV_SYMBOL_USB " Computer mode");

  lv_obj_t *body = lv_label_create(scr);
  lv_obj_set_width(body, lv_pct(100));
  lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(body, c(kWhite), 0);
  lv_label_set_text(body,
                    "MIDI is off. Plug this board straight into a computer and open the installer in Chrome "
                    "or Edge.\n\nAfter an update it goes back to MIDI by itself.");

  lv_obj_t *ver = lv_label_create(scr);
  lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ver, c(kGrey), 0);
  lv_label_set_text(ver, info);

  lv_obj_t *spacer = box(scr);
  lv_obj_set_width(spacer, lv_pct(100));
  lv_obj_set_flex_grow(spacer, 1);

  wide_button(scr, "Back to MIDI", back_to_host_tapped, nullptr);
  return scr;
}

// ---------- Public API ----------
bool ui_begin(const BoardProfile &b, const UiSettings &s, const UiHooks &h, const char *fw) {
  board = &b;
  settings = s;
  if (settings.text_size > 2) settings.text_size = 1;
  hooks = h;
  firmware = fw;

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
  // Read the panel every 10 ms, not once a frame (33 ms): a quick tap lasts
  // under 100 ms, and every sample it misses makes the screen feel late.
  lv_timer_set_period(lv_indev_get_read_timer(touch), 10);
#if UI_TOUCH_PROBE
  // Diagnostic build: a dot where LVGL thinks the finger is.
  lv_obj_t *dot = lv_obj_create(lv_layer_sys());
  lv_obj_set_size(dot, 12, 12);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, c(kRed), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_indev_set_cursor(touch, dot);
  probe_label = lv_label_create(lv_layer_sys());
  lv_obj_set_style_text_font(probe_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(probe_label, c(kRed), 0);
  lv_obj_align(probe_label, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(probe_label, "touch: -");
#endif

  lv_style_init(&plain);
  lv_style_set_bg_opa(&plain, LV_OPA_TRANSP);
  lv_style_set_border_width(&plain, 0);
  lv_style_set_pad_all(&plain, 0);
  lv_style_set_radius(&plain, 0);
  lv_style_set_pad_row(&plain, 0);
  lv_style_set_pad_column(&plain, 0);
  lv_style_init(&title_style);
  lv_style_init(&detail_style);
  apply_fonts(0);

  build_chain_screen();
  if (hooks.back_up_fm1) build_firmware_screen();
  build_settings_screen();
  lv_screen_load(chain_screen);
  return true;
}

void ui_show_computer_mode(const char *info) { lv_screen_load(build_computer_screen(info)); }

void ui_tick() {
#if UI_TOUCH_PROBE
  // The panel's own numbers, before the board clamps them to the screen.
  static int16_t shown_x = -1, shown_y = -1;
  if (board_touch_raw[0] != shown_x || board_touch_raw[1] != shown_y) {
    shown_x = board_touch_raw[0];
    shown_y = board_touch_raw[1];
    lv_label_set_text_fmt(probe_label, "raw %d, %d", shown_x, shown_y);
  }
#endif
  lv_timer_handler();
}

void ui_update(const UiFrame &f) {
  const midi_core::ChainView &v = f.view;
  const Level levels[kNodes] = {v.power, v.hub, v.controllers, v.router, v.target};
  for (int n = 0; n < kNodes; ++n) {
    set_colour(nodes[n].dot, level_hex(levels[n]), false);
    set_colour(nodes[n].line, n + 1 < v.first_broken ? kSpine : kDim, false);
    // Off is a dim dot, but its text stays readable.
    const bool plain_text = levels[n] == Level::Ok || levels[n] == Level::Off;
    set_colour(nodes[n].detail, plain_text ? kGrey : level_hex(levels[n]));
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

  // The router blocks only SysEx, so "blocked" says it; the longer wording
  // ran past the width at M.
  snprintf(buf, sizeof buf, "%lu passed  %lu blocked", (unsigned long)f.sent, (unsigned long)f.blocked);
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

  if (v.has_problem) {
    lv_obj_remove_flag(problem_box, LV_OBJ_FLAG_HIDDEN);
    set_text(problem_1, v.problem.line1);
    set_text(problem_2, v.problem.line2);
  } else {
    lv_obj_add_flag(problem_box, LV_OBJ_FLAG_HIDDEN);
  }

  // What decides the chain's height: rows shown, and the problem's wrapped
  // text. Refit only when that changes; plain text changes keep line counts.
  uint32_t shape = f.controller_count | (lv_obj_has_flag(nodes[4].extra, LV_OBJ_FLAG_HIDDEN) ? 0 : 0x100);
  if (v.has_problem) shape |= 0x200 | (strlen(v.problem.line1) << 12) | (strlen(v.problem.line2) << 20);
  if (shape != fitted_shape) {
    fitted_shape = shape;
    fit_chain();
  }
}
