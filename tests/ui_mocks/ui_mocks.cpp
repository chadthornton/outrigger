// `make ui-mocks`: renders the proposed UI redesign (docs/ui-redesign.md) with
// LVGL itself, at each board's real size (240x280 rounded, 320x480 portrait and
// landscape) and text sizes S, M and L.
// Design exploration only: these screens are built from fixed sample data and
// don't touch host_ui. Each shot is checked like ui_shots does (labels inside
// the glass's safe area, never clipped, at least a line tall), and pages that
// need to scroll are reported, so the mocks prove they fit.
//
// Writes <out>/<size>_<screen>.png (round 1, 240x280 only), <out>/nav/<board>_
// <size>_<screen>.png (round 2, the top level) and a report.txt in each; exits
// 1 on any layout problem.
#include <lvgl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../ui_shots/png.h"

// The boards the mocks render for. radius 0: a rect panel with square corners.
struct Board {
  const char *id;
  int w, h, radius, font_boost;
};
static const Board kBoards[] = {{"240x280", 240, 280, 40, 0},  // Waveshare 1.69", rounded
                                {"320x480", 320, 480, 0, 1},   // 3.5" IPS / SPI panels, portrait
                                {"480x320", 480, 320, 0, 1}};  // the same panels turned sideways
static const Board *bd = &kBoards[0];
static int W, H, kRadius, kSide, kTop;  // host_ui's safe_padding() for the board
static void use_board(const Board &b) {
  bd = &b;
  W = b.w;
  H = b.h;
  kRadius = b.radius;
  kSide = b.radius ? b.radius * 3 / 10 + 4 : 10;  // square panels: 10, not host_ui's 6, off the bezel
  kTop = b.radius ? b.radius / 3 + 4 : 8;
}
static bool landscape() { return W > H; }

static uint32_t now_ms = 1000;
static uint32_t tick() { return now_ms; }
static uint16_t fb[480 * 480];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  const uint16_t *p = reinterpret_cast<const uint16_t *>(px);
  const int w = lv_area_get_width(a);
  for (int y = a->y1; y <= a->y2; ++y) memcpy(&fb[y * W + a->x1], p + (y - a->y1) * w, w * 2);
  lv_display_flush_ready(d);
}

// Each board x size runs in its own fork() and writes its own PNGs; the
// report files collect one line per shot.
static const char *out_dir;
static int problems_found;
static char where[48];

// ---------- Fonts and colours (as host_ui) ----------
static const lv_font_t *const kSteps[] = {&lv_font_montserrat_10, &lv_font_montserrat_12, &lv_font_montserrat_14,
                                          &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20,
                                          &lv_font_montserrat_22, &lv_font_montserrat_24, &lv_font_montserrat_28};
static const lv_font_t *font_for(int step) { return kSteps[step < 0 ? 0 : step > 8 ? 8 : step]; }
static int text_size;  // 0 S, 1 M, 2 L
static int base() { return text_size * 2 + bd->font_boost; }  // S 10, M 14, L 18 px, plus the boost
// Headings: two or three steps up, but never past 20 px: at L, 24 px titles
// take a line each and push the page into scrolling.
static int heading(int up) { return base() + up < 5 ? base() + up : 5; }

static lv_color_t c(uint32_t hex) { return lv_color_hex(hex); }
static const uint32_t kWhite = 0xF2F2F2, kGrey = 0xCFCFCF, kMuted = 0x8A8A8A, kDim = 0x333333, kGreen = 0x22DD44,
                      kSpine = 0x1D6B2C, kYellow = 0xFFE14A, kOrange = 0xFFA53A, kRed = 0xFF4A4A,
                      kMagenta = 0xFF6AF0, kBlue = 0x2196F3, kWarnBg = 0x2A1508, kWarnEdge = 0x6A3A10,
                      kDangerBg = 0x3A0A0A, kDangerEdge = 0x8A1A1A, kOkBg = 0x0A2A12, kOkEdge = 0x1D6B2C;

// ---------- Building blocks ----------
static lv_obj_t *box(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_style_pad_row(o, 0, 0);
  lv_obj_set_style_pad_column(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
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
static lv_obj_t *text(lv_obj_t *parent, const char *s, uint32_t colour, int step) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(l, font_for(step), 0);
  lv_obj_set_style_text_color(l, c(colour), 0);
  lv_label_set_text(l, s);
  return l;
}
// One line that sizes to its text (for rows).
static lv_obj_t *word(lv_obj_t *parent, const char *s, uint32_t colour, int step) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font_for(step), 0);
  lv_obj_set_style_text_color(l, c(colour), 0);
  lv_label_set_text(l, s);
  return l;
}

static lv_obj_t *new_screen() {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, c(0x000000), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_hor(scr, kSide, 0);
  lv_obj_set_style_pad_ver(scr, kTop, 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 5, 0);
  return scr;
}

// The status strip on every screen: what this board is doing (the mode pill)
// and what the FM-1 runs. Fixed size: chrome doesn't zoom with text size.
struct Mode {
  const char *name;
  uint32_t colour;
};
static const Mode kPlay = {"PLAY", kGreen}, kLoad = {"LOAD", kYellow};
static void header(lv_obj_t *scr, Mode mode, const char *right, uint32_t right_colour = kGrey) {
  const int step = text_size == 2 ? 2 : 1;  // 12, 12, 14 px
  lv_obj_t *r = row(scr, 6);
  lv_obj_t *pill = box(r);
  lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(pill, c(mode.colour), 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pill, 4, 0);
  lv_obj_set_style_pad_hor(pill, 5, 0);
  lv_obj_set_style_pad_ver(pill, 1, 0);
  word(pill, mode.name, 0x000000, step);
  lv_obj_t *l = word(r, right, right_colour, step);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

// A sub-page's title row: back arrow and title. Tapping it goes back.
static void title_row(lv_obj_t *scr, const char *title, uint32_t colour = kWhite) {
  lv_obj_t *r = row(scr, 6);
  word(r, LV_SYMBOL_LEFT, kMuted, base() + 1);
  lv_obj_t *t = word(r, title, colour, heading(2));
  lv_obj_set_flex_grow(t, 1);
  lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
}

// The page body: fills what the header and footer leave, and scrolls only if
// it must (reported per shot).
static std::vector<lv_obj_t *> bodies;  // the screen's scrolling parts, for the checks
static bool is_body(lv_obj_t *o) {
  for (lv_obj_t *b : bodies)
    if (b == o) return true;
  return false;
}
static lv_obj_t *body(lv_obj_t *scr, int gap = 6) {
  lv_obj_t *b = box(scr);
  lv_obj_set_width(b, lv_pct(100));
  lv_obj_set_flex_grow(b, 1);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(b, gap, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(b, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(b, LV_SCROLLBAR_MODE_ACTIVE);
  bodies.push_back(b);
  return b;
}

// The three top-level sections, at the bottom of every top-level page.
static void tabs(lv_obj_t *scr, int active) {
  static const char *const kTabs[] = {LV_SYMBOL_AUDIO " Play", LV_SYMBOL_DRIVE " FM-1", LV_SYMBOL_SETTINGS " Setup"};
  lv_obj_t *r = row(scr, 4);
  lv_obj_set_height(r, 34);
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *b = box(r);
    lv_obj_set_height(b, lv_pct(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, c(i == active ? kBlue : 0x1E1E1E), 0);
    lv_obj_t *l = word(b, kTabs[i], i == active ? kWhite : kGrey, text_size == 0 ? 1 : 2);
    lv_obj_center(l);
  }
}

// The 2-second hold: the only way to act on the FM-1 or stop MIDI. Its label
// always names exactly what it runs.
static void hold(lv_obj_t *scr, const char *what, int percent = 0, uint32_t fill = kYellow) {
  lv_obj_t *b = box(scr);
  lv_obj_set_size(b, lv_pct(100), 44);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, c(0x3A3A2A), 0);
  lv_obj_set_style_border_color(b, c(fill), 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_clip_corner(b, true, 0);
  if (percent) {
    lv_obj_t *bar = box(b);
    lv_obj_set_size(bar, lv_pct(percent), lv_pct(100));
    lv_obj_set_style_bg_color(bar, c(fill), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, 0);
  }
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_width(l, lv_pct(94));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  // 12 px on the 1.69", so "Hold: switch to Felucca 1.6.1" stays on one line.
  lv_obj_set_style_text_font(l, font_for(W < 320 ? 1 : 2 + bd->font_boost), 0);
  lv_obj_set_style_text_color(l, c(kWhite), 0);
  lv_label_set_text_fmt(l, "Hold: %s", what);
  lv_obj_center(l);
}

static void plain_button(lv_obj_t *scr, const char *what) {
  lv_obj_t *b = box(scr);
  lv_obj_set_size(b, lv_pct(100), 40);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, c(kDim), 0);
  lv_obj_t *l = word(b, what, kWhite, text_size == 0 ? 1 : 2);
  lv_obj_center(l);
}

// A tappable row that opens a sub-page: title, one grey line under it, chevron.
static void nav_row(lv_obj_t *parent, const char *title, const char *sub, uint32_t sub_colour = kGrey) {
  lv_obj_t *r = row(parent, 6);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(r, c(0x1E1E1E), 0);
  lv_obj_set_style_radius(r, 8, 0);
  lv_obj_set_style_pad_all(r, 7, 0);
  lv_obj_t *col = column(r, 1);
  lv_obj_set_width(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(col, 1);
  text(col, title, kWhite, base() + 1);
  if (sub) text(col, sub, sub_colour, base() - 1);
  word(r, LV_SYMBOL_RIGHT, kMuted, base());
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

// The error pattern: what happened, is the FM-1 safe, what to do, then the
// technical detail small and last.
struct Problem {
  const char *what;
  bool safe;
  const char *safety;  // "Nothing was written." / "Don't unplug..."
  const char *why;
  const char *todo;
  const char *detail;
};
static void problem(lv_obj_t *parent, const Problem &p) {
  text(parent, p.what, p.safe ? kOrange : kRed, heading(2));
  lv_obj_t *s = card(parent, p.safe ? kOkBg : kDangerBg, p.safe ? kOkEdge : kDangerEdge);
  lv_obj_t *sr = row(s, 6);
  word(sr, p.safe ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, p.safe ? kGreen : kRed, base());
  lv_obj_t *st = text(sr, p.safety, kWhite, base());
  lv_obj_set_width(st, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(st, 1);
  if (p.why) text(parent, p.why, kGrey, base());
  if (p.todo) text(parent, p.todo, kWhite, base());
  if (p.detail) text(parent, p.detail, kMuted, base() - 2);
}

// One step of a running job: done, now, or to come.
enum class StepState { Done, Now, Next };
static void step(lv_obj_t *parent, StepState s, const char *what) {
  lv_obj_t *r = row(parent, 6);
  lv_obj_t *icon = word(r, s == StepState::Done ? LV_SYMBOL_OK : s == StepState::Now ? LV_SYMBOL_RIGHT : "-",
                        s == StepState::Done ? kGreen : s == StepState::Now ? kYellow : kMuted, base());
  lv_obj_set_width(icon, 16);
  lv_obj_t *t = text(r, what, s == StepState::Next ? kMuted : s == StepState::Now ? kWhite : kGrey, base());
  lv_obj_set_width(t, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(t, 1);
}

static void progress(lv_obj_t *parent, int percent) {
  lv_obj_t *bar = lv_bar_create(parent);
  lv_obj_set_size(bar, lv_pct(100), 8);
  lv_obj_set_style_bg_color(bar, c(kDim), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, c(kYellow), LV_PART_INDICATOR);
  lv_bar_set_value(bar, percent, LV_ANIM_OFF);
}

// ---------- The chain (Play tab) ----------
// As today's chain, minus the Power node (the header carries it; a power
// problem shows in the problem card), with a fit loop like host_ui's.
struct ChainNode {
  lv_obj_t *title, *dot, *line, *content;
  std::vector<lv_obj_t *> details;
};
static void chain(lv_obj_t *parent, int controllers, bool fm1_problem, lv_obj_t *fit_in = nullptr,
                  bool show_runs = false) {
  if (!fit_in) fit_in = parent;
  static const char *const kTags[] = {"LAU", "KEY", "ARP", "BSP", "CTL", "DRM"};
  static const char *const kDetail[] = {"> NoteOn C#4 v127", "> CC 74 = 101", "port 1.3 no USB channel",
                                        "> Pitch +8191",      "> NoteOff A0",  "> CC 1 = 64"};
  std::vector<ChainNode> nodes;
  auto node = [&](const char *title, uint32_t colour, uint32_t dot, bool last) -> ChainNode & {
    ChainNode n;
    lv_obj_t *r = row(parent, 8);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *rail = box(r);
    lv_obj_set_size(rail, 12, lv_pct(100));
    lv_obj_add_flag(rail, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    n.dot = box(rail);
    lv_obj_set_style_radius(n.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(n.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(n.dot, c(dot), 0);
    n.line = box(rail);
    lv_obj_set_width(n.line, 2);
    lv_obj_set_flex_grow(n.line, 1);
    lv_obj_set_style_bg_opa(n.line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(n.line, c(kSpine), 0);
    if (last) lv_obj_add_flag(n.line, LV_OBJ_FLAG_HIDDEN);
    n.content = column(r);
    lv_obj_set_style_pad_left(n.content, 20, 0);
    n.title = word(n.content, title, colour, 0);
    nodes.push_back(n);
    return nodes.back();
  };
  auto detail = [&](ChainNode &n, const char *s, uint32_t colour) {
    lv_obj_t *l = word(n.content, s, colour, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_max_lines(l, 1);
    n.details.push_back(l);
  };
  ChainNode &hub = node("Hub", kWhite, kGreen, false);
  detail(hub, "1:LAU 2:FM- 3:off  USB 8/8", kGrey);
  char title[24];
  snprintf(title, sizeof title, controllers > 1 ? "Controllers (%d)" : "Controller", controllers);
  ChainNode &ctl = node(title, kYellow, kGreen, false);
  for (int i = 0; i < controllers; ++i) {
    char buf[48];
    snprintf(buf, sizeof buf, "%s  %s", kTags[i], kDetail[i]);
    detail(ctl, buf, i == 2 ? kRed : i == 0 ? kGreen : kGrey);
  }
  if (!controllers) detail(ctl, "none", kGrey);
  ChainNode &router = node("Router", kWhite, kGreen, false);
  detail(router, "123456 passed  12 blocked", kGrey);
  ChainNode &fm1 = node("FM-1", kMagenta, fm1_problem ? kRed : kGreen, true);
  if (show_runs) detail(fm1, "runs Skiff 1.4", kWhite);
  detail(fm1, fm1_problem ? "in: LAU NoteOn C#4 v127" : "in: LAU NoteOn C#4 v127", fm1_problem ? kRed : kGrey);
  if (!fm1_problem) detail(fm1, "itself sent: CC 7 = 100", kGrey);
  if (fm1_problem) {
    lv_obj_t *k = card(parent, kWarnBg, kWarnEdge);
    text(k, "FM-1 not answering", kOrange, base());
    text(k, "Replug it into hub port 2.", kWhite, base());
  }

  // Shrink until the chain fits: gaps first, then text, as host_ui does. On
  // 320 px and wider boards only the gaps tighten; past that the page scrolls.
  const int max_shrink = W >= 320 ? 3 : 8;
  for (int shrink = 0; shrink < max_shrink; ++shrink) {
    const int d = base() - (shrink > 2 ? shrink - 2 : 0);
    const lv_font_t *tf = font_for(d + 3), *df = font_for(d);
    static const int kGapEighths[] = {6, 3, 1};
    const int gap = lv_font_get_line_height(df) * kGapEighths[shrink < 2 ? shrink : 2] / 8;
    const int dot = 9 + (d > 0 ? d : 0) / 2;
    for (size_t i = 0; i < nodes.size(); ++i) {
      lv_obj_set_style_text_font(nodes[i].title, tf, 0);
      for (lv_obj_t *l : nodes[i].details) lv_obj_set_style_text_font(l, df, 0);
      lv_obj_set_style_pad_bottom(nodes[i].content, i + 1 < nodes.size() ? gap : 0, 0);
      lv_obj_set_size(nodes[i].dot, dot, dot);
      lv_obj_set_style_margin_top(nodes[i].dot, (lv_font_get_line_height(tf) - dot) / 2, 0);
    }
    lv_obj_update_layout(lv_obj_get_screen(parent));
    lv_obj_update_layout(lv_obj_get_screen(parent));
    if (lv_obj_get_scroll_bottom(fit_in) <= 0) break;
  }
}

// ---------- The screens ----------
static const char kRuns[] = "FM-1: Skiff 1.4";

static void play_chain(int controllers, bool problem_) {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, kRuns);
  lv_obj_t *b = body(scr, 0);
  tabs(scr, 0);
  lv_screen_load(scr);
  chain(b, controllers, problem_);
}

static void fm1_home() {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, kRuns);
  lv_obj_t *b = body(scr);
  lv_obj_t *k = card(b, 0x141414, kDim);
  text(k, "Runs Skiff 1.4", kWhite, heading(2));
  text(k, "Read from backup b50. Its USB name says Felucca.", kMuted, base() - 1);
  nav_row(b, LV_SYMBOL_DOWNLOAD " Back up", "Latest b50 matches it");
  nav_row(b, LV_SYMBOL_DRIVE " Change firmware", "5 in the library");
  tabs(scr, 1);
  lv_screen_load(scr);
}

// The board booted into Play with an install left unfinished (resume_pending()):
// the retry is the only action, Back up and Change firmware are gone.
static void fm1_unfinished() {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, "FM-1: install unfinished", kRed);
  lv_obj_t *b = body(scr, 4);
  problem(b, {"Install unfinished", false, "Don't unplug the FM-1 or switch it off.",
              "It's in ROM boot with half of Felucca 1.6.1 written.", nullptr, nullptr});
  hold(scr, "retry Felucca 1.6.1", 0, kRed);
  tabs(scr, 1);
  lv_screen_load(scr);
}

static void firmware_list() {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, kRuns);
  title_row(scr, "Firmware");
  lv_obj_t *b = body(scr, 4);
  struct Item {
    const char *name, *note;
    bool pinned, running;
  };
  static const Item kItems[] = {{"Skiff 1.4", "running", true, true},
                                {"Felucca 1.6.1", "save", true, false},
                                {"Felucca 1.5.2", nullptr, false, false},
                                {"M-VAVE stock V15", nullptr, false, false},
                                {"SLOOP 2.8.1", "not yet", false, false}};
  for (const Item &it : kItems) {
    lv_obj_t *r = row(b, 6);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, c(it.running ? 0x10301A : 0x1E1E1E), 0);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_pad_hor(r, 7, 0);
    lv_obj_set_style_pad_ver(r, 5, 0);
    lv_obj_t *name = text(r, it.name, kWhite, base() + 1);
    lv_obj_set_width(name, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(name, 1);
    if (it.note) word(r, it.note, it.running ? kGreen : kMuted, base() - 1);
    word(r, "*", it.pinned ? kYellow : 0x555555, heading(2));
  }
  text(b, "Store: 7.8 of 12.1 MB free", kMuted, base() - 2);
  lv_screen_load(scr);
}

static void install_confirm() {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, kRuns);
  title_row(scr, "Felucca 1.6.1");
  lv_obj_t *b = body(scr, 5);
  text(b, "Replaces Skiff 1.4: writes 143 of 143 app sectors.", kWhite, base());
  text(b, "Saves Skiff's songs first. Brings back Felucca's from its save.", kGrey, base());
  text(b, "MIDI stops until it's done.", kMuted, base() - 1);
  hold(scr, "install Felucca 1.6.1");
  lv_screen_load(scr);
}

static void backup_confirm() {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, kRuns);
  title_row(scr, "Back up");
  lv_obj_t *b = body(scr, 5);
  text(b, "Reads all 1 MB of the FM-1 twice and keeps the copy on this board.", kWhite, base());
  text(b, "Writes nothing to the FM-1. Every install needs one first: it's the undo.", kGrey, base());
  text(b, "MIDI stops until it's done.", kMuted, base() - 1);
  hold(scr, "back up this FM-1");
  lv_screen_load(scr);
}

static void job_backup() {
  lv_obj_t *scr = new_screen();
  header(scr, kLoad, "MIDI off");
  text(scr, "Backing up", kYellow, heading(3));
  lv_obj_t *b = body(scr, 3);
  step(b, StepState::Done, "Board in Load mode");
  step(b, StepState::Done, "FM-1 in ROM boot");
  step(b, StepState::Now, "Reading, 2nd pass: 42%");
  step(b, StepState::Next, "Compare and store");
  step(b, StepState::Next, "Restart the FM-1");
  progress(scr, 42);
  text(scr, "Writes nothing to the FM-1.", kMuted, base() - 1);
  lv_screen_load(scr);
}

static void job_install() {
  lv_obj_t *scr = new_screen();
  header(scr, kLoad, "MIDI off");
  text(scr, "Installing Felucca 1.6.1", kYellow, heading(2));
  lv_obj_t *k = card(scr, kDangerBg, kDangerEdge);
  text(k, LV_SYMBOL_WARNING " Don't unplug or switch off the FM-1", kWhite, base() - 1);
  lv_obj_t *b = body(scr, 3);
  step(b, StepState::Done, "Checked against b50");
  step(b, StepState::Done, "Saved Skiff's songs");
  step(b, StepState::Now, "Writing 88 of 149");
  step(b, StepState::Next, "Restart the FM-1");
  progress(scr, 59);
  lv_screen_load(scr);
}

static void stopped_retry() {
  lv_obj_t *scr = new_screen();
  header(scr, kLoad, "MIDI off");
  lv_obj_t *b = body(scr, 4);
  problem(b, {"Install stopped", false, "Don't unplug the FM-1 or switch it off.",
              "It's in ROM boot with half of Felucca 1.6.1 written.",
              "Hold to retry: it carries on where it stopped.",
              "sector 3 of 149: USB FB01 ESP_FAIL 0/16. If the retry fails: Transporter + b50."});
  hold(scr, "retry Felucca 1.6.1", 0, kRed);
  lv_screen_load(scr);
}

static void stopped_no_rom() {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, kRuns);
  title_row(scr, "Last job");
  lv_obj_t *b = body(scr, 4);
  problem(b, {"Backup didn't start", true, "Nothing was written.", "The FM-1 was already in ROM boot.",
              "Switch the FM-1 off and on, then try again.", "No ROM boot: key not sent"});
  hold(scr, "back up this FM-1");
  lv_screen_load(scr);
}

static void stopped_changed() {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, kRuns);
  title_row(scr, "Last job");
  lv_obj_t *b = body(scr, 4);
  problem(b, {"Install not started", true, "Nothing was written.",
              "The FM-1 changed since backup b50: a saved song counts.", "Back up again, then install.",
              "check read: FM-1 changed"});
  hold(scr, "back up this FM-1");
  lv_screen_load(scr);
}

static void done() {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, "FM-1: Felucca 1.6.1");
  title_row(scr, "Last job");
  lv_obj_t *b = body(scr, 4);
  text(b, "Installed Felucca 1.6.1", kGreen, heading(2));
  step(b, StepState::Done, "149 sectors written and read back");
  step(b, StepState::Done, "Skiff's songs kept in save s51");
  step(b, StepState::Done, "FM-1 restarted and answers");
  plain_button(scr, "OK");
  lv_screen_load(scr);
}

static void setup() {
  lv_obj_t *scr = new_screen();
  header(scr, kPlay, kRuns);
  lv_obj_t *b = body(scr, 6);
  text(b, "Board v 4d1d09e hub+L", kGrey, base());
  text(b, "Web: 192.168.1.40", kMuted, base() - 1);
  lv_obj_t *sizes = row(b, 6);
  lv_obj_set_height(sizes, 40);
  static const char *const kSizes[] = {"S", "M", "L"};
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *s = box(sizes);
    lv_obj_set_height(s, lv_pct(100));
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_style_radius(s, 8, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, c(i == text_size ? kBlue : kDim), 0);
    lv_obj_center(word(s, kSizes[i], kWhite, i * 2 + 3));
  }
  nav_row(b, LV_SYMBOL_USB " Computer mode", "For updates over USB");
  tabs(scr, 2);
  lv_screen_load(scr);
}

// ---------- Round 2: the top level, on several boards ----------
// No status strip: the nav is the header. In portrait it's a full-bleed band
// of three icons across the top; in landscape a rail down the left edge. It's
// part of the page, so on a page that scrolls it scrolls away with the
// content. Holds stay pinned below the scrolling part.

static int icon_size() { return W >= 320 ? 30 : 24; }
static const uint32_t kBand = 0x171717, kIconOff = 0x7A7A7A;

static lv_obj_t *dot_at(lv_obj_t *parent, int x, int y, int w, int h, uint32_t col, int radius) {
  lv_obj_t *o = box(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, c(col), 0);
  return o;
}

// Play: a 5-pin MIDI DIN socket, pins round the top, key at the bottom.
static lv_obj_t *icon_midi(lv_obj_t *parent, int s, uint32_t col) {
  lv_obj_t *o = box(parent);
  lv_obj_set_size(o, s, s);
  lv_obj_t *ring = box(o);
  lv_obj_set_size(ring, s, s);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, s >= 28 ? 3 : 2, 0);
  lv_obj_set_style_border_color(ring, c(col), 0);
  const int d = s >= 28 ? 4 : 3;
  const float r = s * 0.26f;
  for (int i = 0; i < 5; ++i) {
    const float a = 3.14159265f * (1.0f + i * 0.25f);  // left, upper left, top, upper right, right
    dot_at(o, int(lroundf(s / 2.0f + r * cosf(a) - d / 2.0f)), int(lroundf(s / 2.0f + r * sinf(a) - d / 2.0f)) + s / 10,
           d, d, col, LV_RADIUS_CIRCLE);
  }
  dot_at(o, (s - s / 4) / 2, s * 7 / 10, s / 4, s / 7, col, 1);  // the key
  return o;
}

// FM-1: a game cartridge: a wide shell with its label, and a narrower
// connector tab with contacts underneath.
static lv_obj_t *icon_cart(lv_obj_t *parent, int s, uint32_t col) {
  lv_obj_t *o = box(parent);
  lv_obj_set_size(o, s, s);
  const int bw = s >= 28 ? 3 : 2, sh = s * 64 / 100, tw = s * 66 / 100, tx = (s - tw) / 2, top = s / 12;
  lv_obj_t *tab = box(o);
  lv_obj_set_pos(tab, tx, top + sh - bw);
  lv_obj_set_size(tab, tw, s - top - sh + bw);
  lv_obj_set_style_radius(tab, 2, 0);
  lv_obj_set_style_border_width(tab, bw, 0);
  lv_obj_set_style_border_color(tab, c(col), 0);
  const int n = 3, inner = tw - 2 * (bw + 2), ch = s - top - sh - 3;
  for (int i = 0; i < n; ++i)
    dot_at(o, tx + bw + 2 + i * (inner - 2) / (n - 1), top + sh + 1, 2, ch > 2 ? ch : 2, col, 0);  // contacts
  lv_obj_t *shell = box(o);
  lv_obj_set_pos(shell, 0, top);
  lv_obj_set_size(shell, s, sh);
  lv_obj_set_style_radius(shell, 3, 0);
  lv_obj_set_style_bg_opa(shell, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(shell, c(kBand), 0);
  lv_obj_set_style_border_width(shell, bw, 0);
  lv_obj_set_style_border_color(shell, c(col), 0);
  dot_at(o, bw + 3, top + bw + 3, s - 2 * (bw + 3), sh - 2 * (bw + 3), col, 1);  // the label
  return o;
}

static lv_obj_t *icon_gear(lv_obj_t *parent, int s, uint32_t col) {
  lv_obj_t *o = box(parent);
  lv_obj_set_size(o, s, s);
  lv_obj_t *l = word(o, LV_SYMBOL_SETTINGS, col, s >= 28 ? 8 : 6);
  lv_obj_center(l);
  return o;
}

enum class Badge { None, Warn, Bad };
static int nav_focus = -1;  // the nav item a D-pad or encoder has reached, -1 none

// One nav item: the icon, a bar that marks the active one, a badge when that
// section needs attention.
static void nav_item(lv_obj_t *parent, int which, bool active, Badge badge) {
  const int s = icon_size();
  lv_obj_t *cell = box(parent);
  lv_obj_set_flex_flow(cell, landscape() ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_row(cell, 5, 0);
  lv_obj_set_style_pad_column(cell, 10, 0);
  lv_obj_t *bar = box(cell);
  if (landscape()) lv_obj_set_size(bar, 3, s + 6);
  const uint32_t col = active ? kWhite : kIconOff;
  lv_obj_t *icon = which == 0 ? icon_midi(cell, s, col) : which == 1 ? icon_cart(cell, s, col) : icon_gear(cell, s, col);
  lv_obj_set_style_pad_hor(cell, landscape() ? 0 : 8, 0);
  if (which == nav_focus) {  // an outline: drawn outside the cell, so the band doesn't move
    lv_obj_set_style_outline_width(cell, 2, 0);
    lv_obj_set_style_outline_color(cell, c(kBlue), 0);
    lv_obj_set_style_outline_pad(cell, 3, 0);
    lv_obj_set_style_radius(cell, 8, 0);
  }
  if (!landscape()) {
    lv_obj_move_to_index(bar, -1);  // under the icon
    lv_obj_set_size(bar, s, 3);
  }
  if (active) {
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, c(kBlue), 0);
    lv_obj_set_style_radius(bar, 1, 0);
  }
  if (badge != Badge::None) {
    lv_obj_t *b = dot_at(icon, s - 7, -2, 9, 9, badge == Badge::Bad ? kRed : kOrange, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, c(kBand), 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_IGNORE_LAYOUT);
  }
}

struct Top {
  lv_obj_t *scr, *page, *content, *foot;  // foot: where a pinned hold goes
};

// A top-level page: the nav, then the content, which scrolls (taking the nav
// with it in portrait) only if it must.
static Top top_page(int active, Badge play = Badge::None, Badge fm1 = Badge::None) {
  Top t;
  t.scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(t.scr, c(0x000000), 0);
  lv_obj_set_style_pad_all(t.scr, 0, 0);
  lv_obj_remove_flag(t.scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(t.scr, landscape() ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  const Badge badges[3] = {play, fm1, Badge::None};
  lv_obj_t *band;
  if (landscape()) {
    band = box(t.scr);  // the rail: bleeds off the left, top and bottom edges
    lv_obj_set_size(band, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(band, 14, 0);
    lv_obj_set_style_pad_right(band, 12, 0);
    lv_obj_set_style_pad_row(band, 22, 0);
    t.foot = column(t.scr);
    lv_obj_set_height(t.foot, lv_pct(100));
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
    lv_obj_set_style_pad_top(band, kRadius ? 9 : 10, 0);
    lv_obj_set_style_pad_bottom(band, 4, 0);
    lv_obj_set_style_pad_column(band, W >= 320 ? 34 : 18, 0);
    t.foot = t.scr;
  }
  lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(band, c(kBand), 0);
  for (int i = 0; i < 3; ++i) nav_item(band, i, i == active, badges[i]);

  lv_obj_set_flex_flow(t.page, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(t.page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(t.page, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(t.page, LV_SCROLLBAR_MODE_ACTIVE);
  bodies.push_back(t.page);
  t.content = column(t.page, 6);
  lv_obj_set_style_pad_hor(t.content, landscape() ? 12 : kSide, 0);
  lv_obj_set_style_pad_top(t.content, landscape() ? kTop + 6 : 10, 0);
  lv_obj_set_style_pad_bottom(t.content, kTop, 0);
  if (landscape()) lv_obj_set_style_pad_right(t.content, kSide + 4, 0);
  lv_screen_load(t.scr);
  return t;
}

// A hold pinned under the scrolling page.
static void pinned_hold(Top &t, const char *what, uint32_t fill) {
  lv_obj_t *f = column(t.foot);
  lv_obj_set_style_pad_hor(f, landscape() ? 12 : kSide, 0);
  if (landscape()) lv_obj_set_style_pad_right(f, kSide + 4, 0);
  lv_obj_set_style_pad_bottom(f, kTop, 0);
  hold(f, what, 0, fill);
  lv_obj_set_style_pad_bottom(t.content, 6, 0);
}

// One column on every board: landscape keeps the rail but stacks the page
// like portrait, scrolling when it must (Chad, round 3: the side-by-side
// columns read as two unrelated lists).
static void nav_play(int controllers, bool problem_, bool scrolled = false) {
  Top t = top_page(0, problem_ ? Badge::Warn : Badge::None);
  chain(t.content, controllers, problem_, t.page, true);
  if (scrolled) {
    lv_obj_update_layout(t.scr);
    lv_obj_scroll_to_y(t.page, LV_COORD_MAX, LV_ANIM_OFF);
  }
}

// Play, compact (round 4): one line per stage, its title and one status on
// the right. Tapping a line opens that stage's detail, one at a time; a
// problem opens its own stage. Every line is a hit target now that a tap on
// the chain no longer means "open Settings".
struct Stage {
  const char *title;
  uint32_t title_colour, dot;
  const char *status;
  uint32_t status_colour;
  std::vector<std::pair<const char *, uint32_t>> lines;
};

// open: the stage showing its detail (-1 none). focus: the line a D-pad or
// encoder has reached (-1 none), drawn with the focus ring.
static void chain_compact(lv_obj_t *parent, const std::vector<Stage> &stages, int open, int focus,
                          const char *problem_what = nullptr, const char *problem_do = nullptr) {
  const int line_h = W >= 320 ? 44 : 36, dot = W >= 320 ? 14 : 12;
  for (size_t i = 0; i < stages.size(); ++i) {
    const Stage &st = stages[i];
    lv_obj_t *r = row(parent, 0);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *rail = box(r);
    lv_obj_add_flag(rail, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(rail, dot, lv_pct(100));
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *d = dot_at(rail, 0, 0, dot, dot, st.dot, LV_RADIUS_CIRCLE);
    lv_obj_set_style_margin_top(d, W < 320 && text_size == 2 ? 8 : (line_h - dot) / 2, 0);
    lv_obj_t *spine = box(rail);
    lv_obj_set_width(spine, 2);
    lv_obj_set_flex_grow(spine, 1);
    if (i + 1 < stages.size()) {
      lv_obj_set_style_bg_opa(spine, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(spine, c(kSpine), 0);
    }
    lv_obj_t *col = column(r, 2);
    lv_obj_set_style_pad_left(col, dot + 8, 0);
    // On the 1.69" at L the status drops under the title; elsewhere it sits
    // on the right of the same line.
    const bool stacked = W < 320 && text_size == 2;
    lv_obj_t *head = stacked ? column(col, 0) : row(col, 8);
    if (!stacked) lv_obj_set_height(head, line_h);
    lv_obj_set_style_pad_hor(head, 6, 0);
    lv_obj_set_style_pad_ver(head, stacked ? 4 : 0, 0);
    lv_obj_set_style_radius(head, 8, 0);
    if (int(i) == open) {
      lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(head, c(0x1A1A1A), 0);
    }
    if (int(i) == focus) {
      lv_obj_set_style_border_width(head, 2, 0);  // inside the line, so nothing clips it
      lv_obj_set_style_border_color(head, c(kBlue), 0);
    }
    word(head, st.title, st.title_colour, heading(1));
    lv_obj_t *status = word(head, st.status, st.status_colour, base() - 1);
    if (!stacked) {
      lv_obj_set_width(status, 1);
      lv_obj_set_flex_grow(status, 1);
      lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
      lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_DOTS);
      lv_label_set_max_lines(status, 1);
    }
    if (W >= 320) word(head, int(i) == open ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, kIconOff, base() - 2);
    if (int(i) != open) continue;
    lv_obj_t *detail = column(col, 1);
    lv_obj_set_style_pad_left(detail, 6, 0);
    lv_obj_set_style_pad_bottom(detail, 8, 0);
    for (const auto &l : st.lines) text(detail, l.first, l.second, base() - 1);  // wraps, never cut
    if (problem_what) {
      lv_obj_t *k = card(detail, kWarnBg, kWarnEdge);
      text(k, problem_what, kOrange, base());
      text(k, problem_do, kWhite, base());
    }
  }
}

static std::vector<Stage> stages(int controllers, bool fm1_problem) {
  static const char *const kLines[] = {"LAU  > NoteOn C#4 v127", "KEY  > CC 74 = 101", "ARP  port 1.3 no USB channel",
                                       "BSP  > Pitch +8191",     "CTL  > NoteOff A0",  "DRM  > CC 1 = 64"};
  std::vector<Stage> s;
  s.push_back({"Hub", kWhite, kGreen, "USB 8/8", kYellow, {{"ports 1:LAU 2:FM- 3:off", kGrey}, {"8 of 8 USB channels in use", kYellow}}});
  Stage ctl = {controllers > 1 ? "Controllers" : "Controller", kYellow, controllers > 2 ? kOrange : kGreen,
               controllers > 2 ? "6, 1 issue" : "LAU playing", controllers > 2 ? kOrange : kGreen, {}};
  for (int i = 0; i < controllers; ++i) ctl.lines.push_back({kLines[i], i == 2 ? kRed : i == 0 ? kGreen : kGrey});
  s.push_back(ctl);
  s.push_back({"Router", kWhite, kGreen, "123k sent", kGrey, {{"123456 passed", kGrey}, {"12 SysEx blocked", kGrey}}});
  if (fm1_problem)
    s.push_back({"FM-1", kMagenta, kRed, "no answer", kOrange, {{"last in: LAU NoteOn C#4 v127", kRed}}});
  else
    s.push_back({"FM-1", kMagenta, kGreen, "Skiff 1.4", kGrey,
                 {{"runs Skiff 1.4 (backup b50)", kWhite}, {"in: LAU NoteOn C#4 v127", kGrey}, {"itself sent: CC 7 = 100", kGrey}}});
  return s;
}

static void nav_chain(int controllers, int open, int focus = -1, bool fm1_problem = false) {
  Top t = top_page(0, fm1_problem ? Badge::Warn : Badge::None);
  chain_compact(t.content, stages(controllers, fm1_problem), fm1_problem ? 3 : open, focus,
                fm1_problem ? "FM-1 not answering" : nullptr, "Replug it into hub port 2.");
}

static void nav_home() {
  Top t = top_page(1);
  lv_obj_t *k = card(t.content, 0x141414, kDim);
  text(k, "Runs Skiff 1.4", kWhite, heading(2));
  text(k, "Read from backup b50. Its USB name says Felucca.", kMuted, base() - 1);
  nav_row(t.content, LV_SYMBOL_DOWNLOAD " Back up", "Latest b50 matches it");
  nav_row(t.content, LV_SYMBOL_DRIVE " Change firmware", "5 in the library");
}

static void nav_unfinished() {
  Top t = top_page(1, Badge::None, Badge::Bad);
  problem(t.content, {"Install unfinished", false, "Don't unplug the FM-1 or switch it off.",
                      "It's in ROM boot with half of Felucca 1.6.1 written.", nullptr, nullptr});
  pinned_hold(t, "retry Felucca 1.6.1", kRed);
}

// The Play badge from another section: the chain has a problem.
static void nav_home_play_problem() {
  Top t = top_page(1, Badge::Warn);
  lv_obj_t *k = card(t.content, 0x141414, kDim);
  text(k, "Runs Skiff 1.4", kWhite, heading(2));
  text(k, "Read from backup b50. Its USB name says Felucca.", kMuted, base() - 1);
  nav_row(t.content, LV_SYMBOL_DOWNLOAD " Back up", "Latest b50 matches it");
  nav_row(t.content, LV_SYMBOL_DRIVE " Change firmware", "5 in the library");
}

static void nav_setup(bool scrolled) {
  Top t = top_page(2);
  text(t.content, "Board v 4d1d09e hub+L", kGrey, base());
  text(t.content, "Web: 192.168.1.40", kMuted, base() - 1);
  lv_obj_t *sizes = row(t.content, 6);
  lv_obj_set_height(sizes, 40);
  static const char *const kSizes[] = {"S", "M", "L"};
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *s = box(sizes);
    lv_obj_set_height(s, lv_pct(100));
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_style_radius(s, 8, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, c(i == text_size ? kBlue : kDim), 0);
    lv_obj_center(word(s, kSizes[i], kWhite, i * 2 + 3 + bd->font_boost));
  }
  if (bd->radius == 0) nav_row(t.content, LV_SYMBOL_REFRESH " Rotate screen", landscape() ? "Now: landscape" : "Now: portrait");
  nav_row(t.content, LV_SYMBOL_USB " Computer mode", "For updates over USB");
  if (scrolled) {
    lv_obj_update_layout(t.scr);
    lv_obj_scroll_to_y(t.page, LV_COORD_MAX, LV_ANIM_OFF);
  }
}

// ---------- Round 5: switching firmware is the FM-1 page ----------
// The FM-1 section opens on the firmware list. A switch checks the FM-1
// against its latest backup and writes; only if the FM-1 changed does it back
// up first, by itself (Chad's call). One hold either way; a manual backup is a
// quiet row at the bottom.

// A page under a section: no nav, a back row on top, a pinned footer.
static Top sub_page(const char *title, uint32_t colour = kWhite, bool back = true) {
  Top t;
  t.scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(t.scr, c(0x000000), 0);
  lv_obj_set_style_pad_all(t.scr, 0, 0);
  lv_obj_remove_flag(t.scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(t.scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_t *head = column(t.scr);
  lv_obj_set_style_pad_hor(head, kSide, 0);
  lv_obj_set_style_pad_top(head, kTop, 0);
  if (kRadius) lv_obj_set_style_pad_hor(head, kRadius / 2 + 4, 0);  // the title sits between the corners
  if (back) title_row(head, title, colour);
  else text(head, title, colour, heading(2));  // a running job: nowhere to go back to
  t.page = box(t.scr);
  lv_obj_set_width(t.page, lv_pct(100));
  lv_obj_set_flex_grow(t.page, 1);
  lv_obj_set_flex_flow(t.page, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(t.page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(t.page, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(t.page, LV_SCROLLBAR_MODE_ACTIVE);
  bodies.push_back(t.page);
  t.content = column(t.page, 6);
  lv_obj_set_style_pad_hor(t.content, kSide, 0);
  lv_obj_set_style_pad_top(t.content, 8, 0);
  lv_obj_set_style_pad_bottom(t.content, kTop, 0);
  t.foot = t.scr;
  lv_screen_load(t.scr);
  return t;
}

// One firmware: name, a short note on the right, tinted when it's running.
static void firmware_line(lv_obj_t *parent, const char *name, const char *note, bool running, bool focus = false) {
  lv_obj_t *r = row(parent, 8);
  lv_obj_set_height(r, W >= 320 ? 44 : 34);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(r, c(running ? 0x10301A : 0x1A1A1A), 0);
  lv_obj_set_style_radius(r, 8, 0);
  lv_obj_set_style_pad_hor(r, 8, 0);
  if (focus) {
    lv_obj_set_style_border_width(r, 2, 0);
    lv_obj_set_style_border_color(r, c(kBlue), 0);
  }
  if (running) word(r, LV_SYMBOL_PLAY, kGreen, base() - 1);
  lv_obj_t *n = word(r, name, kWhite, heading(1));
  lv_obj_set_width(n, 1);
  lv_obj_set_flex_grow(n, 1);
  lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_max_lines(n, 1);
  if (note) word(r, note, running ? kGreen : kMuted, base() - 1);
}

static void fm1_list(bool focus = false) {
  Top t = top_page(1);
  firmware_line(t.content, "Skiff 1.4", "running", true);
  firmware_line(t.content, "Felucca 1.6.1", "your songs", false, focus);
  firmware_line(t.content, "Felucca 1.5.2", nullptr, false);
  firmware_line(t.content, "M-VAVE stock V15", nullptr, false);
  firmware_line(t.content, "SLOOP 2.8.1", "not yet", false);
  lv_obj_t *gap = box(t.content);
  lv_obj_set_size(gap, 1, 4);
  nav_row(t.content, "Backups", "Latest b50; switches keep it current", kMuted);
}

static void switch_confirm(bool has_save) {
  Top t = sub_page("Felucca 1.6.1");
  text(t.content, "Switches from Skiff 1.4.", kWhite, base());
  step(t.content, StepState::Next, "Backs up only if the FM-1 changed");
  step(t.content, StepState::Next, "Saves Skiff's songs");
  step(t.content, StepState::Next, "Writes Felucca: 143 sectors");
  step(t.content, StepState::Next, has_save ? "Brings back Felucca's songs" : "Starts Felucca fresh: no songs saved yet");
  text(t.content, "MIDI stops until it's done.", kMuted, base() - 1);
  pinned_hold(t, "switch to Felucca 1.6.1", kYellow);
}

// changed: the check found the FM-1 differs from b50, so this switch backs
// up first. Otherwise it goes straight to writing.
static void switch_job(bool changed) {
  Top t = sub_page("Switching to Felucca", kYellow, false);
  if (!changed) {
    lv_obj_t *k = card(t.content, kDangerBg, kDangerEdge);
    text(k, LV_SYMBOL_WARNING " Don't unplug or switch off the FM-1", kWhite, base() - 1);
    step(t.content, StepState::Done, "Checked: same as b50");
    step(t.content, StepState::Done, "Saved Skiff's songs");
    step(t.content, StepState::Now, "Writing 88 of 149");
    step(t.content, StepState::Next, "Restart the FM-1");
    progress(t.content, 59);
    return;
  }
  step(t.content, StepState::Done, "Changed since b50: nothing written");
  step(t.content, StepState::Now, "Backing up: 2nd read, 42%");
  step(t.content, StepState::Next, "Save Skiff's songs");
  step(t.content, StepState::Next, "Write Felucca");
  step(t.content, StepState::Next, "Restart the FM-1");
  progress(t.content, 21);
}

static void backups_page() {
  Top t = sub_page("Backups");
  text(t.content, "A switch backs up by itself when the FM-1 changed since its last backup. You don't need to.", kWhite, base());
  text(t.content, "A backup reads all 1 MB of the FM-1 twice and keeps it on this board. It writes nothing to the FM-1.",
       kGrey, base() - 1);
  text(t.content, "Kept: the first (b1) and the latest (b50).", kGrey, base() - 1);
  text(t.content, "Back up by hand before you try something risky, or to copy one to a computer.", kMuted, base() - 1);
  pinned_hold(t, "back up now", kYellow);
}

// ---------- Checks and shots ----------
static int text_width(const char *s, const lv_font_t *font) {
  int w = 0;
  for (; *s; ++s) w += lv_font_get_glyph_width(font, uint8_t(s[0]), uint8_t(s[1]));
  return w;
}

// True if (x, y) is on the glass, at least `inset` px in from its edge.
static bool on_glass(int x, int y, int inset) {
  if (x < inset || y < inset || x >= W - inset || y >= H - inset) return false;
  if (!kRadius) return true;
  const int cx = x < kRadius ? kRadius : x >= W - kRadius ? W - kRadius - 1 : x;
  const int cy = y < kRadius ? kRadius : y >= H - kRadius ? H - kRadius - 1 : y;
  return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= (kRadius - inset) * (kRadius - inset);
}

static int check_labels(lv_obj_t *o) {
  int problems = 0;
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_obj_t *ch = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_check_type(ch, &lv_label_class)) {
      const char *t = lv_label_get_text(ch);
      const lv_font_t *font = lv_obj_get_style_text_font(ch, LV_PART_MAIN);
      if (strstr(t, "...")) printf("  .. %s: '%s'\n", where, t);
      lv_area_t a, vis;
      lv_obj_get_coords(ch, &a);
      vis = a;
      for (lv_obj_t *p = lv_obj_get_parent(ch); p; p = lv_obj_get_parent(p)) {
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        vis.x1 = LV_MAX(vis.x1, pa.x1);
        vis.y1 = LV_MAX(vis.y1, pa.y1);
        vis.x2 = LV_MIN(vis.x2, pa.x2);
        vis.y2 = LV_MIN(vis.y2, pa.y2);
      }
      // A label scrolled out of a page's view is fine: it scrolls back in.
      lv_obj_t *scroller = nullptr;
      for (lv_obj_t *p = lv_obj_get_parent(ch); p; p = lv_obj_get_parent(p))
        if (is_body(p)) scroller = p;
      const bool off_view = scroller &&
                            (lv_obj_get_scroll_bottom(scroller) > 0 || lv_obj_get_scroll_top(scroller) > 0) &&
                            vis.x2 - vis.x1 >= a.x2 - a.x1;
      const bool cut = !off_view && (vis.x2 - vis.x1 < a.x2 - a.x1 || vis.y2 - vis.y1 < a.y2 - a.y1);
      const bool wide = lv_label_get_long_mode(ch) != LV_LABEL_LONG_MODE_WRAP &&
                        lv_label_get_long_mode(ch) != LV_LABEL_LONG_MODE_DOTS &&
                        text_width(t, font) > lv_obj_get_content_width(ch) + 1;
      const bool short_ = lv_area_get_height(&a) < lv_font_get_line_height(font);
      // The label's text box, not its full width, must sit on the glass.
      const int tw = LV_MIN(text_width(t, font), lv_area_get_width(&a));
      const lv_text_align_t align = lv_obj_get_style_text_align(ch, LV_PART_MAIN);
      const int tx1 = align == LV_TEXT_ALIGN_RIGHT    ? a.x2 - tw
                      : align == LV_TEXT_ALIGN_CENTER ? (a.x1 + a.x2 - tw) / 2
                                                      : a.x1;
      const int tx2 = tx1 + tw, inset = kRadius ? 3 : 2;
      const bool unsafe = !off_view && (!on_glass(tx1, a.y1, inset) || !on_glass(tx2, a.y1, inset) ||
                                        !on_glass(tx1, a.y2, inset) || !on_glass(tx2, a.y2, inset));
      if (cut || wide || short_ || unsafe) {
        ++problems;
        printf("  !! %s: '%s' x %d..%d y %d..%d%s%s%s%s\n", where, t, (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2,
               cut ? " CLIPPED" : "", wide ? " TOO-WIDE" : "", short_ ? " SHORT" : "",
               unsafe ? " OUTSIDE-SAFE-AREA" : "");
      }
    }
    problems += check_labels(ch);
  }
  return problems;
}

static bool any_scrolls(lv_obj_t *o) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_obj_t *ch = lv_obj_get_child(o, i);
    if (is_body(ch) && (lv_obj_get_scroll_bottom(ch) > 0 || lv_obj_get_scroll_top(ch) > 0)) return true;
    if (any_scrolls(ch)) return true;
  }
  return false;
}

// Renders the active screen, checks it, writes <file>.png and a report line.
static void shoot(const char *file, const char *report_name, const char *label) {
  snprintf(where, sizeof where, "%s %c %s", bd->id, "SML"[text_size], label);
  lv_obj_t *scr = lv_screen_active();
  lv_obj_update_layout(scr);
  lv_obj_invalidate(scr);
  for (int t = 0; t < 50; t += 5) {
    now_ms += 5;
    lv_timer_handler();
  }
  const int problems = check_labels(scr);
  const bool scrolls = any_scrolls(scr);
  problems_found += problems;
  std::vector<uint8_t> rgb(size_t(W) * H * 3);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const uint16_t p = fb[y * W + x];
      const bool out = !on_glass(x, y, 0);
      uint8_t *o = &rgb[(size_t(y) * W + x) * 3];
      o[0] = out ? 20 : ((p >> 11) & 31) * 255 / 31;
      o[1] = out ? 20 : ((p >> 5) & 63) * 255 / 63;
      o[2] = out ? 20 : (p & 31) * 255 / 31;
    }
  char path[512];
  snprintf(path, sizeof path, "%s/%s.png", out_dir, file);
  if (!png::write(path, W, H, rgb.data())) {
    printf("  !! can't write %s\n", path);
    ++problems_found;
  }
  snprintf(path, sizeof path, "%s/%s", out_dir, report_name);
  if (FILE *f = fopen(path, "a")) {
    fprintf(f, "%s %s %d\n", label, scrolls ? "scrolls" : "fits", problems);
    fclose(f);
  }
}

static void run(const Board &b, int size) {
  use_board(b);
  text_size = size;
  lv_init();
  lv_tick_set_cb(tick);
  lv_display_t *d = lv_display_create(W, H);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  static uint16_t buf[480 * 480 / 4];
  lv_display_set_buffers(d, buf, nullptr, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(d, flush_cb);
  lv_obj_t *blank = lv_obj_create(nullptr);
  lv_screen_load(blank);
  const char sz = "SML"[size];
  char file[96], label[64];

  struct Entry {
    const char *name;
    void (*build)();
  };
  // Round 1: every page, 240x280 only, under the old status strip.
  static const Entry kRound1[] = {
      {"1_play_chain1", [] { play_chain(1, false); }},
      {"2_play_chain6", [] { play_chain(6, false); }},
      {"3_play_problem", [] { play_chain(1, true); }},
      {"4_fm1_home", fm1_home},
      {"5_firmware_list", firmware_list},
      {"6_backup_confirm", backup_confirm},
      {"7_install_confirm", install_confirm},
      {"8_job_backup", job_backup},
      {"9_job_install", job_install},
      {"10_stopped_retry", stopped_retry},
      {"11_stopped_norom", stopped_no_rom},
      {"12_stopped_changed", stopped_changed},
      {"13_done", done},
      {"14_setup", setup},
      {"15_fm1_unfinished", fm1_unfinished},
  };
  if (&b == &kBoards[0])
    for (const Entry &e : kRound1) {
      e.build();
      snprintf(file, sizeof file, "%c_%s", sz, e.name);
      snprintf(label, sizeof label, "%c %s", sz, e.name);
      shoot(file, "report.txt", label);
      lv_screen_load(blank);
      bodies.clear();
    }

  // Round 2: the top level, every board.
  static const Entry kRound2[] = {
      {"play1", [] { nav_play(1, false); }},
      {"play6", [] { nav_play(6, false); }},
      {"play6_scrolled", [] { nav_play(6, false, true); }},
      {"play_problem", [] { nav_play(1, true); }},
      {"chain", [] { nav_chain(1, -1); }},
      {"chain6", [] { nav_chain(6, -1); }},
      {"chain6_open", [] { nav_chain(6, 1); }},
      {"chain_fm1_open", [] { nav_chain(1, 3); }},
      {"chain_problem", [] { nav_chain(1, -1, -1, true); }},
      {"chain_focus", [] { nav_chain(6, -1, 2); }},
      {"chain_navfocus", [] {
         nav_focus = 1;
         nav_chain(1, -1);
         nav_focus = -1;
       }},
      {"home", nav_home},
      {"fm1_list", [] { fm1_list(); }},
      {"fm1_list_focus", [] { fm1_list(true); }},
      {"switch_confirm", [] { switch_confirm(true); }},
      {"switch_confirm_fresh", [] { switch_confirm(false); }},
      {"switch_changed", [] { switch_job(true); }},
      {"switch_writing", [] { switch_job(false); }},
      {"backups", backups_page},
      {"home_badge", nav_home_play_problem},
      {"unfinished", nav_unfinished},
      {"setup", [] { nav_setup(false); }},
      {"setup_scrolled", [] { nav_setup(true); }},
  };
  for (const Entry &e : kRound2) {
    e.build();
    snprintf(file, sizeof file, "nav/%s_%c_%s", b.id, sz, e.name);
    snprintf(label, sizeof label, "%s %c %s", b.id, sz, e.name);
    shoot(file, "nav/report.txt", label);
    lv_screen_load(blank);
    bodies.clear();
  }
}

int main(int argc, char **argv) {
  out_dir = argc > 1 ? argv[1] : "build/ui-mocks";
  setvbuf(stdout, nullptr, _IOLBF, 0);
  mkdir(out_dir, 0755);
  char path[512];
  snprintf(path, sizeof path, "%s/nav", out_dir);
  mkdir(path, 0755);
  int failed = 0;
  for (const Board &b : kBoards)
    for (int size = 0; size < 3; ++size) {
      fflush(stdout);
      const pid_t pid = fork();
      if (pid == 0) {
        run(b, size);
        fflush(stdout);
        _exit(problems_found ? 1 : 0);
      }
      int status = 0;
      waitpid(pid, &status, 0);
      if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        printf("  !! %s size %d: layout problems or a crash (status %d)\n", b.id, size, status);
        ++failed;
      }
    }
  printf("mocks in %s (report.txt, nav/report.txt): %s\n", out_dir, failed ? "layout problems" : "0 layout problems");
  return failed ? 1 : 0;
}
