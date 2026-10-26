// The web control panel at http://fm1hub.local: Wi-Fi (with a setup hotspot
// when no network is saved), mDNS, and an esp_http_server serving the page, a
// JSON API and a WebSocket with live status and a MIDI monitor.
//
// Threads: web_monitor() runs on the USB task and only pushes into a lock-free
// ring. Everything else is called from the Arduino loop. The server's own task
// (core 0, priority 3, below the USB tasks) answers requests and drains the
// ring; it never calls LVGL or midiSend().
#pragma once

#include "host_ui.h"
#include "monitor_ring.h"

void web_begin(const UiSettings &settings);
void web_loop(uint32_t now_ms);
// The device's own settings page changed them.
void web_settings_changed(const UiSettings &settings);
// Settings the page changed, for the loop to apply and save. False if none.
bool web_take_settings(UiSettings &out);
// Fills the frame's network lines and banner, and publishes it to the page.
void web_frame(UiFrame &frame);
// USB task: one MIDI event for the monitor. Never blocks.
void web_monitor(const web_core::MonitorEvent &event);
