#include "host_ui.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>

#include <initializer_list>

using midi_core::Level;
using midi_core::Tone;

// Build with -DUI_TOUCH_PROBE=1 to draw a dot where the touch lands.
#ifndef UI_TOUCH_PROBE
#define UI_TOUCH_PROBE 0
#endif

// ---------- Fonts: one type scale for every screen ----------
// A label names its role, never a size. The text size (S/M/L) picks every
// role's pixels here, so each screen, and any new one, follows the setting.
// The board's font_boost moves every role up a step on big screens.
enum class Role : uint8_t { Fine, Detail, Body, Title, Heading };
constexpr int kRoles = 5;
static const lv_font_t *const kSansSteps[] = {&lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
                                              &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_22,
                                              &lv_font_montserrat_24, &lv_font_montserrat_28};
// Steps into kSansSteps, per text size and role (pixels on the 1.69").
static const uint8_t kScale[3][kRoles] = {
    // Fine Detail Body Title Heading
    {0, 1, 2, 2, 3},  // S: 12 14 16 16 18
    {1, 2, 3, 3, 4},  // M: 14 16 18 18 20
    {2, 3, 4, 5, 5},  // L: 16 18 20 22 22
};

static const lv_font_t *font_for(int step) {
  const int n = sizeof kSansSteps / sizeof kSansSteps[0];
  return kSansSteps[step < 0 ? 0 : step >= n ? n - 1 : step];
}
static const lv_font_t *font_at(int size, Role r);  // after the state it reads
static const lv_font_t *font(Role r);
// The Setup gear: an icon, the same at every text size.
static const lv_font_t *icon_font();

// ---------- Colours ----------
static lv_color_t c(uint32_t hex) { return lv_color_hex(hex); }
// kGrey is the detail text: near white, so it reads on the panel at a glance.
static const uint32_t kWhite = 0xF2F2F2, kGrey = 0xCFCFCF, kMuted = 0x8A8A8A, kDim = 0x333333, kGreen = 0x22DD44,
                      kSpine = 0x1D6B2C, kYellow = 0xFFE14A, kOrange = 0xFFA53A, kRed = 0xFF4A4A,
                      kMagenta = 0xFF6AF0, kBlue = 0x2196F3, kRow = 0x1A1A1A, kRunningRow = 0x10301A,
                      kBand = 0x171717, kIconOff = 0x7A7A7A, kWarnBg = 0x2A1508, kWarnEdge = 0x6A3A10,
                      kDangerBg = 0x3A0A0A, kDangerEdge = 0x8A1A1A, kOkBg = 0x0A2A12, kOkEdge = 0x1D6B2C;

static uint32_t level_hex(Level l) {
  switch (l) {
    case Level::Ok: return kGreen;
    case Level::Warn: return kOrange;
    case Level::Bad: return kRed;
    default: return kDim;
  }
}

static uint32_t tone_hex(Tone t) {
  switch (t) {
    case Tone::Live: return kGreen;
    case Tone::Full: return kYellow;
    case Tone::Warn: return kOrange;
    case Tone::Bad: return kRed;
    default: return kGrey;
  }
}

// ---------- State ----------
static const BoardProfile *board;
static UiSettings settings;
static UiHooks hooks;
static const char *firmware = "";
static bool has_fm1() { return hooks.loader_act != nullptr; }

// The screen's shape decides the layout (docs/ui-redesign.md, "Screen shapes"):
// a rail down the left in landscape, a band across the top otherwise; and on
// boards 320 px and wider, taller lines and bigger icons.
static bool landscape() { return board->width > board->height; }
static bool wide() { return board->width >= 320 && board->height >= 320; }

// Insets that keep content inside the glass: rounded corners eat the edges,
// a circle needs a much bigger margin, a square bezel a little.
static void safe_padding(int *side, int *top) {
  switch (board->shape) {
    case ScreenShape::Rounded: *side = board->corner_radius * 3 / 10 + 4; *top = board->corner_radius / 3 + 4; break;
    case ScreenShape::Circle: *side = board->width * 15 / 100; *top = board->height * 8 / 100; break;
    default: *side = 10; *top = 8; break;
  }
}

static const lv_font_t *font_at(int size, Role r) {
  return font_for(kScale[size < 0 ? 0 : size > 2 ? 2 : size][int(r)] + board->font_boost);
}
static const lv_font_t *font(Role r) { return font_at(settings.text_size, r); }
static const lv_font_t *icon_font() { return wide() ? &lv_font_montserrat_28 : &lv_font_montserrat_22; }

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

static void encoder_cb(lv_indev_t *, lv_indev_data_t *data) {
  bool pressed = false;
  data->enc_diff = board->read_encoder ? board->read_encoder(&pressed) : 0;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void keys_cb(lv_indev_t *, lv_indev_data_t *data) {
  static uint32_t last = LV_KEY_ENTER;
  const UiKey k = board->read_key ? board->read_key() : UiKey::None;
  switch (k) {
    case UiKey::Next: last = LV_KEY_NEXT; break;
    case UiKey::Prev: last = LV_KEY_PREV; break;
    case UiKey::Left: last = LV_KEY_LEFT; break;
    case UiKey::Right: last = LV_KEY_RIGHT; break;
    case UiKey::Enter: last = LV_KEY_ENTER; break;
    default: break;
  }
  data->key = last;  // the key being released is the one last pressed
  data->state = k == UiKey::None ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
}

// ---------- Building blocks ----------
static lv_style_t plain;

static lv_obj_t *box(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_add_style(o, &plain, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);  // taps fall through to what's under it
  return o;
}
static lv_obj_t *column(lv_obj_t *parent, int gap = 0) {
  lv_obj_t *o = box(parent);
  lv_obj_set_size(o, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(o, gap, 0);
  return o;
}
static lv_obj_t *row(lv_obj_t *parent, int gap = 0) {
  lv_obj_t *o = box(parent);
  lv_obj_set_size(o, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(o, gap, 0);
  return o;
}
// Wrapping text: never cut, it takes the lines it needs.
static lv_obj_t *text(lv_obj_t *parent, const char *s, uint32_t colour, Role role) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(l, font(role), 0);
  lv_obj_set_style_text_color(l, c(colour), 0);
  lv_label_set_text(l, s);
  return l;
}
// One line that sizes to its text (for rows).
static lv_obj_t *word(lv_obj_t *parent, const char *s, uint32_t colour, Role role) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font(role), 0);
  lv_obj_set_style_text_color(l, c(colour), 0);
  lv_label_set_text(l, s);
  return l;
}
// A word that takes the rest of its row and is cut with "..." if it must be.
static lv_obj_t *grow_word(lv_obj_t *parent, const char *s, uint32_t colour, Role role) {
  lv_obj_t *l = word(parent, s, colour, role);
  lv_obj_set_width(l, 1);
  lv_obj_set_flex_grow(l, 1);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_max_lines(l, 1);
  return l;
}

// Only touch LVGL when something really changed, so idle frames cost nothing.
static void set_text(lv_obj_t *l, const char *s) {
  if (strcmp(lv_label_get_text(l), s) != 0) lv_label_set_text(l, s);
}
static void set_colour(lv_obj_t *o, uint32_t hex) {
  if (lv_color_to_u32(lv_obj_get_style_text_color(o, LV_PART_MAIN)) != lv_color_to_u32(c(hex)))
    lv_obj_set_style_text_color(o, c(hex), 0);
}
static void set_bg(lv_obj_t *o, uint32_t hex) {
  if (lv_color_to_u32(lv_obj_get_style_bg_color(o, LV_PART_MAIN)) != lv_color_to_u32(c(hex)))
    lv_obj_set_style_bg_color(o, c(hex), 0);
}
static void show_if(lv_obj_t *o, bool shown) {
  if (shown == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
    if (shown) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}

// A bordered card, for status and errors.
static lv_obj_t *card(lv_obj_t *parent, uint32_t bg, uint32_t edge) {
  lv_obj_t *k = column(parent, 3);
  lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(k, c(bg), 0);
  lv_obj_set_style_border_color(k, c(edge), 0);
  lv_obj_set_style_border_width(k, 1, 0);
  lv_obj_set_style_radius(k, 8, 0);
  lv_obj_set_style_pad_all(k, 7, 0);
  return k;
}

// ---------- Focus without touch ----------
// Every screen works by focus too: an LVGL group holds the screen's focusable
// objects in reading order (the nav icons, then each line). The ring is drawn
// only in LV_STATE_FOCUS_KEY, which LVGL sets for keypad and encoder focus, so
// touch boards never show it. It's a border always present but transparent
// (or an outline, outside the object), so nothing moves when focus arrives.
static lv_group_t *group;
constexpr lv_obj_flag_t kFocusable = LV_OBJ_FLAG_USER_1;
enum class Ring { Border, Outline };
static void key_event(lv_event_t *e);
static void pressed_bg(lv_obj_t *o, uint32_t hex) { lv_obj_set_style_bg_color(o, c(hex), LV_STATE_PRESSED); }

static void focusable(lv_obj_t *o, Ring ring) {
  lv_obj_add_flag(o, kFocusable);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);  // an encoder's press would edit a scrollable object
  lv_obj_set_style_outline_width(o, 0, LV_STATE_FOCUS_KEY);  // not the theme's ring as well
  if (ring == Ring::Border) {
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, c(kBlue), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
  } else {
    lv_obj_set_style_outline_width(o, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(o, c(kBlue), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(o, 3, LV_STATE_FOCUS_KEY);
  }
  lv_obj_add_event_cb(o, key_event, LV_EVENT_KEY, nullptr);
}

static bool really_shown(lv_obj_t *o) {
  for (; o; o = lv_obj_get_parent(o))
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  return true;
}
static void add_focusables(lv_obj_t *o) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_obj_t *ch = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_has_flag(ch, kFocusable)) lv_group_add_obj(group, ch);
    add_focusables(ch);
  }
}
// Fills the group from the active screen, keeping the focus where it was.
static void refresh_group() {
  if (!group) return;
  lv_obj_t *had = lv_group_get_focused(group);
  lv_group_remove_all_objs(group);
  add_focusables(lv_screen_active());
  if (had && lv_obj_get_group(had) == group && really_shown(had)) lv_group_focus_obj(had);
}

// ---------- Pages: sections, sub-pages, job pages ----------
// The three sections carry the nav. A sub-page has a back row instead, and a
// stack remembers the way back. Job pages have neither: a job's boot routes
// nothing, and there's nowhere to go.
enum Section { kPlay, kFm1, kSetup, kSections };
static lv_obj_t *section_screens[kSections];
static int section = kPlay;
static lv_obj_t *stack[4];
static int depth;
static bool job_showing;
static lv_obj_t *switching_screen;  // ui_show_switching(), until a page replaces it

static void show(lv_obj_t *scr) {
  lv_screen_load(scr);
  refresh_group();
}
static void go_section(int s) {
  section = s;
  depth = 1;
  stack[0] = section_screens[s];
  show(stack[0]);
}
static void push(lv_obj_t *scr) {
  if (depth < 4) stack[depth++] = scr;
  show(scr);
}
static void go_back() {
  if (depth > 1) --depth;
  show(stack[depth - 1]);
}
static bool on_section_page() { return !job_showing && depth == 1 && lv_screen_active() == stack[0]; }

// Left and right: the next section on a section page, back on a sub-page.
static int next_section(int from, int dir) {
  for (int s = (from + dir + kSections) % kSections; s != from; s = (s + dir + kSections) % kSections)
    if (section_screens[s]) return s;
  return from;
}
static void key_event(lv_event_t *e) {
  const uint32_t key = lv_event_get_key(e);
  if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT) return;
  if (on_section_page()) go_section(next_section(section, key == LV_KEY_RIGHT ? 1 : -1));
  else if (!job_showing && depth > 1 && key == LV_KEY_LEFT) go_back();
}

// ---------- The nav icons ----------
// A8 images drawn once at start-up, so the icon's colour is its recolor: white
// when active, grey otherwise. Each pixel is the shape's coverage, 4x4 samples.
static int icon_size() { return wide() ? 30 : 24; }
static uint8_t icon_pixels[2][30 * 30];
static lv_image_dsc_t icon_dsc[2];

static bool in_round_rect(float x, float y, float x0, float y0, float w, float h, float r) {
  if (x < x0 || y < y0 || x > x0 + w || y > y0 + h) return false;
  const float cx = x < x0 + r ? x0 + r : x > x0 + w - r ? x0 + w - r : x;
  const float cy = y < y0 + r ? y0 + r : y > y0 + h - r ? y0 + h - r : y;
  return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}
static bool in_frame(float x, float y, float x0, float y0, float w, float h, float r, float t) {
  return in_round_rect(x, y, x0, y0, w, h, r) && !in_round_rect(x, y, x0 + t, y0 + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0);
}

// Play: a 5-pin MIDI DIN socket, pins round the top, key at the bottom.
static bool midi_shape(float x, float y, int s) {
  const float m = s / 2.0f, bw = s >= 28 ? 3 : 2, dx = x - m, dy = y - m, d2 = dx * dx + dy * dy;
  if (d2 <= m * m && d2 >= (m - bw) * (m - bw)) return true;
  const float pin = (s >= 28 ? 4 : 3) / 2.0f, r = s * 0.26f;
  for (int i = 0; i < 5; ++i) {
    const float a = 3.14159265f * (1.0f + i * 0.25f);  // left, upper left, top, upper right, right
    const float px = m + r * cosf(a), py = m + r * sinf(a) + s / 10.0f;
    if ((x - px) * (x - px) + (y - py) * (y - py) <= pin * pin) return true;
  }
  const float kw = s / 4.0f, kh = s / 7.0f;
  return in_round_rect(x, y, (s - kw) / 2, s * 0.7f, kw, kh, 1);
}

// FM-1: a cartridge: a wide shell with its label, and a narrower connector tab
// with contacts underneath.
static bool cart_shape(float x, float y, int s) {
  const float bw = s >= 28 ? 3 : 2, sh = s * 0.64f, tw = s * 0.66f, tx = (s - tw) / 2, top = s / 12.0f;
  if (in_frame(x, y, 0, top, s, sh, 3, bw)) return true;
  if (in_round_rect(x, y, bw + 3, top + bw + 3, s - 2 * (bw + 3), sh - 2 * (bw + 3), 1)) return true;  // the label
  if (y >= top + sh && in_frame(x, y, tx, top + sh - bw, tw, s - top - sh + bw, 2, bw)) return true;
  const float inner = tw - 2 * (bw + 2);
  for (int i = 0; i < 3; ++i) {  // contacts
    const float cx = tx + bw + 2 + i * (inner - 2) / 2;
    if (x >= cx && x <= cx + 2 && y >= top + sh + 1 && y <= s - bw - 1) return true;
  }
  return false;
}

static void draw_icon(int which, bool (*shape)(float, float, int)) {
  const int s = icon_size();
  for (int y = 0; y < s; ++y)
    for (int x = 0; x < s; ++x) {
      int hits = 0;
      for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx) hits += shape(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4, s);
      icon_pixels[which][y * s + x] = uint8_t(hits * 255 / 16);
    }
  lv_image_dsc_t &d = icon_dsc[which];
  d = lv_image_dsc_t{};
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_A8;
  d.header.w = s;
  d.header.h = s;
  d.header.stride = s;
  d.data_size = uint32_t(s * s);
  d.data = icon_pixels[which];
}

// One nav per section page: each cell is an icon, a bar that marks the active
// section, and an attention dot.
struct NavCell {
  lv_obj_t *cell, *icon, *bar, *dot;
};
static NavCell nav[kSections][kSections];  // [page][item]
static bool dot_play, dot_fm1;              // orange: a chain problem; red: an install unfinished

static void nav_tapped(lv_event_t *e) {
  const int s = int(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (s != section || !on_section_page()) go_section(s);
}

static void nav_item(lv_obj_t *band, int page, int which) {
  const int s = icon_size();
  const bool active = which == page;
  NavCell &n = nav[page][which];
  n.cell = box(band);
  lv_obj_set_size(n.cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(n.cell, landscape() ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(n.cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(n.cell, 5, 0);
  lv_obj_set_style_pad_column(n.cell, 10, 0);
  lv_obj_set_style_pad_hor(n.cell, landscape() ? 0 : 8, 0);
  lv_obj_set_style_radius(n.cell, 8, 0);
  lv_obj_set_ext_click_area(n.cell, 6);
  focusable(n.cell, Ring::Outline);
  lv_obj_add_event_cb(n.cell, nav_tapped, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(which)));

  n.bar = box(n.cell);
  lv_obj_set_size(n.bar, landscape() ? 3 : s, landscape() ? s + 6 : 3);
  lv_obj_set_style_radius(n.bar, 1, 0);
  lv_obj_set_style_bg_color(n.bar, c(kBlue), 0);
  lv_obj_set_style_bg_opa(n.bar, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

  // The icon in a box of its own, so the dot can sit on its corner.
  lv_obj_t *holder = box(n.cell);
  lv_obj_set_size(holder, s, s);
  lv_obj_add_flag(holder, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  const uint32_t col = active ? kWhite : kIconOff;
  if (which == kSetup) {
    n.icon = word(holder, LV_SYMBOL_SETTINGS, col, Role::Body);
    lv_obj_set_style_text_font(n.icon, icon_font(), 0);
    lv_obj_center(n.icon);
  } else {
    n.icon = lv_image_create(holder);
    lv_image_set_src(n.icon, &icon_dsc[which == kPlay ? 0 : 1]);
    lv_obj_set_style_image_recolor(n.icon, c(col), 0);
    lv_obj_set_style_image_recolor_opa(n.icon, LV_OPA_COVER, 0);
  }
  if (!landscape()) lv_obj_move_to_index(n.bar, -1);  // under the icon
  n.dot = box(holder);
  lv_obj_add_flag(n.dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_pos(n.dot, s - 7, -2);
  lv_obj_set_size(n.dot, 9, 9);
  lv_obj_set_style_radius(n.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(n.dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(n.dot, c(which == kFm1 ? kRed : kOrange), 0);
  lv_obj_set_style_border_width(n.dot, 2, 0);
  lv_obj_set_style_border_color(n.dot, c(kBand), 0);
  lv_obj_add_flag(n.dot, LV_OBJ_FLAG_HIDDEN);
}

static void show_dots() {
  for (int p = 0; p < kSections; ++p) {
    if (!section_screens[p]) continue;
    if (nav[p][kPlay].dot) show_if(nav[p][kPlay].dot, dot_play);
    if (nav[p][kFm1].dot) show_if(nav[p][kFm1].dot, dot_fm1);
  }
}

// A section page: the nav, then the content, which scrolls (taking the band
// with it in portrait) only if it must. A pinned hold goes in foot.
struct Page {
  lv_obj_t *scr, *page, *content, *foot;
};

static void scroll_only_if_needed(lv_obj_t *page) {
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(page, LV_DIR_VER);
  lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_ACTIVE);
}

static lv_obj_t *black_screen() {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, c(0x000000), 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

static Page section_page(int which) {
  int side, top;
  safe_padding(&side, &top);
  Page t;
  t.scr = section_screens[which] = black_screen();
  lv_obj_set_flex_flow(t.scr, landscape() ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_t *band;
  if (landscape()) {
    band = box(t.scr);  // the rail: bleeds off the left, top and bottom edges
    lv_obj_set_size(band, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(band, 14, 0);
    lv_obj_set_style_pad_left(band, board->shape == ScreenShape::Rect ? 0 : side - 6, 0);
    lv_obj_set_style_pad_right(band, 12, 0);
    lv_obj_set_style_pad_row(band, 22, 0);
    t.foot = column(t.scr);
    lv_obj_set_height(t.foot, lv_pct(100));
    lv_obj_set_width(t.foot, 1);
    lv_obj_set_flex_grow(t.foot, 1);
    t.page = box(t.foot);
    lv_obj_set_width(t.page, lv_pct(100));
    lv_obj_set_flex_grow(t.page, 1);
  } else {
    t.page = box(t.scr);
    lv_obj_set_width(t.page, lv_pct(100));
    lv_obj_set_flex_grow(t.page, 1);
    band = box(t.page);  // bleeds off the top and both sides
    lv_obj_set_size(band, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(band, board->shape == ScreenShape::Rect ? 10 : 9, 0);
    lv_obj_set_style_pad_bottom(band, 4, 0);
    lv_obj_set_style_pad_column(band, wide() ? 34 : 18, 0);
    t.foot = t.scr;
  }
  lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(band, c(kBand), 0);
  for (int i = 0; i < kSections; ++i)
    if (i != kFm1 || has_fm1()) nav_item(band, which, i);

  lv_obj_set_flex_flow(t.page, LV_FLEX_FLOW_COLUMN);
  scroll_only_if_needed(t.page);
  t.content = column(t.page, 6);
  lv_obj_set_style_pad_hor(t.content, landscape() ? 12 : side, 0);
  lv_obj_set_style_pad_top(t.content, landscape() ? top + 6 : 10, 0);
  lv_obj_set_style_pad_bottom(t.content, top, 0);
  if (landscape()) lv_obj_set_style_pad_right(t.content, side + 4, 0);
  return t;
}

// A page under a section: a back row on top (none on a job's page), a
// scrolling middle, and the foot for a pinned hold or a button.
static void back_tapped(lv_event_t *) { go_back(); }

static Page sub_page(lv_obj_t **title, lv_obj_t **back_row, bool back) {
  int side, top;
  safe_padding(&side, &top);
  Page t;
  t.scr = black_screen();
  lv_obj_set_flex_flow(t.scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_t *head = column(t.scr);
  lv_obj_set_style_pad_hor(head, board->shape == ScreenShape::Rounded ? board->corner_radius / 2 + 4 : side, 0);
  lv_obj_set_style_pad_top(head, top, 0);
  lv_obj_t *r = row(head, 6);
  lv_obj_set_style_radius(r, 8, 0);
  lv_obj_set_style_pad_ver(r, 2, 0);
  lv_obj_t *arrow = word(r, LV_SYMBOL_LEFT, kMuted, Role::Title);
  *title = grow_word(r, "", kWhite, Role::Heading);
  lv_label_set_long_mode(*title, LV_LABEL_LONG_MODE_WRAP);  // "Switching to Felucca 1.6.1" takes two lines
  lv_label_set_max_lines(*title, 0);
  if (back) {
    focusable(r, Ring::Border);
    lv_obj_set_ext_click_area(r, 4);
    lv_obj_add_event_cb(r, back_tapped, LV_EVENT_CLICKED, nullptr);
  } else {
    lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);
  }
  if (back_row) *back_row = r;
  t.page = box(t.scr);
  lv_obj_set_width(t.page, lv_pct(100));
  lv_obj_set_flex_grow(t.page, 1);
  lv_obj_set_flex_flow(t.page, LV_FLEX_FLOW_COLUMN);
  scroll_only_if_needed(t.page);
  t.content = column(t.page, 6);
  lv_obj_set_style_pad_hor(t.content, side, 0);
  lv_obj_set_style_pad_top(t.content, 8, 0);
  lv_obj_set_style_pad_bottom(t.content, top, 0);
  t.foot = t.scr;
  return t;
}

// The foot's box: under the scrolling page, so what's in it never scrolls away.
static lv_obj_t *foot_box(Page &t) {
  int side, top;
  safe_padding(&side, &top);
  lv_obj_t *f = column(t.foot);
  lv_obj_set_style_pad_hor(f, landscape() && t.foot != t.scr ? 12 : side, 0);
  if (landscape() && t.foot != t.scr) lv_obj_set_style_pad_right(f, side + 4, 0);
  lv_obj_set_style_pad_bottom(f, top, 0);
  lv_obj_set_style_pad_top(f, 4, 0);
  return f;
}

static lv_obj_t *plain_button(lv_obj_t *parent, const char *what, lv_event_cb_t on_click) {
  lv_obj_t *b = box(parent);
  lv_obj_set_size(b, lv_pct(100), 40);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, c(kDim), 0);
  pressed_bg(b, 0x5A5A5A);
  focusable(b, Ring::Border);
  lv_obj_t *l = word(b, what, kWhite, Role::Body);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, nullptr);
  return b;
}

// A tappable row that opens a sub-page: a title, one quieter line, a chevron.
static lv_obj_t *nav_row(lv_obj_t *parent, const char *title, lv_obj_t **sub, lv_event_cb_t on_click) {
  lv_obj_t *r = row(parent, 6);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(r, c(0x1E1E1E), 0);
  pressed_bg(r, 0x3A3A3A);
  lv_obj_set_style_radius(r, 8, 0);
  lv_obj_set_style_pad_all(r, 7, 0);
  focusable(r, Ring::Border);
  lv_obj_add_event_cb(r, on_click, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *col = column(r, 1);
  lv_obj_set_width(col, 1);
  lv_obj_set_flex_grow(col, 1);
  text(col, title, kWhite, Role::Title);
  lv_obj_t *s = text(col, "", kMuted, Role::Detail);
  if (sub) *sub = s;
  word(r, LV_SYMBOL_RIGHT, kMuted, Role::Body);
  return r;
}

// ---------- The hold ----------
// Anything that touches the FM-1 or stops MIDI needs a 2-second hold, never a
// single tap (loader safety rule 7). Its label always names exactly what it
// runs; the bar fills while the finger (or the encoder's press, or ENTER)
// stays down.
static constexpr uint32_t kHoldMs = 2000;
struct Hold {
  lv_obj_t *button, *bar, *label;
  char text[48];
  bool enabled;
  uint32_t started;  // millis() when the hold began, 0 when not held
  bool fired;
  void (*run)(Hold &);
  UiAct act;
  int index;
};
static Hold plan_hold, backups_hold, fm1_hold, result_hold, computer_hold;

static void hold_draw(Hold &h) {
  char buf[56];
  snprintf(buf, sizeof buf, "Hold: %s", h.text);
  if (!h.started && !h.fired) {
    set_text(h.label, buf);
    lv_bar_set_value(h.bar, 0, LV_ANIM_OFF);
  }
  set_colour(h.label, h.enabled ? kWhite : kMuted);
  lv_obj_set_style_border_opa(h.button, h.enabled ? LV_OPA_COVER : LV_OPA_30, 0);
}

static void hold_set(Hold &h, const char *what, bool enabled) {
  if (strcmp(h.text, what) != 0) {
    strlcpy(h.text, what, sizeof h.text);
    h.fired = false;  // a new action: the hold is fresh
  }
  h.enabled = enabled;
  hold_draw(h);
}

// LVGL sends PRESSING only for a pointer; an encoder or a key sends PRESSED
// and RELEASED. So a timer fills the bar while the hold's button is down.
static Hold *holding;  // the hold being held, if any
static lv_timer_t *hold_timer;

static void hold_tick(lv_timer_t *) {
  Hold *h = holding;
  if (!h || !h->started || h->fired) return;
  const uint32_t held = millis() - h->started;
  lv_bar_set_value(h->bar, held >= kHoldMs ? 100 : int32_t(held * 100 / kHoldMs), LV_ANIM_OFF);
  if (held < kHoldMs) return;
  h->fired = true;
  h->started = 0;
  holding = nullptr;
  lv_label_set_text(h->label, "Starting...");
  if (h->run) h->run(*h);
}

static void hold_event(lv_event_t *e) {
  Hold &h = *static_cast<Hold *>(lv_event_get_user_data(e));
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    h.started = h.enabled && !h.fired ? (millis() ? millis() : 1) : 0;
    holding = h.started ? &h : nullptr;
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DELETE) {
    h.started = 0;
    if (holding == &h) holding = nullptr;
    if (!h.fired && code != LV_EVENT_DELETE) lv_bar_set_value(h.bar, 0, LV_ANIM_OFF);
  }
}

static void hold_widget(lv_obj_t *parent, Hold &h, uint32_t fill, void (*run)(Hold &)) {
  h = Hold{};
  h.run = run;
  h.index = -1;
  h.button = box(parent);
  lv_obj_set_size(h.button, lv_pct(100), 44);
  lv_obj_set_style_radius(h.button, 8, 0);
  lv_obj_set_style_bg_opa(h.button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(h.button, c(0x3A3A2A), 0);
  lv_obj_set_style_border_color(h.button, c(fill), 0);
  lv_obj_set_style_border_width(h.button, 1, 0);
  lv_obj_set_style_clip_corner(h.button, true, 0);
  lv_obj_set_ext_click_area(h.button, 3);
  focusable(h.button, Ring::Outline);
  h.bar = lv_bar_create(h.button);
  lv_obj_set_size(h.bar, lv_pct(100), lv_pct(100));
  lv_obj_set_style_radius(h.bar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(h.bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(h.bar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_color(h.bar, c(fill), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(h.bar, LV_OPA_50, LV_PART_INDICATOR);
  lv_obj_remove_flag(h.bar, LV_OBJ_FLAG_CLICKABLE);
  lv_bar_set_range(h.bar, 0, 100);
  h.label = lv_label_create(h.button);
  lv_obj_set_width(h.label, lv_pct(94));
  lv_label_set_long_mode(h.label, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_align(h.label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(h.label, font(Role::Fine), 0);
  lv_label_set_text(h.label, "");
  lv_obj_center(h.label);
  lv_obj_add_event_cb(h.button, hold_event, LV_EVENT_ALL, &h);
  if (!hold_timer) hold_timer = lv_timer_create(hold_tick, 20, nullptr);
}

static void run_loader_hold(Hold &h) {
  if (hooks.loader_act) hooks.loader_act(h.act, h.index);
}

// ---------- The error pattern ----------
// What happened (the page's title), is the FM-1 safe, why, what to do, then
// the detail for us, small and last.
struct ProblemView {
  lv_obj_t *headline, *card, *icon, *safety, *why, *todo, *detail;
};
static ProblemView problem_view(lv_obj_t *parent, bool with_headline) {
  ProblemView v{};
  if (with_headline) v.headline = text(parent, "", kOrange, Role::Heading);
  v.card = card(parent, kOkBg, kOkEdge);
  lv_obj_t *r = row(v.card, 6);
  v.icon = word(r, LV_SYMBOL_OK, kGreen, Role::Body);
  v.safety = text(r, "", kWhite, Role::Body);
  lv_obj_set_width(v.safety, 1);
  lv_obj_set_flex_grow(v.safety, 1);
  v.why = text(parent, "", kGrey, Role::Body);
  v.todo = text(parent, "", kWhite, Role::Body);
  v.detail = text(parent, "", kMuted, Role::Fine);
  return v;
}
static uint32_t headline_hex(const UiResult &r) {
  return r.ok ? kGreen : r.safety == UiSafety::Danger ? kRed : kOrange;
}
static void fill_problem(ProblemView &v, const UiResult &r) {
  if (v.headline) {
    set_text(v.headline, r.headline);
    set_colour(v.headline, headline_hex(r));
  }
  const bool danger = r.safety == UiSafety::Danger;
  set_bg(v.card, danger ? kDangerBg : kOkBg);
  lv_obj_set_style_border_color(v.card, c(danger ? kDangerEdge : kOkEdge), 0);
  set_text(v.icon, danger ? LV_SYMBOL_WARNING : LV_SYMBOL_OK);
  set_colour(v.icon, danger ? kRed : kGreen);
  set_text(v.safety, r.safety_text);
  set_text(v.why, r.why);
  set_text(v.todo, r.todo);
  set_text(v.detail, r.detail);
  show_if(v.why, r.why[0]);
  show_if(v.todo, r.todo[0]);
  show_if(v.detail, r.detail[0]);
}

// One step of a job, or of a plan: done, now, or to come.
struct StepRow {
  lv_obj_t *row, *icon, *text;
};
static StepRow step_row(lv_obj_t *parent) {
  StepRow s;
  s.row = row(parent, 6);
  lv_obj_set_flex_align(s.row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  s.icon = word(s.row, "-", kMuted, Role::Body);
  lv_obj_set_width(s.icon, 16);
  s.text = text(s.row, "", kMuted, Role::Body);
  lv_obj_set_width(s.text, 1);
  lv_obj_set_flex_grow(s.text, 1);
  return s;
}
static void fill_step(StepRow &s, UiStepState state, const char *what) {
  set_text(s.icon, state == UiStepState::Done ? LV_SYMBOL_OK : state == UiStepState::Now ? LV_SYMBOL_RIGHT : "-");
  set_colour(s.icon, state == UiStepState::Done ? kGreen : state == UiStepState::Now ? kYellow : kMuted);
  set_text(s.text, what);
  set_colour(s.text, state == UiStepState::Next ? kMuted : state == UiStepState::Now ? kWhite : kGrey);
}

// ---------- Play: one line per stage ----------
// Hub, Controllers, Router, FM-1: a dot, a name and one status. Tapping a line
// opens that stage's detail (today's lines, wrapping), one at a time; a second
// tap closes it. A problem opens its own stage, with the fix card.
using midi_core::kStages;
struct StageLine {
  lv_obj_t *up, *dot, *spine, *head, *title, *status, *chevron, *detail, *problem, *problem_1, *problem_2;
  lv_obj_t *lines[midi_core::kMaxDevices + 1];
  int line_count;
};
static StageLine stages[kStages];
static lv_obj_t *play_page;
static int open_stage = -1;
static bool stage_tapped_open;  // the user opened open_stage: Play may scroll to it
static uint32_t problem_seen;  // what the last problem was, so a new one opens its stage once
static UiFrame last_frame;
static bool have_frame;
static UiLoader last_loader;
static bool have_loader;
static int play_shrink;
static uint32_t fitted_play = UINT32_MAX;
// The status under the name: always on the 1.69", where a name and its status
// side by side get cut at any readable size; at L on bigger boards.
static bool stacked() { return !wide() || settings.text_size == 2; }

static void fit_play();
static void stage_tapped(lv_event_t *e) {
  const int i = int(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  open_stage = open_stage == i ? -1 : i;
  stage_tapped_open = open_stage >= 0;
  if (have_frame) ui_update(last_frame);
  refresh_group();
}

static void build_play() {
  Page t = section_page(kPlay);
  play_page = t.page;
  lv_obj_set_style_pad_row(t.content, 0, 0);
  static const char *const kTitles[kStages] = {"Hub", "Controllers", "Router", "FM-1"};
  static const uint32_t kTitleColours[kStages] = {kWhite, kYellow, kWhite, kMagenta};
  const int dot = wide() ? 14 : 12;
  for (int i = 0; i < kStages; ++i) {
    StageLine &s = stages[i];
    lv_obj_t *r = row(t.content, 0);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // The rail: the spine from the stage above, the dot, then the spine down
    // to the next stage.
    lv_obj_t *rail = box(r);
    lv_obj_add_flag(rail, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(rail, dot, lv_pct(100));
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s.up = box(rail);
    lv_obj_set_width(s.up, 2);
    lv_obj_set_style_bg_opa(s.up, i > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s.up, c(kDim), 0);
    s.dot = box(rail);
    lv_obj_set_size(s.dot, dot, dot);
    lv_obj_set_style_radius(s.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s.dot, c(kDim), 0);
    s.spine = box(rail);
    lv_obj_set_width(s.spine, 2);
    lv_obj_set_flex_grow(s.spine, 1);
    lv_obj_set_style_bg_opa(s.spine, i + 1 < kStages ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s.spine, c(kDim), 0);

    lv_obj_t *col = column(r, 2);
    lv_obj_set_style_pad_left(col, dot + 8, 0);
    s.head = stacked() ? column(col, 0) : row(col, 8);
    lv_obj_set_style_pad_hor(s.head, 6, 0);
    lv_obj_set_style_pad_ver(s.head, stacked() ? 1 : 0, 0);
    lv_obj_set_style_radius(s.head, 8, 0);
    lv_obj_set_style_bg_color(s.head, c(kRow), 0);
    pressed_bg(s.head, 0x2A2A2A);
    lv_obj_set_style_bg_opa(s.head, LV_OPA_COVER, LV_STATE_PRESSED);
    focusable(s.head, Ring::Border);
    lv_obj_add_event_cb(s.head, stage_tapped, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(i)));
    s.title = word(s.head, kTitles[i], kTitleColours[i], Role::Title);
    s.status = word(s.head, "", kGrey, Role::Detail);
    if (!stacked()) {
      lv_obj_set_width(s.status, 1);
      lv_obj_set_flex_grow(s.status, 1);
      lv_obj_set_style_text_align(s.status, LV_TEXT_ALIGN_RIGHT, 0);
      lv_label_set_long_mode(s.status, LV_LABEL_LONG_MODE_DOTS);
      lv_label_set_max_lines(s.status, 1);
    }
    s.chevron = wide() && !stacked() ? word(s.head, LV_SYMBOL_DOWN, kIconOff, Role::Fine) : nullptr;

    s.detail = column(col, 1);
    lv_obj_set_style_pad_left(s.detail, 6, 0);
    lv_obj_set_style_pad_bottom(s.detail, 8, 0);
    const int lines = i == 1 ? int(midi_core::kMaxDevices) + 1 : 3;
    s.line_count = lines;
    for (int l = 0; l < lines; ++l) s.lines[l] = text(s.detail, "", kGrey, Role::Detail);
    s.problem = card(s.detail, kWarnBg, kWarnEdge);
    s.problem_1 = text(s.problem, "", kOrange, Role::Body);
    s.problem_2 = text(s.problem, "", kWhite, Role::Body);
    lv_obj_add_flag(s.problem, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s.detail, LV_OBJ_FLAG_HIDDEN);
  }
}

// Lines 36 px tall on the 1.69", 44 on 320+: each is a real hit target. When
// Play doesn't fit, the gaps tighten; the text keeps the size the user picked,
// and the page scrolls to the open stage instead.
static void size_play(int shrink) {
  const int line_h = (wide() ? 44 : 36) - (shrink >= 1 ? 6 : 0) - (shrink >= 2 ? 4 : 0);
  const int dot = wide() ? 14 : 12;
  for (StageLine &s : stages) {
    if (!stacked()) lv_obj_set_height(s.head, line_h);
    lv_obj_set_style_pad_bottom(s.detail, shrink >= 1 ? 4 : 8, 0);
    // The dot sits beside the name: on its line when stacked, mid-row otherwise.
    const int line = stacked() ? lv_font_get_line_height(font(Role::Title)) + 4 : line_h;
    lv_obj_set_height(s.up, (line - dot) / 2);
  }
}

static bool play_overflows() {
  lv_obj_t *scr = lv_obj_get_screen(play_page);
  lv_obj_update_layout(scr);
  lv_obj_mark_layout_as_dirty(play_page);
  lv_obj_update_layout(scr);
  return lv_obj_get_scroll_bottom(play_page) > 0;
}

static void fit_play() {
  lv_obj_scroll_to_y(play_page, 0, LV_ANIM_OFF);
  for (play_shrink = 0; play_shrink < 2; ++play_shrink) {
    size_play(play_shrink);
    if (!play_overflows()) return;
  }
  size_play(play_shrink);
  if (!play_overflows()) return;
  // Still too tall: a stage the user opened goes to the top, so its lines show
  // and the nav isn't left half cut. A swipe down brings the rest back. One a
  // problem opened stays put, with the nav at the top: after a restart Play
  // came up scrolled past it (2026-11-05).
  if (open_stage < 0 || !stage_tapped_open) return;
  lv_area_t page, head;
  lv_obj_get_coords(play_page, &page);
  lv_obj_get_coords(stages[open_stage].head, &head);
  const int down = head.y1 - page.y1 - 4;
  if (down > 0) lv_obj_scroll_by(play_page, 0, -down, LV_ANIM_OFF);
}

static void set_line(StageLine &s, int i, const char *text_, uint32_t colour) {
  set_text(s.lines[i], text_);
  set_colour(s.lines[i], colour);
  show_if(s.lines[i], text_[0]);
}

void ui_update(const UiFrame &f) {
  if (&f != &last_frame) last_frame = f;
  have_frame = true;
  const midi_core::ChainView &v = f.view;
  dot_play = v.has_problem;
  show_dots();

  // A new problem opens its own stage, once: after that the stages are the user's.
  uint32_t key = 0;
  if (v.has_problem) {
    key = 1u + uint32_t(v.problem_stage);
    for (const char *p = v.problem.line1; *p; ++p) key = key * 31 + uint8_t(*p);
  }
  if (key != problem_seen) {
    problem_seen = key;
    if (v.has_problem && v.problem_stage < kStages) {
      open_stage = v.problem_stage;
      stage_tapped_open = false;
    }
  }

  const Level levels[kStages] = {v.hub, v.controllers, v.router, v.target};
  for (int i = 0; i < kStages; ++i) {
    StageLine &s = stages[i];
    set_bg(s.dot, level_hex(levels[i]));
    set_bg(s.spine, i + 2 < v.first_broken ? kSpine : kDim);  // node i + 1 to the next
    set_bg(s.up, i + 1 < v.first_broken ? kSpine : kDim);
    set_text(s.status, f.stages[i].text);
    set_colour(s.status, tone_hex(f.stages[i].tone));
    const bool open = i == open_stage;
    show_if(s.detail, open);
    // Stacked, an open stage takes the room of the others' statuses; their dots
    // still show how they are.
    show_if(s.status, !stacked() || open_stage < 0 || open);
    lv_obj_set_style_bg_opa(s.head, open ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (s.chevron) set_text(s.chevron, open ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    const bool problem_here = v.has_problem && v.problem_stage == i;
    show_if(s.problem, problem_here);
    if (problem_here) {
      set_text(s.problem_1, v.problem.line1);
      set_text(s.problem_2, v.problem.line2);
    }
  }

  char buf[64];
  StageLine &hub = stages[midi_core::kStageHub];
  set_line(hub, 0, f.hub_detail, kGrey);
  snprintf(buf, sizeof buf, "%u of 8 USB channels in use", unsigned(f.snapshot.channels_used));
  set_line(hub, 1, buf, f.snapshot.channels_used >= 8 ? kYellow : kGrey);
  set_line(hub, 2, "", kGrey);

  StageLine &ctl = stages[midi_core::kStageControllers];
  set_text(ctl.title, f.controller_count > 1 ? "Controllers" : "Controller");
  for (int i = 0; i < int(midi_core::kMaxDevices); ++i) {
    if (i >= f.controller_count) {
      set_line(ctl, i, "", kGrey);
      continue;
    }
    const UiDeviceLine &d = f.controllers[i];
    snprintf(buf, sizeof buf, "%s  %s", d.tag, d.detail);
    set_line(ctl, i, buf, !d.open ? kRed : d.fresh ? kGreen : kGrey);
  }
  set_line(ctl, int(midi_core::kMaxDevices), f.controller_count ? "" : "none", kGrey);

  StageLine &router = stages[midi_core::kStageRouter];
  snprintf(buf, sizeof buf, "%lu passed", (unsigned long)f.sent);
  set_line(router, 0, buf, kGrey);
  snprintf(buf, sizeof buf, "%lu SysEx blocked", (unsigned long)f.blocked);
  set_line(router, 1, buf, kGrey);
  set_line(router, 2, "", kGrey);

  StageLine &fm1 = stages[midi_core::kStageFm1];
  if (have_loader && last_loader.running[0] && v.target == Level::Ok) snprintf(buf, sizeof buf, "runs %s", last_loader.running);
  else buf[0] = 0;
  set_line(fm1, 0, buf, kWhite);
  set_line(fm1, 1, f.fm1_in, v.target == Level::Ok ? (f.fm1_in_fresh ? kGreen : kGrey) : level_hex(v.target));
  set_line(fm1, 2, f.fm1_own, kGrey);

  // What decides Play's height: the open stage, its lines, and the problem's
  // wrapped text. Refit only when that changes.
  uint32_t shape = uint32_t(open_stage + 1) | (uint32_t(f.controller_count) << 4);
  if (v.has_problem) shape |= 0x100 | (strlen(v.problem.line1) << 12) | (strlen(v.problem.line2) << 20);
  if (shape != fitted_play) {
    fitted_play = shape;
    fit_play();
  }
}

// ---------- FM-1: the firmware list ----------
struct ItemRow {
  lv_obj_t *row, *mark, *name, *note;
};
static ItemRow item_rows[kLoaderItems];
static lv_obj_t *fm1_list, *fm1_blocked, *backups_sub, *fm1_foot;
static ProblemView fm1_problem;
static lv_obj_t *plan_screen, *plan_title, *plan_lines[kPlanLines], *backups_screen, *backups_latest;
static StepRow plan_steps[kPlanLines];
static int wanted_plan = -1;  // the row whose plan was opened

static void item_tapped(lv_event_t *e) {
  const int i = int(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  wanted_plan = i;
  if (hooks.loader_select) hooks.loader_select(i);
  if (have_loader) ui_update_loader(last_loader);
  push(plan_screen);
}
static void backups_tapped(lv_event_t *) { push(backups_screen); }

static void build_fm1() {
  Page t = section_page(kFm1);
  fm1_list = column(t.content, 6);
  for (int i = 0; i < kLoaderItems; ++i) {
    ItemRow &r = item_rows[i];
    // On the 1.69" at L the note drops under the name, as Play's status does.
    r.row = row(fm1_list, 8);
    lv_obj_set_height(r.row, stacked() ? LV_SIZE_CONTENT : wide() ? 44 : 34);
    if (stacked()) {
      lv_obj_set_flex_flow(r.row, LV_FLEX_FLOW_ROW_WRAP);
      lv_obj_set_style_pad_ver(r.row, 5, 0);
      lv_obj_set_style_pad_row(r.row, 0, 0);
    }
    lv_obj_set_style_bg_opa(r.row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r.row, c(kRow), 0);
    pressed_bg(r.row, 0x3A3A3A);
    lv_obj_set_style_radius(r.row, 8, 0);
    lv_obj_set_style_pad_hor(r.row, 8, 0);
    focusable(r.row, Ring::Border);
    lv_obj_add_event_cb(r.row, item_tapped, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(i)));
    r.mark = word(r.row, LV_SYMBOL_PLAY, kGreen, Role::Detail);
    r.name = grow_word(r.row, "", kWhite, Role::Title);
    r.note = word(r.row, "", kMuted, Role::Detail);
    if (stacked()) {
      lv_obj_set_width(r.name, LV_SIZE_CONTENT);
      lv_obj_set_flex_grow(r.name, 0);
      lv_obj_add_flag(r.note, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    }
    lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_t *gap = box(fm1_list);
  lv_obj_set_size(gap, 1, 4);
  nav_row(fm1_list, "Backups", &backups_sub, backups_tapped);

  // An unfinished install, or a store that can't be read: instead of the list.
  fm1_blocked = column(t.content, 6);
  fm1_problem = problem_view(fm1_blocked, true);
  lv_obj_add_flag(fm1_blocked, LV_OBJ_FLAG_HIDDEN);
  fm1_foot = foot_box(t);
  hold_widget(fm1_foot, fm1_hold, kRed, run_loader_hold);
  fm1_hold.act = UiAct::Retry;
  lv_obj_add_flag(fm1_foot, LV_OBJ_FLAG_HIDDEN);

  // The plan: what holding "switch to X" does.
  Page p = sub_page(&plan_title, nullptr, true);
  plan_screen = p.scr;
  for (int i = 0; i < kPlanLines; ++i) {
    plan_lines[i] = text(p.content, "", kWhite, Role::Body);
    plan_steps[i] = step_row(p.content);
  }
  hold_widget(foot_box(p), plan_hold, kYellow, run_loader_hold);
  plan_hold.act = UiAct::Switch;

  // Backups: reached from the quiet row at the bottom of the list.
  lv_obj_t *backups_title;
  Page b = sub_page(&backups_title, nullptr, true);
  backups_screen = b.scr;
  lv_label_set_text(backups_title, "Backups");
  text(b.content, "A switch backs up by itself when the FM-1 changed since its last backup. You don't need to.",
       kWhite, Role::Body);
  text(b.content, "A backup reads all 1 MB of the FM-1 twice and keeps it on this board. It writes nothing to the FM-1.",
       kGrey, Role::Detail);
  backups_latest = text(b.content, "", kGrey, Role::Detail);
  text(b.content, "Back up after you change songs or settings, before a switch, or before trying something risky.",
       kMuted, Role::Detail);
  hold_widget(foot_box(b), backups_hold, kYellow, run_loader_hold);
  backups_hold.act = UiAct::Backup;
}

// ---------- Results, and the job page ----------
static lv_obj_t *result_screen, *result_title, *result_back, *result_foot, *result_ok, *result_continue;
static ProblemView result_problem;
static uint32_t shown_result_seq;
static lv_obj_t *job_screen, *job_title, *job_danger, *job_bar, *job_note;
static StepRow job_steps[kJobSteps];

static void result_done(lv_event_t *) { go_section(kFm1); }
// Continue: a tap, since the page itself is what the user agrees to.
static int continue_index = -1;
static void continue_tapped(lv_event_t *) {
  if (hooks.loader_act) hooks.loader_act(UiAct::Continue, continue_index);
}

static void build_results() {
  Page r = sub_page(&result_title, &result_back, true);
  result_screen = r.scr;
  lv_obj_remove_event_cb(result_back, back_tapped);
  lv_obj_add_event_cb(result_back, result_done, LV_EVENT_CLICKED, nullptr);
  result_problem = problem_view(r.content, false);
  result_foot = foot_box(r);
  hold_widget(result_foot, result_hold, kYellow, run_loader_hold);
  result_ok = plain_button(result_foot, "OK", result_done);
  result_continue = plain_button(result_foot, "Continue", continue_tapped);
  lv_obj_set_style_opa(result_continue, LV_OPA_40, LV_STATE_DISABLED);  // until the Loader's preview agrees

  lv_obj_t *back_row;
  Page j = sub_page(&job_title, &back_row, false);
  job_screen = j.scr;
  job_danger = card(j.content, kDangerBg, kDangerEdge);
  text(job_danger, LV_SYMBOL_WARNING " Don't unplug or switch off the FM-1", kWhite, Role::Detail);
  for (StepRow &s : job_steps) s = step_row(j.content);
  job_bar = lv_bar_create(j.content);
  lv_obj_set_size(job_bar, lv_pct(100), 8);
  lv_obj_set_style_bg_color(job_bar, c(kDim), LV_PART_MAIN);
  lv_obj_set_style_bg_color(job_bar, c(kYellow), LV_PART_INDICATOR);
  lv_bar_set_range(job_bar, 0, 100);
  job_note = text(j.content, "", kMuted, Role::Detail);
}

static bool fix_enabled(const UiLoader &l, const UiResult &r) {
  switch (r.fix) {
    case UiAct::Switch: return l.plan.can_act;
    case UiAct::Continue: return l.can_continue;
    case UiAct::Backup: return l.can_back_up;
    case UiAct::Retry: return l.can_retry;
    default: return false;
  }
}

void ui_update_loader(const UiLoader &l) {
  if (!has_fm1() || !section_screens[kFm1]) return;
  if (&l != &last_loader) last_loader = l;
  have_loader = true;
  dot_fm1 = l.unfinished;
  // A hold that fired but started no job (the Loader refused it): ready again.
  if (!l.job.active)
    for (Hold *h : {&plan_hold, &backups_hold, &fm1_hold, &result_hold}) h->fired = false;
  show_dots();

  // The FM-1 page: the list, or what stands in its way.
  const bool blocked = l.blocked.headline[0];
  show_if(fm1_list, !blocked);
  show_if(fm1_blocked, blocked);
  show_if(fm1_foot, blocked && l.blocked.fix == UiAct::Retry);
  if (blocked) {
    fill_problem(fm1_problem, l.blocked);
    hold_set(fm1_hold, l.blocked.fix_label, l.can_retry && !l.job.active);
  }
  for (int i = 0; i < kLoaderItems; ++i) {
    ItemRow &r = item_rows[i];
    show_if(r.row, i < l.item_count);
    if (i >= l.item_count) continue;
    const UiLoaderItem &it = l.items[i];
    const bool running = it.note == UiNote::Running;
    set_text(r.name, it.label);
    show_if(r.mark, running);
    set_bg(r.row, running ? kRunningRow : kRow);
    set_text(r.note, it.note == UiNote::Running ? "running" : it.note == UiNote::YourSongs ? "your songs"
                     : it.note == UiNote::NotYet  ? "not yet" : "");
    set_colour(r.note, running ? kGreen : kMuted);
    show_if(r.note, it.note != UiNote::None);
  }
  set_text(backups_sub, l.backups_note);
  set_text(backups_latest, l.backups_note);

  // The plan, once the Loader has made it for the row that was opened.
  const bool planned = l.plan.item >= 0 && l.plan.item == wanted_plan;
  const int lines = planned ? l.plan.line_count : 1;
  set_text(plan_title, planned ? l.plan.title : wanted_plan >= 0 && wanted_plan < l.item_count ? l.items[wanted_plan].label : "");
  for (int i = 0; i < kPlanLines; ++i) {
    // The first and last lines are plain text; the ones between, the steps it runs.
    const bool shown = i < lines;
    const bool warn = planned && i >= l.plan.warn_from;
    const bool as_step = shown && !warn && i > 0 && i + 1 < lines;
    show_if(plan_lines[i], shown && !as_step);
    show_if(plan_steps[i].row, as_step);
    const char *s = planned ? l.plan.lines[i] : "Planning...";
    if (as_step) {
      fill_step(plan_steps[i], UiStepState::Next, s);
    } else if (shown) {
      set_text(plan_lines[i], s);
      set_colour(plan_lines[i], warn ? kOrange : i == 0 ? kWhite : kMuted);
    }
  }
  plan_hold.index = l.plan.item;
  hold_set(plan_hold, planned ? l.plan.hold : "switch", planned && l.plan.can_act && !l.job.active);
  hold_set(backups_hold, "back up now", l.can_back_up && !l.job.active);

  // The result page.
  const UiResult &r = l.result;
  set_text(result_title, r.headline);
  set_colour(result_title, headline_hex(r));
  fill_problem(result_problem, r);
  const bool danger = r.safety == UiSafety::Danger;
  // The FM-1 is in ROM boot: the retry is the way on, so no way back.
  show_if(lv_obj_get_child(result_back, 0), !danger);
  if (danger) lv_obj_remove_flag(result_back, LV_OBJ_FLAG_CLICKABLE);
  else lv_obj_add_flag(result_back, LV_OBJ_FLAG_CLICKABLE);
  if (danger == lv_obj_has_flag(result_back, kFocusable)) {
    if (danger) lv_obj_remove_flag(result_back, kFocusable);
    else lv_obj_add_flag(result_back, kFocusable);
  }
  show_if(result_hold.button, r.fix != UiAct::None && r.fix != UiAct::Continue);
  show_if(result_ok, r.fix == UiAct::None);
  show_if(result_continue, r.fix == UiAct::Continue);
  continue_index = l.plan.item;
  if (fix_enabled(l, r) && !l.job.active) lv_obj_remove_state(result_continue, LV_STATE_DISABLED);
  else lv_obj_add_state(result_continue, LV_STATE_DISABLED);
  result_hold.act = r.fix;
  result_hold.index = r.fix == UiAct::Switch ? l.plan.item : -1;
  lv_obj_set_style_border_color(result_hold.button, c(danger ? kRed : kYellow), 0);
  lv_obj_set_style_bg_color(result_hold.bar, c(danger ? kRed : kYellow), LV_PART_INDICATOR);
  hold_set(result_hold, r.fix_label, fix_enabled(l, r) && !l.job.active);

  // The job page.
  const UiJob &j = l.job;
  set_text(job_title, j.title);
  set_colour(job_title, kYellow);
  show_if(job_danger, j.danger);
  for (int i = 0; i < kJobSteps; ++i) {
    show_if(job_steps[i].row, i < j.step_count);
    if (i < j.step_count) fill_step(job_steps[i], j.steps[i].state, j.steps[i].text);
  }
  lv_bar_set_value(job_bar, j.percent, LV_ANIM_OFF);
  set_text(job_note, j.note);
  show_if(job_note, j.note[0]);

  // Which page: a running job takes the whole screen; after it, a new result
  // opens once, and its OK returns to the list.
  if (j.active && !job_showing) {
    job_showing = true;
    depth = 0;
    show(job_screen);
  } else if (!j.active && job_showing) {
    job_showing = false;
    if (r.seq == shown_result_seq) go_section(kFm1);
  } else if (!j.active && lv_screen_active() == switching_screen && !(r.seq && r.seq != shown_result_seq)) {
    go_section(kPlay);  // the boot's job never started, and no result opens: Play
  }
  if (!j.active && r.seq && r.seq != shown_result_seq) {
    shown_result_seq = r.seq;
    go_section(kFm1);
    push(result_screen);
  }
  if (lv_screen_active() != job_screen) refresh_group();
}

// ---------- Setup and Computer mode ----------
static lv_obj_t *size_buttons[3], *version_label, *computer_screen_confirm;
static lv_obj_t *network_labels[3];  // Setup, the Computer mode screen, Remote diagnostics
static char network_text[48] = "";
// The setup hotspot as a QR code and a hint, under Setup's network line, only
// while it's open (ui_set_join_qr): made then and deleted after, so it costs no
// RAM the rest of the time.
static lv_obj_t *join_qr;
static char join_qr_text[64] = "";
// Computer mode's line about a firmware the page is adding (ui_set_computer_status).
static char computer_status[64] = "";
static lv_obj_t *computer_status_label;

static void rebuild(void *);
static void size_tapped(lv_event_t *e) {
  const uint8_t picked = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
  if (picked == settings.text_size) return;
  settings.text_size = picked;
  if (hooks.settings_changed) hooks.settings_changed(settings);
  lv_async_call(rebuild, nullptr);  // not from inside the button's own event
}

// One small wrapping line, hidden while empty (ui_set_network).
static lv_obj_t *network_label(lv_obj_t *parent) {
  lv_obj_t *l = text(parent, network_text, kMuted, Role::Detail);
  show_if(l, network_text[0]);
  return l;
}

// Black on white with a quiet zone, the way phone cameras read best: 37 modules
// (the hotspot's text, version 3 or 4 by the board's name, and the quiet zone)
// at 4 px, or 3 px on a narrow screen so all of it shows without scrolling.
// Beside it, what to do when the phone doesn't open the setup page by itself
// (iOS doesn't always): the hotspot's address, the ESP32 default.
static void place_join_qr() {
  if (join_qr) lv_obj_delete(join_qr);
  join_qr = nullptr;
  lv_obj_t *line = network_labels[0];
  if (!join_qr_text[0] || !line) return;
  join_qr = row(lv_obj_get_parent(line), 8);
  lv_obj_t *qr = lv_qrcode_create(join_qr);
  lv_qrcode_set_size(qr, board->width >= 320 ? 148 : 111);
  lv_qrcode_set_dark_color(qr, c(0x000000));
  lv_qrcode_set_light_color(qr, c(0xFFFFFF));
  lv_qrcode_set_quiet_zone(qr, true);
  lv_qrcode_update(qr, join_qr_text, strlen(join_qr_text));
  lv_obj_t *hint = text(join_qr, "Scan to join. No setup page? Open 192.168.4.1", kMuted, Role::Detail);
  lv_obj_set_width(hint, 1);
  lv_obj_set_flex_grow(hint, 1);
  lv_obj_move_to_index(join_qr, lv_obj_get_index(line) + 1);
}

static void computer_tapped(lv_event_t *) { push(computer_screen_confirm); }
static void run_computer_hold(Hold &) {
  if (hooks.connect_computer) hooks.connect_computer();
}

// Remote diagnostics: its own page, one button, since turning it on or off restarts the board.
static lv_obj_t *diag_screen;
static void diag_tapped(lv_event_t *) { push(diag_screen); }
static void diag_toggle_tapped(lv_event_t *) {
  if (hooks.set_diagnostics) hooks.set_diagnostics(!settings.diagnostics);
}

static void build_diag_page() {
  lv_obj_t *title;
  Page p = sub_page(&title, nullptr, true);
  diag_screen = p.scr;
  lv_label_set_text(title, "Remote diagnostics");
  if (settings.diagnostics) {
    text(p.content, "On: this board's status, log and crashes are on a read-only page on your Wi-Fi.", kWhite,
         Role::Body);
    network_labels[2] = network_label(p.content);
  } else {
    text(p.content, "Off: no Wi-Fi. On, this board's status, log and crashes go on a read-only page on your Wi-Fi.",
         kWhite, Role::Body);
    text(p.content, "The first time, it starts a hotspot to join your Wi-Fi.", kGrey, Role::Detail);
  }
  plain_button(foot_box(p), settings.diagnostics ? "Turn off (restarts)" : "Turn on (restarts)", diag_toggle_tapped);
}

static void build_setup() {
  Page t = section_page(kSetup);
  version_label = text(t.content, firmware, kGrey, Role::Body);
  network_labels[0] = network_label(t.content);
  join_qr = nullptr;  // went with the old Setup (rebuild)
  place_join_qr();
  // Text size: three buttons sharing the width, each lettered in the size it
  // picks, so the letters are the preview.
  lv_obj_t *sizes = row(t.content, 6);
  lv_obj_set_height(sizes, 40);
  static const char *const kSizes[] = {"S", "M", "L"};
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *b = size_buttons[i] = box(sizes);
    lv_obj_set_height(b, lv_pct(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, c(i == settings.text_size ? kBlue : kDim), 0);
    pressed_bg(b, 0x5A5A5A);
    focusable(b, Ring::Outline);
    // Each letter in its size's body text, so the letters are the preview.
    lv_obj_t *l = word(b, kSizes[i], kWhite, Role::Body);
    lv_obj_set_style_text_font(l, font_at(i, Role::Body), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, size_tapped, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
  }
  // Rotate screen comes with the first board that can turn (a rect panel).
  lv_obj_t *sub;
  nav_row(t.content, LV_SYMBOL_USB " Computer mode", &sub, computer_tapped);
  lv_label_set_text(sub, "For updates over USB");
  if (hooks.set_diagnostics) {
    nav_row(t.content, LV_SYMBOL_WIFI " Remote diagnostics", &sub, diag_tapped);
    lv_label_set_text(sub, settings.diagnostics ? "On" : "Off");
    build_diag_page();
  }

  // Computer mode: its own page, and a hold, since it stops MIDI.
  lv_obj_t *title;
  Page p = sub_page(&title, nullptr, true);
  computer_screen_confirm = p.scr;
  lv_label_set_text(title, "Computer mode");
  text(p.content, "Restarts this board with MIDI off, so a computer can update it over USB.", kWhite, Role::Body);
  text(p.content, "Open the installer page in Chrome or Edge. After an update it goes back to MIDI by itself.", kGrey,
       Role::Detail);
  hold_widget(foot_box(p), computer_hold, kYellow, run_computer_hold);
  hold_set(computer_hold, "restart in Computer mode", true);
}

static void back_to_host_tapped(lv_event_t *) {
  if (hooks.back_to_host) hooks.back_to_host();
}

// The board in Computer mode: the USB host isn't running, so no nav; one way back.
static lv_obj_t *build_computer_screen(const char *info) {
  lv_obj_t *title;
  Page t = sub_page(&title, nullptr, false);
  lv_label_set_text(title, LV_SYMBOL_USB " Computer mode");
  set_colour(title, kYellow);
  text(t.content, "MIDI is off. Plug this board into a computer and open the installer in Chrome or Edge.", kWhite,
       Role::Body);
  computer_status_label = text(t.content, computer_status, kYellow, Role::Body);
  show_if(computer_status_label, computer_status[0]);
  text(t.content, info, kGrey, Role::Detail);
  network_labels[1] = network_label(t.content);
  plain_button(foot_box(t), "Back to MIDI", back_to_host_tapped);
  return t.scr;
}

// Safe mode: not Computer mode, though the port goes to the computer here too
// (so a crash loop can be reflashed). Its own title, so a reset mid-install
// doesn't read as "it switched me into Computer mode" (2026-11-05).
static lv_obj_t *build_safe_mode_screen(const char *info) {
  lv_obj_t *title;
  Page t = sub_page(&title, nullptr, false);
  lv_label_set_text(title, LV_SYMBOL_WARNING " Safe mode");
  set_colour(title, kRed);
  text(t.content, "A fault stopped the board, so MIDI is off. Where:", kWhite, Role::Body);
  text(t.content, info, kGrey, Role::Detail);
  network_labels[1] = network_label(t.content);  // /crash and /coredump are on the Web UI
  plain_button(foot_box(t), "Restart", back_to_host_tapped);
  return t.scr;
}

static lv_obj_t *build_switching_screen(const char *title_text) {
  lv_obj_t *title;
  Page t = sub_page(&title, nullptr, false);
  lv_label_set_text(title, title_text);
  set_colour(title, kYellow);
  text(t.content, "The board restarts during a job. That's normal.", kWhite, Role::Body);
  text(t.content, "Don't unplug or switch off the FM-1.", kYellow, Role::Body);
  return t.scr;
}

// ---------- Building it all ----------
static void build_all() {
  for (lv_obj_t *&s : section_screens) s = nullptr;
  memset(nav, 0, sizeof nav);
  build_play();
  if (has_fm1()) {
    build_fm1();
    build_results();
  }
  build_setup();
  fitted_play = UINT32_MAX;
  if (have_frame) ui_update(last_frame);
  if (have_loader) ui_update_loader(last_loader);
}

// A new text size: every page again, at the new size, on the same section.
static void rebuild(void *) {
  lv_obj_t *old[] = {section_screens[kPlay], section_screens[kFm1], section_screens[kSetup], plan_screen,
                     backups_screen,        result_screen,        job_screen,             computer_screen_confirm,
                     diag_screen};
  plan_screen = backups_screen = result_screen = job_screen = diag_screen = nullptr;
  network_labels[2] = nullptr;  // it was on the diagnostics page
  const int was = section;
  const bool job = job_showing;
  build_all();
  if (job) show(job_screen);
  else go_section(was);
  for (lv_obj_t *o : old)
    if (o) lv_obj_delete(o);
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

  lv_indev_t *in = lv_indev_create();
  if (b.input == InputKind::Touch) {
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_cb);
  } else {
    lv_indev_set_type(in, b.input == InputKind::Encoder ? LV_INDEV_TYPE_ENCODER : LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(in, b.input == InputKind::Encoder ? encoder_cb : keys_cb);
    group = lv_group_create();
    lv_indev_set_group(in, group);
  }
  // Read the input every 10 ms, not once a frame (33 ms): a quick tap lasts
  // under 100 ms, and every sample it misses makes the screen feel late.
  lv_timer_set_period(lv_indev_get_read_timer(in), 10);
#if UI_TOUCH_PROBE
  // Diagnostic build: a dot where LVGL thinks the finger is.
  lv_obj_t *dot = lv_obj_create(lv_layer_sys());
  lv_obj_set_size(dot, 12, 12);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, c(kRed), 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_indev_set_cursor(in, dot);
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
  draw_icon(0, midi_shape);
  draw_icon(1, cart_shape);

  build_all();
  go_section(kPlay);
  return true;
}

void ui_show_computer_mode(const char *info) {
  depth = 0;
  show(build_computer_screen(info));
}

void ui_show_safe_mode(const char *info) {
  depth = 0;
  show(build_safe_mode_screen(info));
}

void ui_show_switching(const char *title) {
  depth = 0;
  if (switching_screen) lv_obj_delete(switching_screen);
  switching_screen = build_switching_screen(title);
  show(switching_screen);
  lv_refr_now(nullptr);  // now, before the store mounts and the USB host starts
}

void ui_set_network(const char *line) {
  if (strcmp(line, network_text) == 0) return;
  strlcpy(network_text, line, sizeof network_text);
  for (lv_obj_t *l : network_labels) {
    if (!l) continue;
    lv_label_set_text(l, network_text);
    show_if(l, network_text[0]);
  }
}

void ui_set_join_qr(const char *text) {
  if (strcmp(text, join_qr_text) == 0) return;
  strlcpy(join_qr_text, text, sizeof join_qr_text);
  place_join_qr();
}

void ui_set_computer_status(const char *line) {
  if (strcmp(line, computer_status) == 0) return;
  strlcpy(computer_status, line, sizeof computer_status);
  if (!computer_status_label) return;
  lv_label_set_text(computer_status_label, computer_status);
  show_if(computer_status_label, computer_status[0]);
}

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

int ui_scale_fonts(const void *fonts[], int max) {
  int n = 0;
  for (int r = 0; r < kRoles && n < max; ++r) fonts[n++] = font(Role(r));
  if (n < max) fonts[n++] = icon_font();
  for (int size = 0; size < 3 && n < max; ++size) fonts[n++] = font_at(size, Role::Body);  // Setup's S/M/L letters
  return n;
}
