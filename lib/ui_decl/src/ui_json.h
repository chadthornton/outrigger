// A small JSON reader for ui_decl: parses a whole document into a tree of
// Values. Enough for screen descriptions and themes: objects, arrays,
// strings (with \uXXXX escapes, BMP only), numbers, true/false/null.
#pragma once

#include <stddef.h>

#include <string>
#include <utility>
#include <vector>

namespace ui_json {

struct Value {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Value> items;                           // Array
  std::vector<std::pair<std::string, Value>> fields;  // Object, in file order

  // Object lookup; nullptr if absent or not an object.
  const Value *get(const char *key) const;
  // "a.b.c" through nested objects; nullptr if any step is missing.
  const Value *path(const char *dotted) const;
  bool is_string() const { return kind == String; }
  bool is_number() const { return kind == Number; }
  bool is_object() const { return kind == Object; }
  bool is_array() const { return kind == Array; }
};

// Parses text into out. On failure returns false and writes "line N: why"
// into err.
bool parse(const char *text, Value &out, std::string &err);

}  // namespace ui_json
