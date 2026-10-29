// Crash breadcrumbs that survive a reboot (RTC memory, not cleared by a panic
// or watchdog reset): each task notes the step it is in, and after an abnormal
// reset the Firmware page shows the reason and the last steps. On this board
// the USB port is the host, so there's no serial log to read a crash from.
#pragma once

#include <esp_attr.h>
#include <esp_system.h>
#include <string.h>

// The last log lines (diag_log, web builds) are kept here too, across every
// reset but a power-on (a panic, RST, the job boot's esp_restart), so GET /crash
// has them after the RAM log is gone. "boot:" lines mark where each boot began.
// 40 lines: a whole job boot (it ends in esp_restart, which wipes the RAM log).
constexpr int kCrumbTail = 40;
constexpr int kCrumbLine = 96;
struct Crumbs {
  uint32_t magic;
  char loop[24], loader[24], usb[24];
  uint32_t tail_next;
  char tail[kCrumbTail][kCrumbLine];
};
extern Crumbs crumbs;
constexpr uint32_t kCrumbMagic = 0xC0FFEE43;

inline void crumb(char *slot, const char *where) { strncpy(slot, where, 23); slot[23] = 0; }
#define CRUMB_LOOP(w) crumb(crumbs.loop, w)
#define CRUMB_LOADER(w) crumb(crumbs.loader, w)
#define CRUMB_USB(w) crumb(crumbs.usb, w)
