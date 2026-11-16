#include "ui_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ui_json {

const Value *Value::get(const char *key) const {
  if (kind != Object) return nullptr;
  for (const auto &f : fields)
    if (f.first == key) return &f.second;
  return nullptr;
}

const Value *Value::path(const char *dotted) const {
  const Value *v = this;
  std::string part;
  for (const char *p = dotted;; ++p) {
    if (*p == '.' || *p == 0) {
      v = v->get(part.c_str());
      if (!v) return nullptr;
      part.clear();
      if (*p == 0) return v;
    } else {
      part += *p;
    }
  }
}

namespace {

struct Parser {
  const char *s;
  const char *start;
  std::string err;

  int line() const {
    int n = 1;
    for (const char *p = start; p < s; ++p) n += *p == '\n';
    return n;
  }
  bool fail(const char *why) {
    if (err.empty()) {
      char b[96];
      snprintf(b, sizeof b, "line %d: %s", line(), why);
      err = b;
    }
    return false;
  }
  void ws() {
    for (;;) {
      while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
      // Comments are not JSON, but designers write them; allow // to end of line.
      if (s[0] == '/' && s[1] == '/') {
        while (*s && *s != '\n') ++s;
      } else {
        return;
      }
    }
  }
  bool lit(const char *w) {
    const size_t n = strlen(w);
    if (strncmp(s, w, n) != 0) return false;
    s += n;
    return true;
  }
  static void utf8(std::string &o, unsigned c) {
    if (c < 0x80) {
      o += char(c);
    } else if (c < 0x800) {
      o += char(0xC0 | (c >> 6));
      o += char(0x80 | (c & 0x3F));
    } else {
      o += char(0xE0 | (c >> 12));
      o += char(0x80 | ((c >> 6) & 0x3F));
      o += char(0x80 | (c & 0x3F));
    }
  }
  bool string(std::string &o) {
    if (*s != '"') return fail("expected a string");
    ++s;
    while (*s != '"') {
      if (*s == 0) return fail("unterminated string");
      if (*s == '\\') {
        ++s;
        switch (*s) {
          case '"': o += '"'; break;
          case '\\': o += '\\'; break;
          case '/': o += '/'; break;
          case 'b': o += '\b'; break;
          case 'f': o += '\f'; break;
          case 'n': o += '\n'; break;
          case 'r': o += '\r'; break;
          case 't': o += '\t'; break;
          case 'u': {
            unsigned c = 0;
            for (int i = 1; i <= 4; ++i) {
              const char h = s[i];
              c <<= 4;
              if (h >= '0' && h <= '9') c |= unsigned(h - '0');
              else if (h >= 'a' && h <= 'f') c |= unsigned(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') c |= unsigned(h - 'A' + 10);
              else return fail("bad \\u escape");
            }
            utf8(o, c);
            s += 4;
            break;
          }
          default: return fail("bad escape");
        }
        ++s;
      } else {
        o += *s++;
      }
    }
    ++s;
    return true;
  }
  bool value(Value &v, int depth) {
    if (depth > 64) return fail("nested too deep");
    ws();
    if (*s == '{') {
      ++s;
      v.kind = Value::Object;
      ws();
      if (*s == '}') { ++s; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!string(k)) return false;
        ws();
        if (*s != ':') return fail("expected ':'");
        ++s;
        v.fields.emplace_back(std::move(k), Value());
        if (!value(v.fields.back().second, depth + 1)) return false;
        ws();
        if (*s == ',') { ++s; continue; }
        if (*s == '}') { ++s; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (*s == '[') {
      ++s;
      v.kind = Value::Array;
      ws();
      if (*s == ']') { ++s; return true; }
      for (;;) {
        v.items.emplace_back();
        if (!value(v.items.back(), depth + 1)) return false;
        ws();
        if (*s == ',') { ++s; continue; }
        if (*s == ']') { ++s; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (*s == '"') {
      v.kind = Value::String;
      return string(v.str);
    }
    if (lit("true")) { v.kind = Value::Bool; v.b = true; return true; }
    if (lit("false")) { v.kind = Value::Bool; v.b = false; return true; }
    if (lit("null")) { v.kind = Value::Null; return true; }
    if (*s == '-' || (*s >= '0' && *s <= '9')) {
      char *end;
      v.kind = Value::Number;
      v.num = strtod(s, &end);
      if (end == s) return fail("bad number");
      s = end;
      return true;
    }
    return fail("unexpected character");
  }
};

}  // namespace

bool parse(const char *text, Value &out, std::string &err) {
  Parser p{text, text, {}};
  out = Value();
  if (!p.value(out, 0)) {
    err = p.err;
    return false;
  }
  p.ws();
  if (*p.s != 0) {
    p.fail("text after the document");
    err = p.err;
    return false;
  }
  return true;
}

}  // namespace ui_json
