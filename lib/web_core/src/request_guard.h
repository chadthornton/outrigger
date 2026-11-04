// Which requests the web server answers at all. Checking Host blocks DNS
// rebinding (a foreign name pointed at the hub's IP); checking a WebSocket's
// Origin blocks other sites opening one from the user's browser.
#pragma once

namespace web_core {

struct HostNames {
  const char *mdns;     // "fm1hub.local"
  const char *own_ip;   // station IP, "" when not joined
  const char *ap_ip;    // setup hotspot IP, "" when the hotspot is off
};

// Host header: one of the names, any case, optional trailing dot and ":80".
bool host_allowed(const char *host, const HostNames &names);
// Origin header: "http://" + an allowed host. A missing Origin (curl, a
// script) is allowed: only browsers carry ambient credentials to abuse.
bool origin_allowed(const char *origin, const HostNames &names);

}  // namespace web_core
