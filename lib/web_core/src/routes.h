// Every route the web server answers. Diagnostics only: everything is GET, so
// no request can start a backup, an install or a pin, or change a setting. The
// one exception is the setup hotspot's join, which saves a Wi-Fi network and
// is answered only on the hotspot's own interface (its password is on the
// device's screen), never from the LAN. tests/test_web_core.cpp holds this.
// The test bench's POST routes are a separate table (bench.h), compiled only
// into BENCH=1 builds and answered only on a listed spare.
#pragma once

namespace web_core {

enum class Method { Get, Post };

struct Route {
  const char *uri;
  Method method;
  bool hotspot_only;
};

constexpr Route kRoutes[] = {
    {"/", Method::Get, false},           // the page, or the setup page on the hotspot
    {"/status", Method::Get, false},     // JSON: build, heap, tasks, USB, Firmware page
    {"/log", Method::Get, false},        // JSON: ?since=N, numbered lines
    {"/crash", Method::Get, false},      // JSON: the last abnormal reset's breadcrumbs
    {"/coredump", Method::Get, false},   // raw coredump partition, for esp-coredump
    {"/store", Method::Get, false},      // JSON: units, backups, saves, packages
    {"/setup/scan", Method::Get, true},  // JSON: networks the hotspot saw
    {"/setup/state", Method::Get, true}, // JSON: how the page's join is going
    {"/setup/join", Method::Post, true}, // saves a network to NVS `wifi`, then joins it
};
constexpr int kRouteCount = sizeof kRoutes / sizeof kRoutes[0];

}  // namespace web_core
