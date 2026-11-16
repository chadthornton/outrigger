// Part of `make ui-shots`: renders every ui_decl screen description in
// lib/ui_decl/screens/ with LVGL, at the P4 board's size (the Waveshare
// ESP32-P4 3.5" has the S3 3.5"'s ST7796 panel, 320x480, used sideways), and
// checks the description format's errors.
//
// Writes <out>/<screen>_<w>x<h>.png and .rgb565 (the raw framebuffer, for
// comparing with the browser build) and <out>/report.txt; exits 1 on a layout
// problem or a failed check.
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>

#include "../../lib/ui_decl/preview/preview.h"
#include "../../lib/ui_decl/src/ui_decl.h"
#include "../ui_shots/png.h"

static std::string slurp(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) {
    fprintf(stderr, "ui_decl: can't read %s\n", path.c_str());
    exit(1);
  }
  std::string s;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  fclose(f);
  return s;
}

static int failures;

// A broken description must fail with an error that names the problem.
static void expect_error(const char *what, const char *screen, const char *data, const char *want) {
  const std::string theme = slurp("lib/ui_decl/screens/theme.json");
  ui_preview::Frame f;
  std::string err;
  const bool ok = ui_preview::render(64, 64, theme.c_str(), screen, data, f, err);
  if (ok || err.find(want) == std::string::npos) {
    printf("  FAIL %s: wanted an error with \"%s\", got %s\n", what, want, ok ? "no error" : err.c_str());
    ++failures;
  } else {
    printf("  ok   %s -> %s\n", what, err.c_str());
  }
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "build/ui-shots/decl";
  mkdir("build", 0755);
  mkdir("build/ui-shots", 0755);
  mkdir(out, 0755);

  struct Shot {
    const char *screen;
    int w, h;
  };
  static const Shot kShots[] = {{"knob_labels", 480, 320}};

  const std::string theme = slurp("lib/ui_decl/screens/theme.json");
  std::string report;
  for (const Shot &s : kShots) {
    const std::string dir = "lib/ui_decl/screens/";
    const std::string screen = slurp(dir + s.screen + ".json");
    const std::string data = slurp(dir + s.screen + ".data.json");
    ui_preview::Frame f;
    std::string err;
    char name[128];
    snprintf(name, sizeof name, "%s_%dx%d", s.screen, s.w, s.h);
    if (!ui_preview::render(s.w, s.h, theme.c_str(), screen.c_str(), data.c_str(), f, err)) {
      printf("  FAIL %s: %s\n", name, err.c_str());
      ++failures;
      continue;
    }
    const std::string base = std::string(out) + "/" + name;
    if (!png::write((base + ".png").c_str(), f.w, f.h, f.rgb888.data())) {
      printf("  FAIL can't write %s.png\n", base.c_str());
      ++failures;
    }
    if (FILE *r = fopen((base + ".rgb565").c_str(), "wb")) {
      fwrite(f.rgb565.data(), 2, f.rgb565.size(), r);
      fclose(r);
    }
    printf("  %s %s: %d layout problems\n", f.layout_problems ? "FAIL" : "ok  ", name, f.layout_problems);
    if (f.layout_problems) printf("%s", f.report.c_str());
    failures += f.layout_problems;
    char line[160];
    snprintf(line, sizeof line, "%s %d\n", name, f.layout_problems);
    report += line + f.report;
  }

  // The format's errors: each names where and what.
  expect_error("unknown widget", R"({"root":{"type":"slider"}})", nullptr, "unknown type \"slider\"");
  expect_error("unknown style key", R"({"root":{"type":"page","style":{"bgcolor":"#000000"}}})", nullptr,
               "root: unknown style key 'bgcolor'");
  expect_error("unknown token", R"({"root":{"type":"label","text":"x","style":{"text":"$color.nope"}}})", nullptr,
               "unknown token $color.nope");
  expect_error("unbound text", R"({"root":{"type":"page","children":[{"type":"label","text":"{param.name}"}]}})",
               R"({"param":{}})", "root.children[0]: nothing in the data for {param.name}");
  expect_error("for over a non-array", R"({"root":{"type":"page","children":[{"for":"x","type":"label"}]}})",
               R"({"x":3})", "isn't an array");
  expect_error("bad JSON", R"({"root":{"type":"page",}})", nullptr, "screen: line 1:");
  expect_error("typo'd node key", R"({"root":{"type":"page","chidren":[]}})", nullptr, "unknown key 'chidren'");

  // "for" repeats a node per item, binds each item, and suffixes its id.
  {
    const ui_decl::Result r = ui_decl::build(
        lv_screen_active(), theme.c_str(),
        R"({"root":{"type":"page","layout":{"flex":"row"},"children":[{"for":"k","type":"label","id":"n","text":"{name}"}]}})",
        R"({"k":[{"name":"A"},{"name":"B"},{"name":"C"}]})");
    lv_obj_t *third = r.find("n.2");
    if (!r.ok || !third || strcmp(lv_label_get_text(third), "C") != 0 || lv_obj_get_child_count(r.root) != 3) {
      printf("  FAIL for: %s\n", r.ok ? "wrong items" : r.error.c_str());
      ++failures;
    } else {
      printf("  ok   for: 3 labels, n.2 says \"C\"\n");
    }
  }

  if (FILE *r = fopen((std::string(out) + "/report.txt").c_str(), "w")) {
    fputs(report.c_str(), r);
    fclose(r);
  }
  printf("ui_decl: %d failures\n", failures);
  return failures ? 1 : 0;
}
