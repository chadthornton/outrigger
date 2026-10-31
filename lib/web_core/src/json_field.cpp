#include "json_field.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace web_core {

// Points just past the ':' after "key", or nullptr.
static const char *value_of(const char *body, const char *key) {
  if (!body || !key) return nullptr;
  const size_t klen = strlen(key);
  for (const char *p = strchr(body, '"'); p; p = strchr(p + 1, '"')) {
    if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"') continue;
    const char *q = p + 2 + klen;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') ++q;
    if (*q == ':') return q + 1;
  }
  return nullptr;
}

static const char *skip_space(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  return p;
}

bool json_string(const char *body, const char *key, char *out, size_t len) {
  const char *p = value_of(body, key);
  if (!p || len == 0) return false;
  p = skip_space(p);
  if (*p++ != '"') return false;
  size_t n = 0;
  while (*p && *p != '"') {
    char ch = *p++;
    if (ch == '\\') {
      ch = *p++;
      if (ch == 'n') ch = '\n';
      else if (ch == 't') ch = '\t';
      else if (ch != '"' && ch != '\\' && ch != '/') return false;
    }
    if (n + 1 >= len) return false;
    out[n++] = ch;
  }
  if (*p != '"') return false;
  out[n] = 0;
  return true;
}

bool json_int(const char *body, const char *key, long *out) {
  const char *p = value_of(body, key);
  if (!p) return false;
  p = skip_space(p);
  char *end = nullptr;
  const long v = strtol(p, &end, 10);
  if (end == p) return false;
  *out = v;
  return true;
}

size_t json_escape(const char *in, char *out, size_t len) {
  if (len == 0) return 0;
  size_t n = 0;
  for (; in && *in; ++in) {
    char piece[8];
    const unsigned char ch = static_cast<unsigned char>(*in);
    if (ch == '"' || ch == '\\') snprintf(piece, sizeof piece, "\\%c", ch);
    else if (ch < 0x20) snprintf(piece, sizeof piece, "\\u%04x", ch);
    else snprintf(piece, sizeof piece, "%c", ch);
    const size_t plen = strlen(piece);
    if (n + plen + 1 > len) break;
    memcpy(out + n, piece, plen);
    n += plen;
  }
  out[n] = 0;
  return n;
}

}  // namespace web_core
