// Read-only web diagnostics at http://fm1hub.local (the Web UI app's first
// slice): Wi-Fi with a setup hotspot when no network is saved, mDNS, and an
// esp_http_server on core 0 at priority 3, below the USB host tasks (5).
// Routes: lib/web_core/src/routes.h. Built with APP_WEB_UI.
//
// Threads: web_begin(), web_loop() and web_publish() run on the Arduino loop.
// The server's task answers requests from copies the loop and the loader task
// publish under a mutex; it never calls EspUsbHost, the broker, the Library,
// LittleFS or LVGL.
#pragma once

#include "apps.h"
#if APP_WEB_UI

#include "broker.h"
#include "host_ui.h"

struct DiagDevice {
  uint8_t address;
  uint16_t vid, pid;
  bool hub;
  char name[24];
  uint8_t channels;              // host channels it costs (EP0 + claimed endpoints)
  uint8_t interfaces, claimed;
  platform::Reason refusal;      // empty when the broker claimed what it wanted
};

// What the loop knows, published a few times a second.
struct DiagState {
  bool host_running;
  platform::Mode mode;
  UiFrame frame;
  DiagDevice devices[8];
  uint8_t device_count;
  uint32_t failed_sends;         // midiSend() refused a forwarded message
  bool has_loader;
  UiLoader loader;               // the Firmware page
};

// safe_mode: the board started without the USB host after a crash. Wi-Fi
// still comes up so /crash and /coredump can be read, unless the boot before
// was safe mode with Wi-Fi that crashed too.
void web_begin(bool safe_mode);
void web_loop(uint32_t now_ms);
void web_publish(const DiagState &state);
#if BENCH
#include "bench.h"
// The command the Mac posted to /bench/select or /bench/act, once: the loop
// presses it (app.cpp). False when none is waiting.
bool web_take_bench_command(web_core::BenchCommand &out);
#endif

// The settings page's network line: address, or the hotspot and its password.
const char *web_network_line();
// The setup hotspot as a Wi-Fi QR code's text while that line says to join it; else empty.
const char *web_join_qr();

#endif  // APP_WEB_UI
