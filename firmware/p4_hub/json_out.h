// A bounded JSON writer into a caller's buffer. Never overflows; len stops
// growing when the buffer is full (the reply is then cut short, not corrupt
// memory).
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct JsonOut {
  char *buf;
  size_t cap;
  size_t len = 0;
  JsonOut(char *b, size_t c) : buf(b), cap(c) { if (cap) buf[0] = 0; }
  size_t room() const { return cap > len ? cap - len : 0; }
  void raw(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (room() <= 1) return;
    va_list a;
    va_start(a, fmt);
    const int n = vsnprintf(buf + len, room(), fmt, a);
    va_end(a);
    if (n > 0) len += (size_t)n < room() ? (size_t)n : room() - 1;
  }
  void str(const char *s) {
    raw("\"");
    for (; s && *s && room() > 8; ++s) {
      const unsigned char c = (unsigned char)*s;
      if (c == '"' || c == '\\') raw("\\%c", c);
      else if (c < 0x20) raw("\\u%04x", c);
      else { buf[len++] = c; buf[len] = 0; }
    }
    raw("\"");
  }
};
