// `make ui-shots`: renders host_ui on the Mac (or CI) the way the Waveshare
// shows it, and checks the layout without a board.
//
// Every screen at text sizes S, M and L, in the hub and hub-loader builds: the
// chain with 0-6 controllers, everything Off, a problem, settings (idle and
// armed), the Firmware page (ready, busy, done, not ready) and Computer mode. Each
// visible label must be unclipped by its parents, at least one line tall and
// inside the rounded glass's safe area; lines LVGL cut with "..." are listed.
// Taps land with a finger that drifts 0, 8 and 16 px and must still work.
//
// Writes <out>/<build>/<size>_<screen>.png and <out>/sheet.png; exits 1 on
// any layout problem or failed tap. host_ui keeps state in statics that
// lv_deinit() doesn't reset, so each build x size runs in its own fork().
#include <lvgl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host_ui.h"
#include "png.h"

constexpr int W = 240, H = 280, kRadius = 40;
extern const BoardProfile kBoard = {"Waveshare 1.69 (sim)", W, H, ScreenShape::Rounded, kRadius, 0};
// host_ui's safe_padding() for this board.
constexpr int kSide = kRadius * 3 / 10 + 4, kTop = kRadius / 3 + 4;
// The longest version string app.cpp makes ("v %07lx%s %s").
static const char kFirmware[] = "v 4d1d09e+ hub+L";

// ---------- The board, simulated ----------
static uint32_t now_ms = 1000;
uint32_t millis() { return now_ms; }
static uint16_t fb[W * H];
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

// ---------- Results, shared with the parent across fork() ----------
constexpr int kMaxShots = 96;
struct Shot {
  char build[16], size, screen[16];
  uint8_t rgb[W * H * 3];
};
struct Results {
  int problems, failures, dotted, clipped_lines, shots;
  Shot shot[kMaxShots];
};
static Results *results;
static char where[48];  // "hub-loader M settings", for messages

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
static int text_width(const char *s, const lv_font_t *font, int letter_space) {
  int w = 0;
  for (; *s; ++s) w += lv_font_get_glyph_width(font, uint8_t(s[0]), uint8_t(s[1])) + letter_space;
  return w;
}

static void check_labels(lv_obj_t *o) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_obj_t *ch = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_check_type(ch, &lv_label_class)) {
      const char *text = lv_label_get_text(ch);
      const lv_font_t *font = lv_obj_get_style_text_font(ch, LV_PART_MAIN);
      const lv_label_long_mode_t mode = lv_label_get_long_mode(ch);
      // DOTS mode writes the "..." into the label's own text.
      if (mode == LV_LABEL_LONG_MODE_DOTS && strstr(text, "...")) {
        ++results->dotted;
        printf("  .. %s: '%s'\n", where, text);
      }
      if (mode == LV_LABEL_LONG_MODE_CLIP && text_width(text, font, lv_obj_get_style_text_letter_space(ch, LV_PART_MAIN)) >
                                                  lv_obj_get_content_width(ch)) {
        ++results->clipped_lines;
        printf("  .. %s: '%s' cut at the right edge\n", where, text);
      }
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
      const int line = lv_font_get_line_height(font);
      const bool cut = vis.x2 - vis.x1 < a.x2 - a.x1 || vis.y2 - vis.y1 < a.y2 - a.y1;
      const bool short_ = lv_area_get_height(&a) < line;
      // Within 2 px of the safe area, as the corners allow.
      const bool unsafe = a.x1 < kSide - 2 || a.x2 > W - kSide + 1 || a.y1 < kTop - 2 || a.y2 > H - kTop + 1;
      if (cut || short_ || unsafe) {
        ++results->problems;
        printf("  !! %s: '%s' x %d..%d y %d..%d (line %d)%s%s%s\n", where, text, (int)a.x1, (int)a.x2, (int)a.y1,
               (int)a.y2, line, cut ? " CLIPPED" : "", short_ ? " SHORT" : "", unsafe ? " OUTSIDE-SAFE-AREA" : "");
      }
    }
    check_labels(ch);
  }
}

static lv_obj_t *find_button(lv_obj_t *o, const char *text) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_obj_t *ch = lv_obj_get_child(o, i);
    if (lv_obj_check_type(ch, &lv_button_class)) {
      for (uint32_t j = 0; j < lv_obj_get_child_count(ch); ++j) {
        lv_obj_t *l = lv_obj_get_child(ch, j);
        if (lv_obj_check_type(l, &lv_label_class) && strstr(lv_label_get_text(l), text)) return ch;
      }
    }
    if (lv_obj_t *r = find_button(ch, text)) return r;
  }
  return nullptr;
}

// Taps the middle of the button labelled `text`; false if there is none.
static bool tap_button(lv_obj_t *scr, const char *text, int hold_ms = 80, int drift = 0) {
  lv_obj_t *b = find_button(scr, text);
  if (!b) {
    fail("no '%s' button", text);
    return false;
  }
  lv_area_t a;
  lv_obj_get_coords(b, &a);
  tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, hold_ms, drift);
  return true;
}

// Checks the active screen's labels and keeps a picture of it.
static void shoot(const char *build, char size, const char *screen) {
  snprintf(where, sizeof where, "%s %c %s", build, size, screen);
  lv_obj_invalidate(lv_screen_active());
  run(50);
  check_labels(lv_screen_active());
  if (results->shots == kMaxShots) {
    fail("more than %d shots", kMaxShots);
    return;
  }
  Shot &s = results->shot[results->shots++];
  snprintf(s.build, sizeof s.build, "%s", build);
  s.size = size;
  snprintf(s.screen, sizeof s.screen, "%s", screen);
  // Outside the glass's rounded corners in dark magenta, so clipping shows.
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const uint16_t p = fb[y * W + x];
      const int dx = x < kRadius ? kRadius - x : x >= W - kRadius ? x - (W - kRadius - 1) : 0;
      const int dy = y < kRadius ? kRadius - y : y >= H - kRadius ? y - (H - kRadius - 1) : 0;
      const bool out = dx && dy && dx * dx + dy * dy > kRadius * kRadius;
      uint8_t *o = &s.rgb[(y * W + x) * 3];
      o[0] = out ? 60 : ((p >> 11) & 31) * 255 / 31;
      o[1] = out ? 0 : ((p >> 5) & 63) * 255 / 63;
      o[2] = out ? 60 : (p & 31) * 255 / 31;
    }
}

// ---------- One build at one text size ----------
static int computer_calls, backup_calls, saved_size;
static void on_settings(const UiSettings &s) { saved_size = s.text_size; }
static void on_computer() { ++computer_calls; }
static void on_backup() { ++backup_calls; }  // the hold (loader_act)
static void on_select(int) {}
static void on_pin(int) {}

static UiFrame frame(int controllers) {
  UiFrame f{};
  f.view.power = f.view.hub = f.view.controllers = f.view.router = f.view.target = midi_core::Level::Ok;
  f.view.first_broken = 5;
  f.snapshot.channels_used = 8;
  strcpy(f.hub_detail, "ports 1:LAU 2:FM- 3:off 4:-");
  f.controller_count = controllers;
  static const char *const kDetail[] = {"> NoteOn C#4 v127", "> CC 74 = 101", "port 1.3 no USB channel",
                                        "> Pitch +8191",      "> NoteOff A0",  "> CC 1 = 64"};
  static const char *const kTags[] = {"LAU", "KEY", "ARP", "BSP", "CTL", "DRM"};
  for (int i = 0; i < controllers; ++i) {
    strcpy(f.controllers[i].tag, kTags[i]);
    strcpy(f.controllers[i].detail, kDetail[i]);
    f.controllers[i].open = i != 2;
    f.controllers[i].fresh = i == 0;
  }
  strcpy(f.fm1_in, "in: LAU NoteOn C#4 v127");
  strcpy(f.fm1_own, "itself sent: CC 7 = 100");
  f.sent = 123456;
  f.blocked = 12;
  return f;
}

// The Firmware page in the states loader_app.cpp shows, with its own wording
// and its longest lines.
enum class Page { Ready, Install, Retry, Busy, Done, NotReady };
static UiLoader loader_page(Page page) {
  UiLoader l{};
  const char *lines[kLoaderLines] = {"Store 7.8 of 12.1 MB free, 1 units", "Loader blob ok"};
  // The library as checkpoint 3 had it, Felucca running and Skiff pinned.
  const UiLoaderItem items[] = {{"Skiff 1.4", "FM-1_714", true},
                                {"Felucca 1.6.1", "running", false},
                                {"M-VAVE stock V15", "FM-1_015", false},
                                {"SLOOP 2.8.1", "FM-1_900", false}};
  memcpy(l.items, items, sizeof items);
  l.item_count = 4;
  l.selected = -1;
  strcpy(l.preview[0], "Reads the FM-1 twice.");
  strcpy(l.preview[1], "Writes nothing.");
  strcpy(l.action, "Hold: back up FM-1");
  switch (page) {
    case Page::Ready:
      strcpy(l.title, "Ready. Writes are on.");
      l.title_colour = 0xFFE14A;
      l.can_act = true;
      break;
    case Page::Install:
      strcpy(l.title, "Ready. Writes are on.");
      l.title_colour = 0xFFE14A;
      l.can_act = true;
      l.selected = 0;
      strcpy(l.preview[0], "Writes 143 of 143 app sectors");
      strcpy(l.preview[1], "Restores its saves (6 sectors)");
      strcpy(l.action, "Hold: install Skiff 1.4");
      break;
    case Page::Retry:
      strcpy(l.title, "Stopped: the FM-1 stopped answering");
      l.title_colour = 0xFF4A4A;
      l.can_act = true;
      strcpy(l.top_label, "Retry the Felucca 1.6.1 install");
      strcpy(l.preview[0], "An install stopped part-way.");
      strcpy(l.preview[1], "The FM-1 must still be in ROM boot.");
      strcpy(l.action, "Hold: retry the install");
      lines[2] = "erase 0x5A000 try 1, sector 3 of 149";
      lines[3] = "USB: FB01 ESP_FAIL 0/16";
      lines[4] = "DON'T UNPLUG the FM-1 or switch it off";
      lines[5] = "Hold to retry; Transporter if that fails";
      break;
    case Page::Busy:
      strcpy(l.title, "Backing up (2 reads)...");
      l.title_colour = 0xFFE14A;
      l.percent = 42;
      l.busy = l.can_act = true;
      break;
    case Page::Done:
      strcpy(l.title, "Backed up. Nothing written.");
      l.title_colour = 0x22DD44;
      l.can_act = true;
      lines[1] = "re-enumerated after ROM boot";
      lines[2] = "Unit 4a3f09c2e1 (dump's)";
      lines[3] = "Running FM-1 1.0.4";
      lines[4] = "Head 7d2c1b9e = dump";
      lines[5] = "BT+voices 0e66d1a4 differs";
      break;
    case Page::NotReady:
      strcpy(l.title, "Not ready");
      l.title_colour = 0xFF4A4A;
      lines[0] = "Store unreadable: make flash-store";
      lines[1] = "Loader blob missing";
      lines[5] = "Free RAM 96 KB internal, 8071 KB PSRAM";
      break;
  }
  for (int i = 0; i < kLoaderLines; ++i) snprintf(l.lines[i], sizeof l.lines[i], "%s", lines[i] ? lines[i] : "");
  return l;
}

static void run_build(bool loader, int size) {
  const char *build = loader ? "hub-loader" : "hub";
  const char sz = "SML"[size];
  ui_begin(kBoard, {uint8_t(size)}, {on_settings, on_computer, nullptr, loader ? on_select : nullptr, loader ? on_pin : nullptr,
                                      loader ? on_backup : nullptr}, kFirmware);
  lv_obj_t *chain = lv_screen_active();

  for (int n = 0; n <= int(midi_core::kMaxDevices); ++n) {
    ui_update(frame(n));
    run(100);
    char screen[16];
    snprintf(screen, sizeof screen, "chain%d", n);
    shoot(build, sz, screen);
  }

  UiFrame off = frame(0);
  off.view.hub = off.view.controllers = off.view.router = off.view.target = midi_core::Level::Off;
  off.view.first_broken = 1;
  strcpy(off.hub_detail, "nothing answers");
  strcpy(off.fm1_in, "not found");
  off.fm1_own[0] = 0;
  off.sent = off.blocked = 0;
  ui_update(off);
  run(100);
  shoot(build, sz, "off");

  UiFrame bad = frame(1);
  bad.view.has_problem = true;
  bad.view.target = midi_core::Level::Bad;
  bad.view.first_broken = 4;
  strcpy(bad.view.problem.line1, "FM-1 not answering");
  strcpy(bad.view.problem.line2, "Replug it into hub port 2");
  ui_update(bad);
  run(100);
  shoot(build, sz, "problem");

  ui_update(frame(1));
  run(100);
  tap(W / 2, H / 2);  // anywhere on the chain opens settings
  lv_obj_t *settings = lv_screen_active();
  snprintf(where, sizeof where, "%s %c settings", build, sz);
  if (settings == chain) {
    fail("a tap on the chain didn't open settings");
    return;
  }
  shoot(build, sz, "settings");

  for (int drift = 0; drift <= 16; drift += 8) {
    snprintf(where, sizeof where, "%s %c settings, drift %d px", build, sz, drift);
    computer_calls = 0;
    if (!tap_button(settings, "Computer", 80, drift)) return;
    if (drift == 0) shoot(build, sz, "armed");
    if (computer_calls != 0) fail("one tap on Computer restarted (%d calls)", computer_calls);
    if (!tap_button(settings, "Tap again", 80, drift)) return;
    if (computer_calls != 1) fail("two taps on Computer gave %d restarts, not 1", computer_calls);
    run(3500);  // the arm times out
    if (!find_button(settings, "Computer")) fail("Computer didn't disarm after 3 s");
    if (!tap_button(settings, "Done", 80, drift)) return;
    if (lv_screen_active() != chain) {
      fail("Done didn't return to the chain");
      return;
    }
    tap(W / 2, H / 2);
  }

  if (loader) {
    snprintf(where, sizeof where, "%s %c firmware", build, sz);
    if (!tap_button(settings, "Firmware")) return;
    lv_obj_t *firmware = lv_screen_active();
    if (firmware == settings) {
      fail("Firmware didn't open its page");
      return;
    }
    ui_update_loader(loader_page(Page::Ready));
    run(100);
    shoot(build, sz, "firmware");
    backup_calls = 0;
    tap_button(firmware, "Hold", 80, 8);
    if (backup_calls) fail("a quick tap on Hold started a backup");
    tap_button(firmware, "Hold", 2200, 8);
    if (backup_calls != 1) fail("a 2 s hold gave %d backups, not 1", backup_calls);
    ui_update_loader(loader_page(Page::Install));
    run(100);
    shoot(build, sz, "install");
    ui_update_loader(loader_page(Page::Retry));
    run(100);
    shoot(build, sz, "retry");
    ui_update_loader(loader_page(Page::Busy));
    run(100);
    shoot(build, sz, "busy");
    ui_update_loader(loader_page(Page::Done));
    run(100);
    shoot(build, sz, "done");
    ui_update_loader(loader_page(Page::NotReady));
    run(100);
    shoot(build, sz, "notready");
    tap_button(firmware, "Done", 80, 8);
    if (lv_screen_active() != settings) fail("Done on Firmware didn't return to settings");
  }

  snprintf(where, sizeof where, "%s %c settings", build, sz);
  const char next[2] = {"SML"[(size + 1) % 3], 0};
  saved_size = -1;
  tap_button(settings, next, 80, 8);
  if (saved_size != (size + 1) % 3) fail("tapping %s saved size %d", next, saved_size);
}

static void run_computer() {
  ui_begin(kBoard, {1}, {on_settings, on_computer, nullptr, nullptr, nullptr, nullptr}, kFirmware);
  ui_show_computer_mode("Firmware 4d1d09e+ hub+L");
  run(100);
  shoot("hub", '-', "computer");
}

// ---------- Contact sheet ----------
// One row per build x size, one column per screen, then Computer mode;
// captions drawn by LVGL.
static const char *const kColumns[] = {"chain0", "chain1", "chain2", "chain3", "chain4", "chain5",   "chain6",
                                       "off",    "problem", "settings", "armed", "firmware", "busy", "done", "notready"};
constexpr int kCols = sizeof kColumns / sizeof *kColumns, kGap = 8, kCaption = 18;
static uint8_t caption_rgb[W * kCaption * 3];

static void caption_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  const uint16_t *p = reinterpret_cast<const uint16_t *>(px);
  for (int y = a->y1; y <= a->y2; ++y)
    for (int x = a->x1; x <= a->x2; ++x, ++p) {
      uint8_t *o = &caption_rgb[(y * W + x) * 3];
      o[0] = ((*p >> 11) & 31) * 255 / 31;
      o[1] = ((*p >> 5) & 63) * 255 / 63;
      o[2] = (*p & 31) * 255 / 31;
    }
  lv_display_flush_ready(d);
}

static bool write_sheet(const char *out) {
  int rows = 0;
  int row_of[kMaxShots], col_of[kMaxShots];
  char last_row[24] = "";
  for (int i = 0; i < results->shots; ++i) {
    const Shot &s = results->shot[i];
    char row[24];
    snprintf(row, sizeof row, "%s %c", s.build, s.size);
    if (strcmp(row, last_row)) {
      snprintf(last_row, sizeof last_row, "%s", row);
      ++rows;
    }
    row_of[i] = rows - 1;
    col_of[i] = 0;
    if (s.size == '-') continue;  // Computer mode starts its own row
    while (col_of[i] < kCols && strcmp(kColumns[col_of[i]], s.screen)) ++col_of[i];
    if (col_of[i] == kCols) return false;
  }
  const int cw = W + kGap, ch = H + kCaption + kGap, sw = kCols * cw + kGap, sh = rows * ch + kGap;
  std::vector<uint8_t> sheet(size_t(sw) * sh * 3, 28);

  lv_init();
  lv_tick_set_cb(millis);
  lv_display_t *d = lv_display_create(W, kCaption);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  static uint16_t buf[W * kCaption];
  lv_display_set_buffers(d, buf, nullptr, sizeof buf, LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(d, caption_flush);
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x1C1C1C), 0);
  lv_obj_t *label = lv_label_create(scr);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);

  for (int i = 0; i < results->shots; ++i) {
    const Shot &s = results->shot[i];
    char text[48];
    if (s.size == '-')
      snprintf(text, sizeof text, "%s", s.screen);
    else
      snprintf(text, sizeof text, "%s %c %s", s.build, s.size, s.screen);
    lv_label_set_text(label, text);
    lv_obj_invalidate(scr);
    lv_refr_now(d);
    const int x0 = kGap + col_of[i] * cw, y0 = kGap + row_of[i] * ch;
    for (int y = 0; y < kCaption; ++y)
      memcpy(&sheet[(size_t(y0 + y) * sw + x0) * 3], &caption_rgb[y * W * 3], W * 3);
    for (int y = 0; y < H; ++y)
      memcpy(&sheet[(size_t(y0 + kCaption + y) * sw + x0) * 3], &s.rgb[y * W * 3], W * 3);
  }
  char path[512];
  snprintf(path, sizeof path, "%s/sheet.png", out);
  return png::write(path, sw, sh, sheet.data());
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "build/ui-shots";
  setvbuf(stdout, nullptr, _IOLBF, 0);
  results = static_cast<Results *>(
      mmap(nullptr, sizeof(Results), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (results == MAP_FAILED) return 2;

  // Hub, then hub-loader, at S, M, L; then Computer mode.
  for (int pass = 0; pass <= 6; ++pass) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
      if (pass < 6)
        run_build(pass >= 3, pass % 3);
      else
        run_computer();
      fflush(stdout);
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
      printf("  !! pass %d crashed (status %d)\n", pass, status);
      ++results->failures;
    }
  }

  char path[512];
  mkdir(out, 0755);
  for (const char *b : {"hub", "hub-loader"}) {
    snprintf(path, sizeof path, "%s/%s", out, b);
    mkdir(path, 0755);
  }
  for (int i = 0; i < results->shots; ++i) {
    const Shot &s = results->shot[i];
    if (s.size == '-')
      snprintf(path, sizeof path, "%s/%s.png", out, s.screen);
    else
      snprintf(path, sizeof path, "%s/%s/%c_%s.png", out, s.build, s.size, s.screen);
    if (!png::write(path, W, H, s.rgb)) {
      printf("  !! can't write %s\n", path);
      return 2;
    }
  }
  if (!write_sheet(out)) {
    printf("  !! can't write %s/sheet.png\n", out);
    return 2;
  }
  printf("%d screens in %s (sheet.png): %d layout problems, %d failed taps, %d lines cut with \"...\", %d cut at "
         "the edge\n",
         results->shots, out, results->problems, results->failures, results->dotted, results->clipped_lines);
  return results->problems || results->failures ? 1 : 0;
}
