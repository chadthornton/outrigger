// ui_decl: an app describes its screen as data, and this builds the LVGL
// objects for it.
//
// Three JSON documents go in (lib/ui_decl/AUTHORING.md has the full format):
//   theme  - design tokens: colours, a type scale, spacing, radii, and named
//            styles built from them. One per product, shared by every screen.
//   screen - a widget tree: page containers with flex or grid layouts, and
//            labels, value bars, knobs, buttons and lists inside them.
//            Values can be tokens ("$color.accent") and text can bind to
//            the data ("{param.name}"); "for" repeats a node per array item.
//   data   - what the app shows right now (a parameter's name and value,
//            the page's knobs). Optional.
//
// The same code runs on the board, in `make ui-shots` on the Mac, and in the
// browser preview (WebAssembly), so all three draw from the same files.
#pragma once

#include <lvgl.h>

#include <string>
#include <utility>
#include <vector>

namespace ui_decl {

struct Result {
  bool ok = false;
  std::string error;  // "screen: root.children[2].style: unknown key 'bgcolor'"
  lv_obj_t *root = nullptr;
  // Every node with an "id", for the app to update later (lv_label_set_text,
  // lv_bar_set_value). Repeated nodes get "<id>.<index>".
  std::vector<std::pair<std::string, lv_obj_t *>> ids;

  lv_obj_t *find(const char *id) const;
};

// Builds the screen under parent (usually lv_screen_active()). data_json may
// be null. Nothing is left behind on failure.
Result build(lv_obj_t *parent, const char *theme_json, const char *screen_json, const char *data_json);

}  // namespace ui_decl
