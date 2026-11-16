#include "ui_decl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_json.h"

using ui_json::Value;

namespace ui_decl {

lv_obj_t *Result::find(const char *id) const {
  for (const auto &p : ids)
    if (p.first == id) return p.second;
  return nullptr;
}

namespace {

// The Montserrat sizes firmware/config/lv_conf.h compiles in. A type token
// picks the largest of these that isn't bigger than it asks for.
struct FontSize {
  int px;
  const lv_font_t *font;
};
const FontSize kFonts[] = {
#if LV_FONT_MONTSERRAT_10
    {10, &lv_font_montserrat_10},
#endif
#if LV_FONT_MONTSERRAT_12
    {12, &lv_font_montserrat_12},
#endif
#if LV_FONT_MONTSERRAT_14
    {14, &lv_font_montserrat_14},
#endif
#if LV_FONT_MONTSERRAT_16
    {16, &lv_font_montserrat_16},
#endif
#if LV_FONT_MONTSERRAT_18
    {18, &lv_font_montserrat_18},
#endif
#if LV_FONT_MONTSERRAT_20
    {20, &lv_font_montserrat_20},
#endif
#if LV_FONT_MONTSERRAT_22
    {22, &lv_font_montserrat_22},
#endif
#if LV_FONT_MONTSERRAT_24
    {24, &lv_font_montserrat_24},
#endif
#if LV_FONT_MONTSERRAT_28
    {28, &lv_font_montserrat_28},
#endif
#if LV_FONT_MONTSERRAT_32
    {32, &lv_font_montserrat_32},
#endif
#if LV_FONT_MONTSERRAT_48
    {48, &lv_font_montserrat_48},
#endif
};

const lv_font_t *font_for(int px) {
  const lv_font_t *best = kFonts[0].font;
  for (const auto &f : kFonts)
    if (f.px <= px) best = f.font;
  return best;
}

// Grid templates live as long as their container.
void free_grid(lv_event_t *e) { delete[] static_cast<int32_t *>(lv_event_get_user_data(e)); }

struct Builder {
  Value theme, data;
  Result *res;
  std::string err;
  std::vector<std::string> where;              // the node path, for errors
  std::vector<const Value *> scope;            // "for" items, innermost last
  std::vector<std::string> id_suffix;          // ".<index>" per enclosing "for"

  std::string here() const {
    std::string s = "root";
    for (const auto &w : where) s += w;
    return s;
  }
  bool fail(const std::string &why) {
    if (err.empty()) err = "screen: " + here() + ": " + why;
    return false;
  }

  // ---------- Tokens and bindings ----------

  // "$color.accent" -> the theme's value (tokens may name other tokens).
  const Value *token(const Value *v) {
    for (int depth = 0; v && v->is_string() && !v->str.empty() && v->str[0] == '$'; ++depth) {
      if (depth > 8) {
        fail("token loop at " + v->str);
        return nullptr;
      }
      const Value *t = theme.path(v->str.c_str() + 1);
      if (!t) {
        fail("unknown token " + v->str);
        return nullptr;
      }
      v = t;
    }
    return v;
  }

  // A data path, looked up in the innermost "for" item first, then the data.
  const Value *lookup(const std::string &path) {
    for (size_t i = scope.size(); i-- > 0;)
      if (const Value *v = scope[i]->path(path.c_str())) return v;
    if (const Value *v = data.path(path.c_str())) return v;
    fail("nothing in the data for {" + path + "}");
    return nullptr;
  }

  static std::string to_text(const Value &v) {
    char b[32];
    switch (v.kind) {
      case Value::String: return v.str;
      case Value::Number:
        if (v.num == double(long(v.num))) snprintf(b, sizeof b, "%ld", long(v.num));
        else snprintf(b, sizeof b, "%g", v.num);
        return b;
      case Value::Bool: return v.b ? "true" : "false";
      default: return "";
    }
  }

  // "Cutoff {param.value}%" -> "Cutoff 87%".
  bool bind(const std::string &in, std::string &out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '{') {
        const size_t j = in.find('}', i);
        if (j == std::string::npos) return fail("unclosed { in \"" + in + "\"");
        const Value *v = lookup(in.substr(i + 1, j - i - 1));
        if (!v) return false;
        out += to_text(*v);
        i = j;
      } else {
        out += in[i];
      }
    }
    return true;
  }

  // A whole-value binding ("{page.knobs}") or a token, else the value itself.
  const Value *resolve(const Value *v) {
    v = token(v);
    if (v && v->is_string() && v->str.size() > 2 && v->str.front() == '{' && v->str.back() == '}' &&
        v->str.find('{', 1) == std::string::npos)
      return lookup(v->str.substr(1, v->str.size() - 2));
    return v;
  }

  bool number(const Value *v, double &out) {
    v = resolve(v);
    if (!v) return false;
    if (v->is_number()) {
      out = v->num;
      return true;
    }
    if (v->is_string()) {
      std::string s;
      if (!bind(v->str, s)) return false;
      char *end;
      out = strtod(s.c_str(), &end);
      if (end != s.c_str() && *end == 0) return true;
    }
    return fail("expected a number");
  }

  bool text(const Value *v, std::string &out) {
    v = resolve(v);
    if (!v) return false;
    if (v->is_string()) return bind(v->str, out);
    if (v->kind == Value::Number || v->kind == Value::Bool) {
      out = to_text(*v);
      return true;
    }
    return fail("expected text");
  }

  bool truthy(const Value *v, bool &out) {
    v = resolve(v);
    if (!v) return false;
    switch (v->kind) {
      case Value::Bool: out = v->b; return true;
      case Value::Number: out = v->num != 0; return true;
      case Value::String: out = !v->str.empty() && v->str != "false"; return true;
      default: out = false; return true;
    }
  }

  bool color(const Value *v, lv_color_t &out) {
    v = token(v);
    if (!v) return false;
    if (v->is_string() && v->str.size() == 7 && v->str[0] == '#') {
      char *end;
      const unsigned long x = strtoul(v->str.c_str() + 1, &end, 16);
      if (*end == 0) {
        out = lv_color_hex(uint32_t(x));
        return true;
      }
    }
    return fail("expected a colour like \"#FF8A3D\"");
  }

  // 120, "50%", "content", or a token for one of those.
  bool size(const Value *v, int32_t &out) {
    v = token(v);
    if (!v) return false;
    if (v->is_number()) {
      out = int32_t(v->num);
      return true;
    }
    if (v->is_string()) {
      if (v->str == "content") {
        out = LV_SIZE_CONTENT;
        return true;
      }
      if (!v->str.empty() && v->str.back() == '%') {
        out = lv_pct(atoi(v->str.c_str()));
        return true;
      }
    }
    return fail("expected a size: px, \"50%\" or \"content\"");
  }

  bool px(const Value *v, int32_t &out) {
    double d;
    if (!number(v, d)) return false;
    out = int32_t(d);
    return true;
  }

  // ---------- Styles ----------

  // A style is a theme style's name, or an object of properties; "use" in an
  // object starts from a named style. Properties land on `part`, except
  // "accent" (a bar's fill, a knob's arc) and "track" (what's behind it).
  bool style(lv_obj_t *o, const Value *s, int depth = 0) {
    if (depth > 8) return fail("style loop");
    if (s->is_string()) {
      std::string name;
      if (!bind(s->str, name)) return false;
      const Value *styles = theme.get("styles");
      const Value *named = styles ? styles->get(name.c_str()) : nullptr;
      if (!named) return fail("unknown style \"" + name + "\"");
      return style(o, named, depth + 1);
    }
    if (!s->is_object()) return fail("a style is a name or an object");
    if (const Value *use = s->get("use"))
      if (!style(o, use, depth + 1)) return false;
    const lv_style_selector_t M = LV_PART_MAIN;
    for (const auto &f : s->fields) {
      const std::string &k = f.first;
      const Value *v = &f.second;
      lv_color_t c;
      int32_t n;
      if (k == "use") {
        continue;
      } else if (k == "bg") {
        if (!color(v, c)) return false;
        lv_obj_set_style_bg_color(o, c, M);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, M);
      } else if (k == "bg_opa") {
        if (!px(v, n)) return false;
        lv_obj_set_style_bg_opa(o, lv_opa_t(n), M);
      } else if (k == "text") {
        if (!color(v, c)) return false;
        lv_obj_set_style_text_color(o, c, M);
      } else if (k == "font") {
        if (!px(v, n)) return false;
        lv_obj_set_style_text_font(o, font_for(n), M);
      } else if (k == "text_align") {
        const Value *t = token(v);
        if (!t || !t->is_string()) return fail("text_align: left, center or right");
        lv_obj_set_style_text_align(o,
                                    t->str == "center"  ? LV_TEXT_ALIGN_CENTER
                                    : t->str == "right" ? LV_TEXT_ALIGN_RIGHT
                                                        : LV_TEXT_ALIGN_LEFT,
                                    M);
      } else if (k == "pad") {
        if (!px(v, n)) return false;
        lv_obj_set_style_pad_all(o, n, M);
      } else if (k == "pad_x") {
        if (!px(v, n)) return false;
        lv_obj_set_style_pad_hor(o, n, M);
      } else if (k == "pad_y") {
        if (!px(v, n)) return false;
        lv_obj_set_style_pad_ver(o, n, M);
      } else if (k == "pad_top") {
        if (!px(v, n)) return false;
        lv_obj_set_style_pad_top(o, n, M);
      } else if (k == "pad_bottom") {
        if (!px(v, n)) return false;
        lv_obj_set_style_pad_bottom(o, n, M);
      } else if (k == "radius") {
        if (!px(v, n)) return false;
        lv_obj_set_style_radius(o, n, M);
        lv_obj_set_style_radius(o, n, LV_PART_INDICATOR);
      } else if (k == "border") {
        if (!px(v, n)) return false;
        lv_obj_set_style_border_width(o, n, M);
      } else if (k == "border_color") {
        if (!color(v, c)) return false;
        lv_obj_set_style_border_color(o, c, M);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, M);
      } else if (k == "opa") {
        if (!px(v, n)) return false;
        lv_obj_set_style_opa(o, lv_opa_t(n), M);
      } else if (k == "letter_space") {
        if (!px(v, n)) return false;
        lv_obj_set_style_text_letter_space(o, n, M);
      } else if (k == "accent") {
        if (!color(v, c)) return false;
        lv_obj_set_style_bg_color(o, c, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(o, c, LV_PART_INDICATOR);
      } else if (k == "track") {
        // A bar's track is its background; a knob's is the arc behind the value.
        if (!color(v, c)) return false;
        if (lv_obj_check_type(o, &lv_arc_class)) {
          lv_obj_set_style_arc_color(o, c, M);
        } else {
          lv_obj_set_style_bg_color(o, c, M);
          lv_obj_set_style_bg_opa(o, LV_OPA_COVER, M);
        }
      } else if (k == "arc_width") {
        if (!px(v, n)) return false;
        lv_obj_set_style_arc_width(o, n, M);
        lv_obj_set_style_arc_width(o, n, LV_PART_INDICATOR);
      } else {
        return fail("unknown style key '" + k + "'");
      }
      if (!err.empty()) return false;
    }
    return true;
  }

  // ---------- Layout ----------

  static bool flex_align(const std::string &s, lv_flex_align_t &out) {
    static const struct {
      const char *name;
      lv_flex_align_t a;
    } kA[] = {{"start", LV_FLEX_ALIGN_START},
              {"center", LV_FLEX_ALIGN_CENTER},
              {"end", LV_FLEX_ALIGN_END},
              {"space_between", LV_FLEX_ALIGN_SPACE_BETWEEN},
              {"space_around", LV_FLEX_ALIGN_SPACE_AROUND},
              {"space_evenly", LV_FLEX_ALIGN_SPACE_EVENLY}};
    for (const auto &a : kA)
      if (s == a.name) {
        out = a.a;
        return true;
      }
    return false;
  }
  static bool grid_align(const std::string &s, lv_grid_align_t &out) {
    static const struct {
      const char *name;
      lv_grid_align_t a;
    } kA[] = {{"start", LV_GRID_ALIGN_START},
              {"center", LV_GRID_ALIGN_CENTER},
              {"end", LV_GRID_ALIGN_END},
              {"stretch", LV_GRID_ALIGN_STRETCH}};
    for (const auto &a : kA)
      if (s == a.name) {
        out = a.a;
        return true;
      }
    return false;
  }

  // One grid track: px, "content" or "<n>fr".
  bool track(const Value *v, int32_t &out) {
    v = token(v);
    if (!v) return false;
    if (v->is_number()) {
      out = int32_t(v->num);
      return true;
    }
    if (v->is_string()) {
      if (v->str == "content") {
        out = LV_GRID_CONTENT;
        return true;
      }
      if (v->str.size() > 2 && v->str.compare(v->str.size() - 2, 2, "fr") == 0) {
        out = LV_GRID_FR(atoi(v->str.c_str()));
        return true;
      }
    }
    return fail("a grid track is px, \"content\" or \"1fr\"");
  }

  bool layout(lv_obj_t *o, const Value *l) {
    if (!l->is_object()) return fail("layout is an object");
    int32_t gap = 0;
    if (const Value *g = l->get("gap")) {
      if (!px(g, gap)) return false;
      lv_obj_set_style_pad_row(o, gap, LV_PART_MAIN);
      lv_obj_set_style_pad_column(o, gap, LV_PART_MAIN);
    }
    if (const Value *f = l->get("flex")) {
      static const struct {
        const char *name;
        lv_flex_flow_t flow;
      } kFlows[] = {{"row", LV_FLEX_FLOW_ROW},
                    {"column", LV_FLEX_FLOW_COLUMN},
                    {"row_wrap", LV_FLEX_FLOW_ROW_WRAP},
                    {"column_wrap", LV_FLEX_FLOW_COLUMN_WRAP}};
      bool found = false;
      for (const auto &k : kFlows)
        if (f->is_string() && f->str == k.name) {
          lv_obj_set_flex_flow(o, k.flow);
          found = true;
        }
      if (!found) return fail("flex: row, column, row_wrap or column_wrap");
      lv_flex_align_t a[3] = {LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START};
      const char *keys[3] = {"main", "cross", "track"};
      for (int i = 0; i < 3; ++i)
        if (const Value *v = l->get(keys[i]))
          if (!v->is_string() || !flex_align(v->str, a[i]))
            return fail(std::string(keys[i]) + ": start, center, end, space_between, space_around or space_evenly");
      // LVGL places the whole line of children by "track"; a designer who
      // says cross: center means the line too, unless they say otherwise.
      if (!l->get("track")) a[2] = a[1];
      lv_obj_set_flex_align(o, a[0], a[1], a[2]);
      return true;
    }
    if (const Value *g = l->get("grid")) {
      const Value *cols = g->get("cols"), *rows = g->get("rows");
      if (!cols || !rows || !cols->is_array() || !rows->is_array()) return fail("grid needs cols and rows arrays");
      // One allocation: cols, LAST, rows, LAST. Freed with the container.
      int32_t *dsc = new int32_t[cols->items.size() + rows->items.size() + 2];
      size_t n = 0;
      bool ok = true;
      for (const auto &c : cols->items) ok = ok && track(&c, dsc[n++]);
      dsc[n++] = LV_GRID_TEMPLATE_LAST;
      const size_t row0 = n;
      for (const auto &r : rows->items) ok = ok && track(&r, dsc[n++]);
      dsc[n++] = LV_GRID_TEMPLATE_LAST;
      if (!ok) {
        delete[] dsc;
        return false;
      }
      lv_obj_set_grid_dsc_array(o, dsc, dsc + row0);
      lv_obj_add_event_cb(o, free_grid, LV_EVENT_DELETE, dsc);
      return true;
    }
    return fail("layout needs flex or grid");
  }

  // {"col": 0, "row": 1, "col_span": 1, "row_span": 1, "x": "stretch", "y": "center"}
  bool cell(lv_obj_t *o, const Value *c) {
    if (!c->is_object()) return fail("cell is an object");
    int32_t col = 0, row = 0, cs = 1, rs = 1;
    if (const Value *v = c->get("col")) if (!px(v, col)) return false;
    if (const Value *v = c->get("row")) if (!px(v, row)) return false;
    if (const Value *v = c->get("col_span")) if (!px(v, cs)) return false;
    if (const Value *v = c->get("row_span")) if (!px(v, rs)) return false;
    lv_grid_align_t ax = LV_GRID_ALIGN_STRETCH, ay = LV_GRID_ALIGN_STRETCH;
    if (const Value *v = c->get("x")) if (!v->is_string() || !grid_align(v->str, ax)) return fail("cell x: start, center, end or stretch");
    if (const Value *v = c->get("y")) if (!v->is_string() || !grid_align(v->str, ay)) return fail("cell y: start, center, end or stretch");
    lv_obj_set_grid_cell(o, ax, col, cs, ay, row, rs);
    return true;
  }

  // ---------- Widgets ----------

  static bool known_key(const std::string &k) {
    static const char *const kKeys[] = {"type", "id", "for", "text", "value", "min", "max", "items", "overflow", "width",
                                        "height", "size", "grow", "cell", "layout", "style", "style_when", "children",
                                        "hidden", "note"};
    for (const char *key : kKeys)
      if (k == key) return true;
    return false;
  }

  bool range(lv_obj_t *o, const Value &n, bool is_bar) {
    double lo = 0, hi = 100, v = 0;
    if (const Value *x = n.get("min")) if (!number(x, lo)) return false;
    if (const Value *x = n.get("max")) if (!number(x, hi)) return false;
    if (const Value *x = n.get("value")) if (!number(x, v)) return false;
    if (is_bar) {
      lv_bar_set_range(o, int32_t(lo), int32_t(hi));
      lv_bar_set_value(o, int32_t(v), LV_ANIM_OFF);
    } else {
      lv_arc_set_range(o, int32_t(lo), int32_t(hi));
      lv_arc_set_value(o, int32_t(v));
    }
    return true;
  }

  lv_obj_t *widget(lv_obj_t *parent, const Value &n) {
    const Value *t = n.get("type");
    if (!t || !t->is_string()) {
      fail("every node needs a \"type\"");
      return nullptr;
    }
    for (const auto &f : n.fields)
      if (!known_key(f.first)) {
        fail("unknown key '" + f.first + "'");
        return nullptr;
      }
    const std::string &type = t->str;
    lv_obj_t *o = nullptr;
    std::string s;
    if (type == "page") {
      o = lv_obj_create(parent);
      lv_obj_remove_style_all(o);
      lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    } else if (type == "label") {
      o = lv_label_create(parent);
      if (const Value *x = n.get("text")) {
        if (!text(x, s)) return nullptr;
        lv_label_set_text(o, s.c_str());
      }
      if (const Value *x = n.get("overflow")) {
        const std::string m = x->is_string() ? x->str : "";
        lv_label_long_mode_t lm;
        if (m == "wrap") lm = LV_LABEL_LONG_MODE_WRAP;
        else if (m == "dots") lm = LV_LABEL_LONG_MODE_DOTS;
        else if (m == "clip") lm = LV_LABEL_LONG_MODE_CLIP;
        else if (m == "scroll") lm = LV_LABEL_LONG_MODE_SCROLL_CIRCULAR;
        else {
          fail("overflow: wrap, dots, clip or scroll");
          return nullptr;
        }
        lv_label_set_long_mode(o, lm);
      }
    } else if (type == "bar") {
      o = lv_bar_create(parent);
      lv_obj_set_width(o, lv_pct(100));
      if (!range(o, n, true)) return nullptr;
    } else if (type == "knob") {
      // A 270-degree arc, open at the bottom, like a pot's travel.
      o = lv_arc_create(parent);
      lv_arc_set_rotation(o, 135);
      lv_arc_set_bg_angles(o, 0, 270);
      lv_obj_remove_style(o, nullptr, LV_PART_KNOB);
      lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
      if (!range(o, n, false)) return nullptr;
    } else if (type == "button") {
      o = lv_button_create(parent);
      lv_obj_t *l = lv_label_create(o);
      if (const Value *x = n.get("text")) {
        if (!text(x, s)) return nullptr;
        lv_label_set_text(l, s.c_str());
      }
      lv_obj_center(l);
    } else if (type == "list") {
      o = lv_list_create(parent);
      if (const Value *x = n.get("items")) {
        const Value *items = resolve(x);
        if (!items) return nullptr;
        if (!items->is_array()) {
          fail("items is an array, or a binding to one");
          return nullptr;
        }
        for (const auto &it : items->items) {
          if (!text(&it, s)) return nullptr;
          lv_list_add_button(o, nullptr, s.c_str());
        }
      }
    } else {
      fail("unknown type \"" + type + "\" (page, label, bar, knob, button or list)");
      return nullptr;
    }
    return o;
  }

  // Builds one node (not "for": the caller expands that) and its children.
  lv_obj_t *node(lv_obj_t *parent, const Value &n) {
    if (!n.is_object()) {
      fail("a node is an object");
      return nullptr;
    }
    lv_obj_t *o = widget(parent, n);
    if (!o) return nullptr;
    const std::string type = n.get("type")->str;
    // The theme's defaults for this type, then the node's own style.
    if (const Value *defs = theme.get("defaults"))
      if (const Value *d = defs->get(type.c_str()))
        if (!style(o, d)) return nullptr;
    if (const Value *s = n.get("style"))
      if (!style(o, s)) return nullptr;
    // {"{active}": "knob_on"}: a named style when the binding is true.
    if (const Value *w = n.get("style_when")) {
      if (!w->is_object()) {
        fail("style_when maps a binding to a style");
        return nullptr;
      }
      for (const auto &f : w->fields) {
        Value cond;
        cond.kind = Value::String;
        cond.str = f.first;
        bool on;
        if (!truthy(&cond, on)) return nullptr;
        if (on && !style(o, &f.second)) return nullptr;
      }
    }
    int32_t v;
    if (const Value *x = n.get("size")) {
      if (!size(x, v)) return nullptr;
      lv_obj_set_size(o, v, v);
    }
    if (const Value *x = n.get("width")) {
      if (!size(x, v)) return nullptr;
      lv_obj_set_width(o, v);
    }
    if (const Value *x = n.get("height")) {
      if (!size(x, v)) return nullptr;
      lv_obj_set_height(o, v);
    }
    if (const Value *x = n.get("grow")) {
      if (!px(x, v)) return nullptr;
      lv_obj_set_flex_grow(o, uint8_t(v));
    }
    if (const Value *x = n.get("layout"))
      if (!layout(o, x)) return nullptr;
    if (const Value *x = n.get("cell"))
      if (!cell(o, x)) return nullptr;
    if (const Value *x = n.get("hidden")) {
      bool h;
      if (!truthy(x, h)) return nullptr;
      if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (const Value *x = n.get("id")) {
      if (!x->is_string()) {
        fail("id is a string");
        return nullptr;
      }
      std::string id = x->str;
      for (const auto &sfx : id_suffix) id += sfx;
      res->ids.emplace_back(id, o);
    }
    if (const Value *c = n.get("children")) {
      if (!c->is_array()) {
        fail("children is an array");
        return nullptr;
      }
      if (!children(o, *c)) return nullptr;
    }
    return o;
  }

  bool children(lv_obj_t *parent, const Value &list) {
    for (size_t i = 0; i < list.items.size(); ++i) {
      const Value &c = list.items[i];
      char w[32];
      snprintf(w, sizeof w, ".children[%zu]", i);
      where.push_back(w);
      const Value *each = c.is_object() ? c.get("for") : nullptr;
      if (each) {
        const Value *arr = each->is_string() ? lookup(each->str) : nullptr;
        if (!arr) return each->is_string() ? false : fail("for names a data array");
        if (!arr->is_array()) return fail("for: {" + each->str + "} isn't an array");
        for (size_t k = 0; k < arr->items.size(); ++k) {
          scope.push_back(&arr->items[k]);
          snprintf(w, sizeof w, ".%zu", k);
          id_suffix.push_back(w);
          const bool ok = node(parent, c) != nullptr;
          id_suffix.pop_back();
          scope.pop_back();
          if (!ok) return false;
        }
      } else if (!node(parent, c)) {
        return false;
      }
      where.pop_back();
    }
    return true;
  }
};

}  // namespace

Result build(lv_obj_t *parent, const char *theme_json, const char *screen_json, const char *data_json) {
  Result r;
  Builder b;
  b.res = &r;
  Value screen;
  std::string e;
  if (!ui_json::parse(theme_json, b.theme, e)) {
    r.error = "theme: " + e;
    return r;
  }
  if (!ui_json::parse(screen_json, screen, e)) {
    r.error = "screen: " + e;
    return r;
  }
  if (data_json && !ui_json::parse(data_json, b.data, e)) {
    r.error = "data: " + e;
    return r;
  }
  const Value *root = screen.get("root");
  if (!root) {
    r.error = "screen: needs a \"root\" node";
    return r;
  }
  const uint32_t before = lv_obj_get_child_count(parent);
  r.root = b.node(parent, *root);
  if (!b.err.empty() || !r.root) {
    r.error = b.err.empty() ? "screen: build failed" : b.err;
    // Whatever was built hangs off parent after its old children; take it back.
    while (lv_obj_get_child_count(parent) > before) lv_obj_delete(lv_obj_get_child(parent, int32_t(before)));
    r.root = nullptr;
    r.ids.clear();
    return r;
  }
  r.ok = true;
  return r;
}

}  // namespace ui_decl
