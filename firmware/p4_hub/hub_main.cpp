// Big-P4 hub firmware: setup and the Arduino loop. See hub_web.h for the
// routes, hub_usb.h for the host, hub_crash.h for why the crash record is in NVS.
#include <Arduino.h>

#include "hub_crash.h"
#include "hub_graph.h"
#include "hub_log.h"
#include "hub_screen.h"
#include "hub_usb.h"
#include "hub_web.h"

// esp-hosted's Wi-Fi init overflows the default 8 KB loop stack (measured:
// "Stack protection fault" in WiFi.mode(), prototypes/p4_wifi_scan).
SET_LOOP_TASK_STACK_SIZE(32 * 1024)

#define HUB_STR2(x) #x
#define HUB_STR(x) HUB_STR2(x)
#ifdef HUB_GIT_SHA_RAW
#define HUB_GIT_SHA HUB_STR(HUB_GIT_SHA_RAW)
#else
#define HUB_GIT_SHA "dev"
#endif

void setup() {
  Serial.begin(115200);
  hub_crash_begin();
  hub_log_begin();
  HLOG_MAIN("P4HUB build=%s boot=%lu reset=%s", HUB_GIT_SHA, (unsigned long)hub_boot_count(), hub_reset_name());
  HLOG_MAIN("SCREEN begin=%s", hub_screen_begin() ? "ok" : "fail");
  graph_begin();
  hub_usb_begin();
  hub_web_begin();
  HLOG_MAIN("P4HUB ready");
}

void loop() {
  const uint32_t now = millis();
  hub_usb_loop(now);
  hub_web_loop(now);
  static uint32_t last_draw = 0, last_alive = 0;
  if (now - last_draw >= 1000) {
    last_draw = now;
    static char l[16][48];
    const char *lines[16];
    int n = 0;
    snprintf(l[n++], 48, "Big-P4 hub %s", HUB_GIT_SHA);
    snprintf(l[n++], 48, "up %lu.%lus", (unsigned long)(now / 1000), (unsigned long)(now / 100 % 10));
    snprintf(l[n++], 48, "%s %ddBm", hub_web_ip()[0] ? hub_web_ip() : "no wifi", hub_web_rssi());
    snprintf(l[n++], 48, "usb %u dev, ch %u/%u", hub_usb_device_count(), hub_usb_channels_used(),
             hub_usb_channels_max());
    n += hub_usb_lines(&l[n], 16 - n);
    for (int i = 0; i < n; ++i) lines[i] = l[i];
    hub_screen_draw(lines, n);
  }
  if (now - last_alive >= 30000) {
    last_alive = now;
    HLOG_MAIN("ALIVE up=%lus heap=%u psram=%u usb=%u ch=%u/%u", (unsigned long)(now / 1000),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
              hub_usb_device_count(), hub_usb_channels_used(), hub_usb_channels_max());
  }
  delay(5);
}
