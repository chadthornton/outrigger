#include "wifi_qr.h"

#include <string.h>

namespace web_core {

namespace {

// Appends s (escaped when asked) at out[n]; false when it runs past cap.
bool put(char *out, size_t cap, size_t &n, const char *s, bool escape) {
  for (; *s; ++s) {
    if (escape && strchr("\\;,:\"", *s)) {
      if (n + 1 >= cap) return false;
      out[n++] = '\\';
    }
    if (n + 1 >= cap) return false;
    out[n++] = *s;
  }
  return true;
}

}  // namespace

bool wifi_qr_payload(const char *ssid, const char *pass, char *out, size_t cap) {
  if (!out || cap == 0) return false;
  size_t n = 0;
  const bool ok = put(out, cap, n, "WIFI:T:WPA;S:", false) && put(out, cap, n, ssid, true) &&
                  put(out, cap, n, ";P:", false) && put(out, cap, n, pass, true) && put(out, cap, n, ";;", false);
  out[ok ? n : 0] = 0;
  return ok;
}

}  // namespace web_core
