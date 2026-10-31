#include "request_guard.h"

#include <string.h>
#include <strings.h>

namespace web_core {

static bool same_name(const char *host, size_t len, const char *name) {
  if (!name || !name[0]) return false;
  return strlen(name) == len && strncasecmp(host, name, len) == 0;
}

bool host_allowed(const char *host, const HostNames &names) {
  if (!host || !host[0]) return false;
  size_t len = strlen(host);
  if (len > 3 && strcmp(host + len - 3, ":80") == 0) len -= 3;
  if (len > 1 && host[len - 1] == '.') --len;
  return same_name(host, len, names.mdns) || same_name(host, len, names.own_ip) ||
         same_name(host, len, names.ap_ip);
}

bool origin_allowed(const char *origin, const HostNames &names) {
  if (!origin) return true;
  static const char kScheme[] = "http://";
  if (strncasecmp(origin, kScheme, sizeof kScheme - 1) != 0) return false;
  return host_allowed(origin + sizeof kScheme - 1, names);
}

}  // namespace web_core
