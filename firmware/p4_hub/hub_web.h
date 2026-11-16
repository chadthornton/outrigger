// The diag web server on Big-P4 (E1): Wi-Fi through the C6 (esp-hosted), mDNS
// p4hub.local, esp_http_server on core 0.
//
// GET  /             the page (polls the routes below, shows the screenshot)
// GET  /status       JSON: build, uptime, heap, Wi-Fi, USB summary, screen
// GET  /log?since=N  JSON: numbered log lines (ours and ESP-IDF's)
// GET  /crash        JSON: reset reason, the last crash's record (NVS), core dump summary
// GET  /coredump     the stored core dump (header + ELF): esp_coredump info_corefile -t raw
// GET  /graph        JSON: the graph's state (hub_graph.h; a stub until HW2)
// GET  /screen.bmp   the screen, from the frame buffer the panel was sent
// GET  /usb          JSON: devices, cables, channels, per-cable counts, remount times
// Bench (lab network only; this is a spike build):
// POST /bench/replug        software replug of the whole bus
// POST /bench/echo?on=0|1   echo MIDI back to the sender (default off)
// POST /bench/reset         zero the USB counters
// GET  /bench/hubport?port=N        the external hub's port status bits
// POST /bench/hubport?port=N&power=0|1
// POST /bench/crash?kind=abort|wdt  crash on purpose (tests the crash record)
#pragma once
#include <stdint.h>

void hub_web_begin();
void hub_web_loop(uint32_t now_ms);
const char *hub_web_ip();
int hub_web_rssi();
