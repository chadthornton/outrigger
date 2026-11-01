// `make ui-shots`: renders host_ui on the Mac (or CI) the way a board shows
// it, and checks the layout without a board.
//
// Every page of docs/ui-redesign.md on each board's screen (firmware/boards.json:
// 240x280 rounded, 320x480 upright), plus 480x320 sideways for boards to come,
// at text sizes S, M and L, in the hub and
// hub-loader builds, plus the focus ring on a board without touch. The text is
// the firmware's own: Play's statuses come from midi_core's stage_status() and
// the FM-1 pages' from loader_messages.cpp. Each visible label must be
// unclipped by its parents (unless scrolled out of a page's view), at least
// one line tall, on the glass and in the text size's type scale; lines cut
// with "..." are listed. Collapsed Play must fit at every size, and the FM-1
// stage's last line must show when it opens. Taps land with a finger that
// drifts 0, 8 and 16 px and must still work; holds fire after 2 s and not
// before.
//
// Writes <out>/<board>/<build>/<size>_<page>.png, <out>/sheet_<board>.png and
// <out>/report.txt (fits or scrolls, per shot); exits 1 on any layout problem
// or failed check. host_ui keeps state in statics that lv_deinit() doesn't
// reset, so each board x build x size runs in its own fork().
#include <lvgl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "board_shots.h"
#include "host_ui.h"
#include "loader_messages.h"
#include "png.h"

using namespace loader_messages;
using fm1_loader::DataAction;
using fm1_loader::Failure;
using fm1_loader::Step;

// ---------- The boards ----------
struct SimBoard {
  const char *id;
  int w, h, radius;
  uint8_t boost;
  InputKind input;
};
// The real boards' screens (BOARD_SHOTS, generated from firmware/boards.json),
// then shapes no board has yet: sideways, and the 1.69 driven by an encoder.
static const SimBoard kBoards[] = {BOARD_SHOTS,
                                   {"480x320", 480, 320, 0, 1, InputKind::Touch},
                                   {"240x280-encoder", 240, 280, 40, 0, InputKind::Encoder}};
constexpr int kBoardCount = sizeof kBoards / sizeof *kBoards;
static const SimBoard *sim = &kBoards[0];
static int W, H, R;
// The longest version string app.cpp makes ("v %07lx%s %s").
static const char kFirmware[] = "v 4d1d09e+ hub+L";

// ---------- The board, simulated ----------
static uint32_t now_ms = 1000;
uint32_t millis() { return now_ms; }
static uint16_t fb[480 * 480];
bool board_display_begin() { return true; }
void board_display_flush(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px) {
  for (int r = 0; r < h; ++r) memcpy(&fb[(y + r) * W + x], px + r * w, w * 2);
}
static bool finger_down = false;
static int16_t finger_x, finger_y;
bool board_touch_read(int16_t *x, int16_t *y) {
  if (!finger_down) return false;
  *x = finger_x;
  *y = finger_y;
  return true;
}
static int16_t enc_turns;
static bool enc_pressed;
static int16_t read_encoder(bool *pressed) {
  *pressed = enc_pressed;
  const int16_t t = enc_turns;
  enc_turns = 0;
  return t;
}

static void run(int ms) {
  for (int t = 0; t < ms; t += 5) {
    now_ms += 5;
    ui_tick();
  }
}
// The finger moves down by `drift` halfway through the press.
static void tap(int x, int y, int hold_ms = 80, int drift = 0) {
  finger_x = x;
  finger_y = y;
  finger_down = true;
  run(hold_ms / 2);
  finger_y = y + drift;
  run(hold_ms / 2);
  finger_down = false;
  run(100);
}
static void turn(int n) {
  enc_turns = int16_t(n);
  run(30);
}
static void press(int ms) {
  enc_pressed = true;
  run(ms);
  enc_pressed = false;
  run(60);
}

// ---------- Results, shared with the parent across fork() ----------
constexpr int kMaxShots = 400;
constexpr size_t kArena = size_t(160) << 20;  // the pictures, for the contact sheets
struct Shot {
  int board;
  char build[12], size, page[20];
  bool scrolls;
  size_t offset;
};
struct Results {
  int problems, failures, dotted, shots;
  size_t used;
  Shot shot[kMaxShots];
};
static Results *results;
static uint8_t *arena;
static char where[64];  // "320x480 hub-loader M plan", for messages

static void fail(const char *fmt, ...) {
  ++results->failures;
  va_list ap;
  va_start(ap, fmt);
  printf("  !! %s: ", where);
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
}

// ---------- Layout checks ----------
static int text_width(const char *s, const lv_font_t *font) {
  int w = 0;
  for (; *s; ++s) w += lv_font_get_glyph_width(font, uint8_t(s[0]), uint8_t(s[1]));
  return w;
}

// True if (x, y) is on the glass, at least `inset` px in from its edge.
static bool on_glass(int x, int y, int inset) {
  if (x < inset || y < inset || x >= W - inset || y >= H - inset) return false;
  if (!R) return true;
  const int cx = x < R ? R : x >= W - R ? W - R - 1 : x;
  const int cy = y < R ? R : y >= H - R ? H - R - 1 : y;
  return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= (R - inset) * (R - inset);
}

static bool scrolls(lv_obj_t *o) {
  return lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_bottom(o) > 0 || lv_obj_get_scroll_top(o) > 0);
}

static void check_labels(lv_obj_t *o) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_obj_t *ch = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_check_type(ch, &lv_label_class)) {
      const char *t = lv_label_get_text(ch);
      const lv_font_t *font = lv_obj_get_style_text_font(ch, LV_PART_MAIN);
      const lv_label_long_mode_t mode = lv_label_get_long_mode(ch);
      // DOTS mode writes the "..." into the label's own text.
      if (mode == LV_LABEL_LONG_MODE_DOTS && strstr(t, "...")) {
        ++results->dotted;
        printf("  .. %s: '%s'\n", where, t);
      }
      lv_area_t a, vis;
      lv_obj_get_coords(ch, &a);
      vis = a;
      lv_obj_t *scroller = nullptr;
      for (lv_obj_t *p = lv_obj_get_parent(ch); p; p = lv_obj_get_parent(p)) {
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        vis.x1 = LV_MAX(vis.x1, pa.x1);
        vis.y1 = LV_MAX(vis.y1, pa.y1);
        vis.x2 = LV_MIN(vis.x2, pa.x2);
        vis.y2 = LV_MIN(vis.y2, pa.y2);
        if (scrolls(p)) scroller = p;
      }
      // A label scrolled out of a page's view is fine: it scrolls back in.
      const bool off_view = scroller && vis.x2 - vis.x1 >= a.x2 - a.x1;
      const bool cut = !off_view && (vis.x2 - vis.x1 < a.x2 - a.x1 || vis.y2 - vis.y1 < a.y2 - a.y1);
      const bool wide = mode != LV_LABEL_LONG_MODE_WRAP && mode != LV_LABEL_LONG_MODE_DOTS &&
                        text_width(t, font) > lv_obj_get_content_width(ch) + 1;
      const bool short_ = lv_area_get_height(&a) < lv_font_get_line_height(font);
      // The label's text box, not its full width, must sit on the glass.
      const int tw = LV_MIN(text_width(t, font), lv_area_get_width(&a));
      const lv_text_align_t align = lv_obj_get_style_text_align(ch, LV_PART_MAIN);
      const int tx1 = align == LV_TEXT_ALIGN_RIGHT    ? a.x2 - tw
                      : align == LV_TEXT_ALIGN_CENTER ? (a.x1 + a.x2 - tw) / 2
                                                      : a.x1;
      const int tx2 = tx1 + tw, inset = R ? 3 : 2;
      const bool unsafe = !off_view && t[0] && (!on_glass(tx1, a.y1, inset) || !on_glass(tx2, a.y1, inset) ||
                                                !on_glass(tx1, a.y2, inset) || !on_glass(tx2, a.y2, inset));
      // Every label takes its font from the text size's type scale, so the
      // setting reaches it (host_ui.cpp, "Fonts").
      const void *scale[12];
      const int n = ui_scale_fonts(scale, 12);
      bool in_scale = false;
      for (int k = 0; k < n; ++k) in_scale |= scale[k] == font;
      if (!in_scale) {
        ++results->problems;
        printf("  !! %s: '%s' has a font outside the text size's scale\n", where, t);
      }
      if (cut || wide || short_ || unsafe) {
        ++results->problems;
        printf("  !! %s: '%s' x %d..%d y %d..%d%s%s%s%s\n", where, t, (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2,
               cut ? " CLIPPED" : "", wide ? " TOO-WIDE" : "", short_ ? " SHORT" : "",
               unsafe ? " OUTSIDE-SAFE-AREA" : "");
      }
    }
    check_labels(ch);
  }
}

static bool any_scrolls(lv_obj_t *o) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_obj_t *ch = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
    if (scrolls(ch) || any_scrolls(ch)) return true;
  }
  return false;
}

// The shown label whose text contains `s` (or is exactly `s`), or null.
static lv_obj_t *find_text(lv_obj_t *o, const char *s, bool exact = false) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_obj_t *ch = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_check_type(ch, &lv_label_class) &&
        (exact ? !strcmp(lv_label_get_text(ch), s) : strstr(lv_label_get_text(ch), s) != nullptr))
      return ch;
    if (lv_obj_t *r = find_text(ch, s, exact)) return r;
  }
  return nullptr;
}
static bool shows(const char *s) { return find_text(lv_screen_active(), s) != nullptr; }
// The label that says `s` is in view as it stands, not scrolled off or cut.
static bool shows_whole(const char *s) {
  lv_obj_t *l = find_text(lv_screen_active(), s);
  if (!l) return false;
  lv_area_t a, vis;
  lv_obj_get_coords(l, &a);
  vis = a;
  for (lv_obj_t *p = lv_obj_get_parent(l); p; p = lv_obj_get_parent(p)) {
    lv_area_t pa;
    lv_obj_get_coords(p, &pa);
    vis.y1 = LV_MAX(vis.y1, pa.y1);
    vis.y2 = LV_MIN(vis.y2, pa.y2);
  }
  return vis.y1 == a.y1 && vis.y2 == a.y2 && a.y2 < H;
}

// The clickable object around the label that says `s` (exactly, if one does).
static lv_obj_t *find_target(const char *s) {
  lv_obj_t *l = find_text(lv_screen_active(), s, true);
  if (!l) l = find_text(lv_screen_active(), s);
  for (lv_obj_t *p = l; p; p = lv_obj_get_parent(p))
    if (lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) return p;
  return nullptr;
}

// The screen's focusable objects in reading order (host_ui's focus group):
// the nav cells come first.
static void focusables(lv_obj_t *o, std::vector<lv_obj_t *> &out) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_obj_t *ch = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_has_flag(ch, LV_OBJ_FLAG_USER_1)) out.push_back(ch);
    focusables(ch, out);
  }
}
static lv_obj_t *nav_cell(int i) {
  std::vector<lv_obj_t *> f;
  focusables(lv_screen_active(), f);
  return i < int(f.size()) ? f[i] : nullptr;
}

static void tap_obj(lv_obj_t *o, int hold_ms = 80, int drift = 0) {
  lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);  // a page that scrolls: bring it up first
  run(20);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, hold_ms, drift);
}
// Taps the object labelled `s`; false (and a failure) if there is none.
static bool tap_on(const char *s, int hold_ms = 80, int drift = 0) {
  lv_obj_t *o = find_target(s);
  if (!o) {
    fail("nothing to tap labelled '%s'", s);
    return false;
  }
  tap_obj(o, hold_ms, drift);
  return true;
}

// Checks the active screen's labels and keeps a picture of it.
static const char *build_name;
static char size_char;
static void shoot(const char *page) {
  snprintf(where, sizeof where, "%s %s %c %s", sim->id, build_name, size_char, page);
  lv_obj_invalidate(lv_screen_active());
  run(50);
  check_labels(lv_screen_active());
  if (results->shots == kMaxShots || results->used + size_t(W) * H * 3 > kArena) {
    fail("out of room for shots");
    return;
  }
  Shot &s = results->shot[results->shots++];
  s.board = int(sim - kBoards);
  snprintf(s.build, sizeof s.build, "%s", build_name);
  s.size = size_char;
  snprintf(s.page, sizeof s.page, "%s", page);
  s.scrolls = any_scrolls(lv_screen_active());
  s.offset = results->used;
  results->used += size_t(W) * H * 3;
  // Off the glass in dark grey, so clipping shows.
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const uint16_t p = fb[y * W + x];
      const bool out = !on_glass(x, y, 0);
      uint8_t *o = &arena[s.offset + (size_t(y) * W + x) * 3];
      o[0] = out ? 40 : ((p >> 11) & 31) * 255 / 31;
      o[1] = out ? 20 : ((p >> 5) & 63) * 255 / 63;
      o[2] = out ? 40 : (p & 31) * 255 / 31;
    }
}

// ---------- Sample data ----------
static midi_core::DeviceView device(uint8_t address, const char *name, midi_core::Role role, uint8_t port, bool open) {
  midi_core::DeviceView d{};
  d.address = address;
  snprintf(d.name, sizeof d.name, "%s", name);
  d.role = role;
  d.port_id = port;
  d.is_midi = true;
  d.midi_open = open;
  return d;
}

// The chain with the FM-1 and n controllers; the third doesn't get a channel.
static bool unfinished_install;
static UiFrame frame(int controllers, bool fm1_open = true) {
  UiFrame f{};
  midi_core::HostSnapshot &s = f.snapshot;
  s.uptime_ms = 60000;
  s.hub_present = true;
  s.hub_ports = 4;
  memcpy(s.port_state, "EEEp", 4);
  s.channels_used = 8;
  s.devices[s.device_count++] = device(2, "Felucca", midi_core::Role::Target, 0x12, fm1_open);
  static const char *const kNames[] = {"Launchkey Mini", "KeyStep", "Arp 2600", "BeatStep", "Control", "Drum"};
  static const char *const kDetail[] = {"> NoteOn C#4 v127", "> CC 74 = 101", "port 1.3 no USB channel",
                                        "> Pitch +8191",     "> NoteOff A0",  "> CC 1 = 64"};
  for (int i = 0; i < controllers && s.device_count < midi_core::kMaxDevices; ++i) {
    s.devices[s.device_count++] = device(uint8_t(3 + i), kNames[i], midi_core::Role::Controller, uint8_t(0x11 + i), i != 2);
    UiDeviceLine &d = f.controllers[f.controller_count++];
    midi_core::make_tag(kNames[i], d.tag);
    snprintf(d.detail, sizeof d.detail, "%s", kDetail[i]);
    d.open = i != 2;
    d.fresh = i == 0;
  }
  f.view = midi_core::evaluate(s);
  f.sent = 123456;
  f.blocked = 12;
  midi_core::stage_status(s, f.view, {controllers ? "LAU" : nullptr, f.sent, "Skiff 1.4", unfinished_install}, f.stages);
  strcpy(f.hub_detail, "ports 1:LAU 2:FM- 3:KEY 4:-");
  strcpy(f.fm1_in, "in: LAU NoteOn C#4 v127");
  f.fm1_in_fresh = true;
  strcpy(f.fm1_own, "itself sent: CC 7 = 100");
  return f;
}

// The library as checkpoint 3 had it, Skiff running.
static UiLoader loader() {
  UiLoader l{};
  const UiLoaderItem items[] = {{"Skiff 1.4", UiNote::Running},
                                {"Felucca 1.6.1", UiNote::YourSongs},
                                {"Felucca 1.5.2", UiNote::None},
                                {"M-VAVE stock V15", UiNote::None},
                                {"SLOOP 2.8.1", UiNote::None}};
  memcpy(l.items, items, sizeof items);
  l.item_count = 5;
  strcpy(l.running, "Skiff 1.4");
  strcpy(l.backups_note, "Latest b50, first b1");
  l.can_back_up = l.can_retry = true;
  l.plan.item = -1;
  return l;
}

static void plan_for(UiLoader &l, int item, DataAction data) {
  PlanInput in{};
  in.target = l.items[item].label;
  in.running = "Skiff 1.4";
  in.backup = "b50";
  in.has_unit = in.ready = in.writes_enabled = true;
  in.preview.ok = true;
  in.preview.current.identity = "FM-1_714";
  in.preview.app_sectors = 143;
  in.preview.decision.action = data;
  l.plan.item = int8_t(item);
  describe_plan(in, l.plan);
}

static void job(UiLoader &l, JobKind kind, Step step, uint32_t done, uint32_t total, bool danger,
                Chain chain = Chain::First) {
  JobInput in = {kind, step, "Felucca 1.6.1", chain == Chain::Install ? "b51" : "b50", done, total, danger};
  in.chain = chain;
  in.stale_backup = "b50";
  describe_job(in, l.job);
}

static uint32_t result_seq;
static void result(UiLoader &l, JobOutcome o) {
  if (!o.firmware) o.firmware = "Felucca 1.6.1";
  if (!o.backup) o.backup = "b50";
  l.job = UiJob{};
  l.result.seq = ++result_seq;
  describe_result(o, l.result);
}

// ---------- One board x build x size ----------
static int computer_calls, saved_size, select_calls, last_select;
static UiAct last_act;
static int act_calls, act_index;
static void on_settings(const UiSettings &s) { saved_size = s.text_size; }
static void on_computer() { ++computer_calls; }
static void on_select(int i) {
  ++select_calls;
  last_select = i;
}
static void on_act(UiAct a, int index) {
  ++act_calls;
  last_act = a;
  act_index = index;
}

static void expect_hold(const char *label, UiAct act, int index, int drift = 8) {
  act_calls = 0;
  if (!tap_on(label, 80, drift)) return;
  if (act_calls) fail("a quick tap on '%s' ran it", label);
  if (!tap_on(label, 2200, drift)) return;
  if (act_calls != 1 || last_act != act || (index >= 0 && act_index != index))
    fail("a 2 s hold on '%s' gave %d calls (act %d, row %d)", label, act_calls, int(last_act), act_index);
}

static void play_pages(bool fm1) {
  lv_obj_t *play = lv_screen_active();
  ui_update(frame(1));
  run(100);
  shoot("play");
  if (any_scrolls(lv_screen_active())) fail("collapsed Play scrolls");
  // A tap opens a stage, a second closes it, with a finger that drifts. Where
  // the open stage makes Play scroll (L on the 1.69"), a 16 px drift is a
  // scroll, so the drift stops at 8 there.
  bool open_scrolls = false;
  for (int drift = 0; drift <= (open_scrolls ? 8 : 16); drift += 8) {
    snprintf(where, sizeof where, "%s %s %c play, drift %d px", sim->id, build_name, size_char, drift);
    if (!tap_on("Hub", 80, drift)) return;
    if (!shows("USB channels in use")) fail("a tap on Hub didn't open it");
    if (drift == 0) shoot("play_hub");
    open_scrolls = any_scrolls(lv_screen_active());
    tap_on("Hub", 80, drift);
    if (shows("USB channels in use")) fail("a second tap on Hub didn't close it");
  }
  // Five controllers, one without a USB channel: that problem opens Controllers.
  ui_update(frame(5));
  run(100);
  if (!shows("ARP  port 1.3")) fail("the controller's problem didn't open Controllers");
  shoot("play5_open");
  tap_on("Controllers");
  if (shows("ARP  port 1.3")) fail("Controllers didn't close");
  shoot("play5");
  if (fm1) {
    tap_on("FM-1");
    if (!shows("runs Skiff 1.4")) fail("the FM-1 stage doesn't say what it runs");
    shoot("play_fm1");
    // The FM-1's lines, the last one included, show on the 1.69" at every size.
    if (!shows_whole("itself sent")) fail("the FM-1 stage's last line is off the screen");
    tap_on("FM-1");
  }
  // A problem opens its own stage, with the fix card.
  ui_update(frame(1, false));
  run(100);
  if (!shows("MIDI not opened")) fail("the FM-1's problem didn't open its stage");
  shoot("problem");
  tap_on("FM-1");
  ui_update(frame(1, false));
  if (shows("MIDI not opened")) fail("a problem's stage can't be closed");
  ui_update(frame(1));
  run(100);
  if (lv_screen_active() != play) fail("Play changed page on its own");
}

static void setup_pages(int size) {
  lv_obj_t *setup = lv_screen_active();
  shoot("setup");
  tap_on("Computer mode");
  if (lv_screen_active() == setup) {
    fail("Computer mode didn't open its page");
    return;
  }
  shoot("computer_confirm");
  computer_calls = 0;
  tap_on("Hold: restart", 80, 8);
  if (computer_calls) fail("a quick tap restarted in Computer mode");
  tap_on("Hold: restart", 2200, 8);
  if (computer_calls != 1) fail("a 2 s hold gave %d restarts, not 1", computer_calls);
  tap_on("Computer mode", 80, 8);  // the back row
  if (lv_screen_active() != setup) fail("the back row didn't return to Setup");
  const char next[2] = {"SML"[(size + 1) % 3], 0};
  saved_size = -1;
  tap_on(next, 80, 8);
  run(50);
  if (saved_size != (size + 1) % 3) fail("tapping %s saved size %d", next, saved_size);
  if (!shows("Computer mode")) fail("Setup isn't showing after a new text size");
}

static void fm1_pages() {
  lv_obj_t *list = lv_screen_active();
  UiLoader l = loader();
  ui_update_loader(l);
  run(100);
  shoot("fm1_list");

  // A firmware opens its plan; the hold names what it runs.
  select_calls = 0;
  if (!tap_on("Felucca 1.6.1", 80, 8)) return;
  if (select_calls != 1 || last_select != 1) fail("the row selected %d (%d calls)", last_select, select_calls);
  shoot("plan_wait");
  plan_for(l, 1, DataAction::Restore);
  ui_update_loader(l);
  run(100);
  shoot("plan");
  expect_hold("Hold: switch to Felucca 1.6.1", UiAct::Switch, 1);
  plan_for(l, 1, DataAction::FreshStart);
  ui_update_loader(l);
  shoot("plan_fresh");
  tap_on("Felucca 1.6.1", 80, 8);  // the back row
  if (lv_screen_active() != list) fail("the plan's back row didn't return to the list");

  tap_on("Backups", 80, 8);
  shoot("backups");
  expect_hold("Hold: back up now", UiAct::Backup, -1);
  tap_on("Backups", 80, 8);

  // A job takes the whole screen.
  job(l, JobKind::Install, Step::WriteApp, 88, 149, true);
  ui_update_loader(l);
  run(100);
  if (!shows("Writing 88 of 149")) fail("the job page didn't open");
  if (nav_cell(0) && lv_obj_get_child_count(nav_cell(0)) > 1) fail("the job page has a nav");
  shoot("job_switch");
  job(l, JobKind::Backup, Step::Backup, 40, 100, false);
  ui_update_loader(l);
  shoot("job_backup");
  // A switch whose FM-1 changed: the backup it runs, then its install.
  job(l, JobKind::Backup, Step::Backup, 42, 100, false, Chain::Backup);
  ui_update_loader(l);
  if (!shows("Changed since b50")) fail("the chained backup doesn't say why");
  shoot("job_chain_backup");
  job(l, JobKind::Install, Step::WriteApp, 88, 149, true, Chain::Install);
  ui_update_loader(l);
  shoot("job_chain_write");

  // Then its result opens, once.
  JobOutcome ok{};
  ok.kind = JobKind::Install;
  ok.done = true;
  ok.sectors = 149;
  ok.data = DataAction::Restore;
  result(l, ok);
  ui_update_loader(l);
  run(100);
  if (!shows("Felucca 1.6.1 is running")) fail("the result didn't open after the job");
  shoot("switched");
  tap_on("OK", 80, 8);
  if (lv_screen_active() != list) fail("OK didn't return to the list");
  ui_update_loader(l);
  if (lv_screen_active() != list) fail("the same result opened twice");

  JobOutcome o{};
  o.kind = JobKind::Install;
  o.failure = Failure::DeviceLost;
  o.danger = o.wrote = o.pending = true;
  o.detail = "erase 0x5A000 try 1, sector 3 of 149";
  result(l, o);
  ui_update_loader(l);
  run(100);
  shoot("err_retry");
  if (shows(LV_SYMBOL_LEFT)) fail("a danger result has a way back");
  expect_hold("Hold: retry Felucca 1.6.1", UiAct::Retry, -1);

  o = JobOutcome{};
  o.kind = JobKind::Install;
  o.failure = Failure::KeyNotSent;
  o.key_stop = KeyStop::AlreadyInRom;
  o.detail = "FM-1 already in ROM boot: power-cycle it";
  result(l, o);
  plan_for(l, 1, DataAction::Restore);  // the Loader previews the failed switch again
  ui_update_loader(l);
  run(100);
  shoot("err_norom");
  expect_hold("Hold: switch to Felucca 1.6.1", UiAct::Switch, 1);

  o.failure = Failure::BackupStale;  // right after the switch's own backup
  o.chain = Chain::Install;
  o.detail = "differs from its backup at 0xE5000";
  result(l, o);
  ui_update_loader(l);
  run(100);
  shoot("err_stale");
  if (!shows("changed again")) fail("the second stale check doesn't say it changed again");
  expect_hold("Hold: switch to Felucca 1.6.1", UiAct::Switch, 1);
  tap_on("Switch didn't start", 80, 8);  // the back row
  if (lv_screen_active() != list) fail("the result's back row didn't return to the list");

  // What stands in the list's way.
  describe_not_ready(false, false, true, "Store unreadable: make flash-store", l.blocked);
  ui_update_loader(l);
  run(100);
  shoot("not_ready");
  describe_unfinished("Felucca 1.6.1", "b50", l.blocked);
  l.unfinished = true;
  ui_update_loader(l);
  run(100);
  shoot("unfinished");
  expect_hold("Hold: retry Felucca 1.6.1", UiAct::Retry, -1);
}

static void run_pass(const SimBoard &b, bool fm1, int size) {
  build_name = fm1 ? "hub-loader" : "hub";
  size_char = "SML"[size];
  snprintf(where, sizeof where, "%s %s %c", b.id, build_name, size_char);
  static BoardProfile profile;
  profile = BoardProfile{b.id, uint16_t(b.w), uint16_t(b.h), b.radius ? ScreenShape::Rounded : ScreenShape::Rect,
                         uint16_t(b.radius), b.boost};
  profile.input = b.input;
  if (b.input == InputKind::Encoder) profile.read_encoder = read_encoder;
  UiHooks hooks = {on_settings, on_computer, nullptr, fm1 ? on_select : nullptr, fm1 ? on_act : nullptr};
  ui_begin(profile, {uint8_t(size)}, hooks, kFirmware);
  ui_set_network("Web: 192.168.1.40");

  if (b.input == InputKind::Encoder) {
    // Focus moves through the nav, then each line; a press opens; a hold runs.
    UiFrame f = frame(1);
    ui_update(f);
    UiLoader l = loader();
    ui_update_loader(l);
    run(100);
    turn(1);  // from the Play icon to the FM-1 icon
    shoot("nav_focus");
    turn(2);  // past Setup to the Hub line
    shoot("play_focus");
    press(80);
    if (!shows("ports 1:LAU")) fail("an encoder press didn't open the Hub");
    turn(-2);
    press(80);  // the FM-1 icon
    if (!shows("Felucca 1.6.1")) {
      fail("an encoder press on the FM-1 icon didn't open the list");
      return;
    }
    turn(3);  // past the three icons to the first firmware
    shoot("fm1_list_focus");
    press(80);
    plan_for(l, 0, DataAction::Keep);
    ui_update_loader(l);
    run(50);
    if (!shows("Hold: switch to Skiff 1.4")) {
      fail("an encoder press didn't open the plan");
      return;
    }
    // The back row is first in the plan's order, then the hold.
    turn(1);
    act_calls = 0;
    press(80);
    if (act_calls) fail("a short press on the hold ran it");
    press(2200);
    if (act_calls != 1 || last_act != UiAct::Switch || act_index != 0) fail("a 2 s press gave %d calls", act_calls);
    return;
  }

  // Play first; in landscape the rail has the same cells as the band.
  if (fm1) ui_update_loader(loader());
  play_pages(fm1);
  int setup_cell = fm1 ? 2 : 1;
  if (fm1) {
    lv_obj_t *cell = nav_cell(1);
    if (!cell) {
      fail("no FM-1 icon");
      return;
    }
    tap_obj(cell, 80, 8);  // Play fits: a drifting finger still taps
    if (!shows("Backups")) {
      fail("the FM-1 icon didn't open the list");
      return;
    }
    fm1_pages();
  }
  lv_obj_t *cell = nav_cell(setup_cell);
  if (!cell) {
    fail("no Setup icon");
    return;
  }
  tap_obj(cell);  // the page may scroll: no drift
  if (!shows("Computer mode")) {
    fail("the Setup icon didn't open Setup");
    return;
  }
  setup_pages(size);
}

static void run_computer(const SimBoard &b) {
  build_name = "hub";
  size_char = 'M';
  BoardProfile profile{b.id, uint16_t(b.w), uint16_t(b.h), b.radius ? ScreenShape::Rounded : ScreenShape::Rect,
                       uint16_t(b.radius), b.boost};
  ui_begin(profile, {1}, {on_settings, on_computer, nullptr, nullptr, nullptr}, kFirmware);
  ui_set_network("Web: 192.168.1.40");
  // A job's boot opens on this (app.cpp); it shares this pass: neither needs the loader.
  ui_show_switching("Switching firmware");
  run(100);
  shoot("switching");
  ui_show_computer_mode("Firmware 4d1d09e+ hub+L");
  run(100);
  shoot("computer");
  ui_set_computer_status("Adding Felucca 8 Patterns 1.6.1_and_sequences_2: 40%");
  run(100);
  shoot("computer_adding");
  ui_set_computer_status("");
}

// ---------- Contact sheets ----------
// One per board: a row per build x size, a column per page; captions drawn by
// LVGL.
static const char *const kColumns[] = {
    "play",       "play_hub",  "play5",      "play5_open",  "play_fm1",   "problem",  "nav_focus", "play_focus",
    "fm1_list",   "fm1_list_focus", "plan_wait", "plan",    "plan_fresh", "backups",  "job_switch", "job_backup", "job_chain_backup", "job_chain_write",
    "switched",   "err_retry", "err_norom",  "err_stale",   "not_ready",  "unfinished", "setup",   "computer_confirm",
    "switching",  "computer", "computer_adding"};
constexpr int kCols = sizeof kColumns / sizeof *kColumns, kGap = 8, kCaption = 18;
static uint8_t caption_rgb[480 * kCaption * 3];
static int caption_w;

static void caption_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  const uint16_t *p = reinterpret_cast<const uint16_t *>(px);
  for (int y = a->y1; y <= a->y2; ++y)
    for (int x = a->x1; x <= a->x2; ++x, ++p) {
      uint8_t *o = &caption_rgb[(y * caption_w + x) * 3];
      o[0] = ((*p >> 11) & 31) * 255 / 31;
      o[1] = ((*p >> 5) & 63) * 255 / 63;
      o[2] = (*p & 31) * 255 / 31;
    }
  lv_display_flush_ready(d);
}

static int column_of(const char *page) {
  for (int i = 0; i < kCols; ++i)
    if (!strcmp(kColumns[i], page)) return i;
  return -1;
}

static bool write_sheet(const char *out, int board) {
  const SimBoard &b = kBoards[board];
  // Only the columns this board has, in order.
  bool used[kCols] = {};
  int rows = 0;
  char last_row[24] = "";
  std::vector<int> row_of(results->shots, -1);
  for (int i = 0; i < results->shots; ++i) {
    const Shot &s = results->shot[i];
    if (s.board != board) continue;
    char r[24];
    snprintf(r, sizeof r, "%s %c", s.build, s.size);
    if (strcmp(r, last_row)) {
      snprintf(last_row, sizeof last_row, "%s", r);
      ++rows;
    }
    row_of[i] = rows - 1;
    const int col = column_of(s.page);
    if (col < 0) return false;
    used[col] = true;
  }
  int col_x[kCols], cols = 0;
  for (int i = 0; i < kCols; ++i) col_x[i] = used[i] ? cols++ : -1;
  if (!rows) return true;
  const int cw = b.w + kGap, ch = b.h + kCaption + kGap, sw = cols * cw + kGap, sh = rows * ch + kGap;
  std::vector<uint8_t> sheet(size_t(sw) * sh * 3, 28);

  caption_w = b.w;
  lv_init();
  lv_tick_set_cb(millis);
  lv_display_t *d = lv_display_create(b.w, kCaption);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  static uint16_t buf[480 * kCaption];
  lv_display_set_buffers(d, buf, nullptr, sizeof buf, LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(d, caption_flush);
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x1C1C1C), 0);
  lv_obj_t *label = lv_label_create(scr);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);

  for (int i = 0; i < results->shots; ++i) {
    const Shot &s = results->shot[i];
    if (s.board != board) continue;
    char text[64];
    snprintf(text, sizeof text, "%s %c %s%s", s.build, s.size, s.page, s.scrolls ? " (scrolls)" : "");
    lv_label_set_text(label, text);
    lv_obj_invalidate(scr);
    lv_refr_now(d);
    const int x0 = kGap + col_x[column_of(s.page)] * cw, y0 = kGap + row_of[i] * ch;
    for (int y = 0; y < kCaption; ++y)
      memcpy(&sheet[(size_t(y0 + y) * sw + x0) * 3], &caption_rgb[y * b.w * 3], b.w * 3);
    for (int y = 0; y < b.h; ++y)
      memcpy(&sheet[(size_t(y0 + kCaption + y) * sw + x0) * 3], &arena[s.offset + size_t(y) * b.w * 3], b.w * 3);
  }
  lv_deinit();
  char path[512];
  snprintf(path, sizeof path, "%s/sheet_%s.png", out, b.id);
  return png::write(path, sw, sh, sheet.data());
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "build/ui-shots";
  setvbuf(stdout, nullptr, _IOLBF, 0);
  results = static_cast<Results *>(
      mmap(nullptr, sizeof(Results), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  arena = static_cast<uint8_t *>(mmap(nullptr, kArena, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (results == MAP_FAILED || arena == MAP_FAILED) return 2;

  // Each board: hub, then hub-loader, at S, M, L; then Computer mode. The
  // encoder board runs its focus checks once, at M.
  for (int bi = 0; bi < kBoardCount; ++bi) {
    const SimBoard &b = kBoards[bi];
    const bool encoder = b.input != InputKind::Touch;
    for (int pass = encoder ? 4 : 0; pass <= (encoder ? 4 : 6); ++pass) {
      fflush(stdout);
      const pid_t pid = fork();
      if (pid == 0) {
        sim = &b;
        W = b.w;
        H = b.h;
        R = b.radius;
        if (pass < 6) run_pass(b, pass >= 3, pass % 3);
        else run_computer(b);
        fflush(stdout);
        _exit(0);
      }
      int status = 0;
      waitpid(pid, &status, 0);
      if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        printf("  !! %s pass %d crashed (status %d)\n", b.id, pass, status);
        ++results->failures;
      }
    }
  }

  char path[512];
  mkdir(out, 0755);
  snprintf(path, sizeof path, "%s/report.txt", out);
  FILE *report = fopen(path, "w");
  for (int i = 0; i < results->shots; ++i) {
    const Shot &s = results->shot[i];
    const SimBoard &b = kBoards[s.board];
    snprintf(path, sizeof path, "%s/%s", out, b.id);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/%s/%s", out, b.id, s.build);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/%s/%s/%c_%s.png", out, b.id, s.build, s.size, s.page);
    if (!png::write(path, b.w, b.h, &arena[s.offset])) {
      printf("  !! can't write %s\n", path);
      return 2;
    }
    if (report) fprintf(report, "%s %s %c %s %s\n", b.id, s.build, s.size, s.page, s.scrolls ? "scrolls" : "fits");
  }
  if (report) fclose(report);
  for (int bi = 0; bi < kBoardCount; ++bi)
    if (!write_sheet(out, bi)) {
      printf("  !! can't write the sheet for %s\n", kBoards[bi].id);
      return 2;
    }
  printf("%d screens in %s (sheet_*.png): %d layout problems, %d failed checks, %d lines cut with \"...\"\n",
         results->shots, out, results->problems, results->failures, results->dotted);
  return results->problems || results->failures ? 1 : 0;
}
