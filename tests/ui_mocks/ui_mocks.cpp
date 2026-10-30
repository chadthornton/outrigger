// `make ui-mocks`: renders the proposed UI redesign (docs/ui-redesign.md) with
// LVGL itself, at the real 240x280 rounded screen and text sizes S, M and L.
// Design exploration only: these screens are built from fixed sample data and
// don't touch host_ui. Each shot is checked like ui_shots does (labels inside
// the glass's safe area, never clipped, at least a line tall), and pages that
// need to scroll are reported, so the mocks prove they fit.
//
// Writes <out>/<size>_<screen>.png and <out>/report.txt; exits 1 on any
// layout problem.
#include <lvgl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../ui_shots/png.h"

constexpr int W = 240, H = 280, kRadius = 40;
constexpr int kSide = kRadius * 3 / 10 + 4, kTop = kRadius / 3 + 4;

static uint32_t now_ms = 1000;
static uint32_t tick() { return now_ms; }
static uint16_t fb[W * H];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  const uint16_t *p = reinterpret_cast<const uint16_t *>(px);
  const int w = lv_area_get_width(a);
  for (int y = a->y1; y <= a->y2; ++y) memcpy(&fb[y * W + a->x1], p + (y - a->y1) * w, w * 2);
  lv_display_flush_ready(d);
}

// ---------- Results, shared with the parent across fork() ----------
constexpr int kMaxShots = 64;
struct Shot {
  char size, screen[24];
  bool scrolls;
  int problems;
  uint8_t rgb[W * H * 3];
};
struct Results {
  int problems, shots;
  Shot shot[kMaxShots];
};
static Results *results;
static char where[40];

// ---------- Fonts and colours (as host_ui) ----------
static const lv_font_t *const kSteps[] = {&lv_font_montserrat_10, &lv_font_montserrat_12, &lv_font_montserrat_14,
                                          &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20,
                                          &lv_font_montserrat_22, &lv_font_montserrat_24, &lv_font_montserrat_28};
static const lv_font_t *font_for(int step) { return kSteps[step < 0 ? 0 : step > 8 ? 8 : step]; }
static int text_size;  // 0 S, 1 M, 2 L
static int base() { return text_size * 2; }  // S 10, M 14, L 18 px body text
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
  lv_obj_set_style_text_font(l, font_for(text_size == 0 ? 1 : 2), 0);
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
static void chain(lv_obj_t *parent, int controllers, bool fm1_problem) {
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
  detail(fm1, fm1_problem ? "in: LAU NoteOn C#4 v127" : "in: LAU NoteOn C#4 v127", fm1_problem ? kRed : kGrey);
  if (!fm1_problem) detail(fm1, "itself sent: CC 7 = 100", kGrey);
  if (fm1_problem) {
    lv_obj_t *k = card(parent, kWarnBg, kWarnEdge);
    text(k, "FM-1 not answering", kOrange, base());
    text(k, "Replug it into hub port 2.", kWhite, base());
  }

  // Shrink until the chain fits: gaps first, then text, as host_ui does.
  for (int shrink = 0; shrink < 8; ++shrink) {
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
    if (lv_obj_get_scroll_bottom(parent) <= 0) break;
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

// ---------- Checks and shots ----------
static int text_width(const char *s, const lv_font_t *font) {
  int w = 0;
  for (; *s; ++s) w += lv_font_get_glyph_width(font, uint8_t(s[0]), uint8_t(s[1]));
  return w;
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
      // A label below a scrolling body's fold is fine: it scrolls into view.
      lv_obj_t *scroller = nullptr;
      for (lv_obj_t *p = lv_obj_get_parent(ch); p; p = lv_obj_get_parent(p))
        if (is_body(p)) scroller = p;
      const bool below_fold = scroller && lv_obj_get_scroll_bottom(scroller) > 0 && vis.x2 - vis.x1 >= a.x2 - a.x1;
      const bool cut = !below_fold && (vis.x2 - vis.x1 < a.x2 - a.x1 || vis.y2 - vis.y1 < a.y2 - a.y1);
      const bool wide = lv_label_get_long_mode(ch) != LV_LABEL_LONG_MODE_WRAP &&
                        lv_label_get_long_mode(ch) != LV_LABEL_LONG_MODE_DOTS &&
                        text_width(t, font) > lv_obj_get_content_width(ch) + 1;
      const bool short_ = lv_area_get_height(&a) < lv_font_get_line_height(font);
      const bool unsafe = !below_fold && (a.x1 < kSide - 2 || a.x2 > W - kSide + 1 || a.y1 < kTop - 2 ||
                                          a.y2 > H - kTop + 1);
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
    if (is_body(ch) && lv_obj_get_scroll_bottom(ch) > 0) return true;
    if (any_scrolls(ch)) return true;
  }
  return false;
}

static void shoot(const char *screen) {
  const char sz = "SML"[text_size];
  snprintf(where, sizeof where, "%c %s", sz, screen);
  lv_obj_t *scr = lv_screen_active();
  lv_obj_update_layout(scr);
  lv_obj_invalidate(scr);
  for (int t = 0; t < 50; t += 5) {
    now_ms += 5;
    lv_timer_handler();
  }
  Shot &s = results->shot[results->shots++];
  s.size = sz;
  snprintf(s.screen, sizeof s.screen, "%s", screen);
  s.problems = check_labels(scr);
  s.scrolls = any_scrolls(scr);
  results->problems += s.problems;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const uint16_t p = fb[y * W + x];
      const int dx = x < kRadius ? kRadius - x : x >= W - kRadius ? x - (W - kRadius - 1) : 0;
      const int dy = y < kRadius ? kRadius - y : y >= H - kRadius ? y - (H - kRadius - 1) : 0;
      const bool out = dx && dy && dx * dx + dy * dy > kRadius * kRadius;
      uint8_t *o = &s.rgb[(y * W + x) * 3];
      o[0] = out ? 20 : ((p >> 11) & 31) * 255 / 31;
      o[1] = out ? 20 : ((p >> 5) & 63) * 255 / 63;
      o[2] = out ? 20 : (p & 31) * 255 / 31;
    }
}

static void run_size(int size) {
  text_size = size;
  lv_init();
  lv_tick_set_cb(tick);
  lv_display_t *d = lv_display_create(W, H);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  static uint16_t buf[W * H / 4];
  lv_display_set_buffers(d, buf, nullptr, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(d, flush_cb);
  lv_obj_t *blank = lv_obj_create(nullptr);
  lv_screen_load(blank);

  struct Entry {
    const char *name;
    void (*build)();
  };
  static const Entry kScreens[] = {
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
  for (const Entry &e : kScreens) {
    e.build();
    shoot(e.name);
    lv_screen_load(blank);
    bodies.clear();
  }
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "build/ui-mocks";
  setvbuf(stdout, nullptr, _IOLBF, 0);
  results = static_cast<Results *>(
      mmap(nullptr, sizeof(Results), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (results == MAP_FAILED) return 2;
  for (int size = 0; size < 3; ++size) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
      run_size(size);
      fflush(stdout);
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
      printf("  !! size %d crashed (status %d)\n", size, status);
      ++results->problems;
    }
  }
  mkdir(out, 0755);
  char path[512];
  snprintf(path, sizeof path, "%s/report.txt", out);
  FILE *report = fopen(path, "w");
  for (int i = 0; i < results->shots; ++i) {
    const Shot &s = results->shot[i];
    snprintf(path, sizeof path, "%s/%c_%s.png", out, s.size, s.screen);
    if (!png::write(path, W, H, s.rgb)) return 2;
    if (report) fprintf(report, "%c %s %s %d\n", s.size, s.screen, s.scrolls ? "scrolls" : "fits", s.problems);
  }
  if (report) fclose(report);
  int scrolling = 0;
  for (int i = 0; i < results->shots; ++i) scrolling += results->shot[i].scrolls;
  printf("%d mock screens in %s: %d layout problems, %d scroll\n", results->shots, out, results->problems, scrolling);
  return results->problems ? 1 : 0;
}
