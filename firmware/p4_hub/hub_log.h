// The diag log behind GET /log. Any task (and ESP-IDF's own ESP_LOGx, through
// esp_log_set_vprintf) appends a line; a spinlock guards the ring, so it is safe
// from every task and both cores, never allocates, and never blocks for long.
// Each line is numbered so pollers can ask for ?since=N. Lines also go to the
// UART console, and the newest kTailLines into RTC memory (hub_crash.cpp) so a
// panic's last lines survive the reset.
#pragma once
#include <stddef.h>
#include <stdint.h>

void hub_log_begin();
void hub_log(char source, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Appends {"next":N,"lost":L,"lines":[[seq,ms,"S","text"],...]} after `since`.
size_t hub_log_json(uint32_t since, char *out, size_t cap);
uint32_t hub_log_next();
uint32_t hub_log_dropped_long();
// Counts of ESP-IDF log lines that matched the USB enumeration failures.
struct HubLogCounters {
  uint32_t short_desc_failed;  // "CHECK_SHORT_DEV_DESC FAILED"
  uint32_t port_disabled;      // "Port disabled"
  uint32_t enum_failed_other;  // any other "ENUM:" line logged as an error
};
HubLogCounters hub_log_counters();

#define HLOG_USB(...) hub_log('U', __VA_ARGS__)
#define HLOG_MAIN(...) hub_log('M', __VA_ARGS__)
#define HLOG_NET(...) hub_log('N', __VA_ARGS__)
