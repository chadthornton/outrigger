#include "hub_crash.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <string.h>

#include "json_out.h"

namespace {

constexpr uint32_t kMagic = 0xC0FFEE50;
constexpr int kTail = 40;
constexpr int kTailText = 96;

struct Rtc {
  uint32_t magic;
  uint32_t boots;
  uint32_t tail_next;
  char tail[kTail][kTailText];
  // Written by the panic handler.
  uint32_t panic_set;
  char panic_reason[48];
  uint32_t panic_pc;
  int32_t panic_core;
  uint32_t bt_len;
  uint32_t bt[16];
};
RTC_NOINIT_ATTR Rtc rtc;

esp_reset_reason_t reason = ESP_RST_UNKNOWN;
uint32_t boots_nvs = 0;
portMUX_TYPE tail_mux = portMUX_INITIALIZER_UNLOCKED;

bool abnormal(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
         r == ESP_RST_BROWNOUT || r == ESP_RST_CPU_LOCKUP;
}

const char *reset_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
  }
}

void on_panic(arduino_panic_info_t *info, void *) {
  rtc.panic_set = kMagic;
  strncpy(rtc.panic_reason, info->reason ? info->reason : "?", sizeof rtc.panic_reason - 1);
  rtc.panic_reason[sizeof rtc.panic_reason - 1] = 0;
  rtc.panic_pc = (uint32_t)info->pc;
  rtc.panic_core = info->core;
  rtc.bt_len = info->backtrace_len < 16 ? info->backtrace_len : 16;
  for (uint32_t i = 0; i < rtc.bt_len; ++i) rtc.bt[i] = info->backtrace[i];
}

// The record as JSON (what NVS keeps): reason, panic info, the tail.
size_t record_json(char *out, size_t cap) {
  JsonOut o{out, cap};
  o.raw("{\"reset_reason\":");
  o.str(reset_name(reason));
  o.raw(",\"boot\":%lu,\"saved_at_uptime_ms\":%lu", (unsigned long)boots_nvs, (unsigned long)millis());
  if (rtc.panic_set == kMagic) {
    o.raw(",\"panic\":{\"reason\":");
    o.str(rtc.panic_reason);
    o.raw(",\"core\":%ld,\"pc\":\"0x%08lx\",\"backtrace\":[", (long)rtc.panic_core, (unsigned long)rtc.panic_pc);
    for (uint32_t i = 0; i < rtc.bt_len && i < 16; ++i) o.raw("%s\"0x%08lx\"", i ? "," : "", (unsigned long)rtc.bt[i]);
    o.raw("]}");
  }
  o.raw(",\"tail\":[");
  bool first = true;
  for (int i = 0; i < kTail; ++i) {
    char line[kTailText];
    memcpy(line, rtc.tail[(rtc.tail_next + i) % kTail], sizeof line);
    line[sizeof line - 1] = 0;
    if (!line[0]) continue;
    if (!first) o.raw(",");
    first = false;
    o.str(line);
  }
  o.raw("]}");
  return o.len;
}

}  // namespace

void hub_crash_begin() {
  reason = esp_reset_reason();
  if (rtc.magic != kMagic) {
    memset(&rtc, 0, sizeof rtc);
    rtc.magic = kMagic;
  }
  ++rtc.boots;
  Preferences p;
  if (p.begin("crash", false)) {
    boots_nvs = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", boots_nvs);
    if (abnormal(reason)) {
      static char buf[6144];
      const size_t n = record_json(buf, sizeof buf);
      p.putBytes("rec", buf, n);
      p.putUInt("rec_boot", boots_nvs);
    }
    p.end();
  }
  rtc.panic_set = 0;
  set_arduino_panic_handler(on_panic, nullptr);
  char mark[kTailText];
  snprintf(mark, sizeof mark, "boot: %lu reset=%s", (unsigned long)boots_nvs, reset_name(reason));
  hub_crash_tail('B', 0, mark);
}

void hub_crash_tail(char source, uint32_t ms, const char *text) {
  portENTER_CRITICAL_SAFE(&tail_mux);
  char *slot = rtc.tail[rtc.tail_next % kTail];
  snprintf(slot, kTailText, "%lu %c %s", (unsigned long)ms, source, text);
  rtc.tail_next = (rtc.tail_next + 1) % kTail;
  portEXIT_CRITICAL_SAFE(&tail_mux);
}

const char *hub_reset_name() { return reset_name(reason); }
uint32_t hub_boot_count() { return boots_nvs; }

size_t hub_crash_json(char *out, size_t cap) {
  JsonOut o{out, cap};
  o.raw("{\"this_boot\":%lu,\"reset_reason\":", (unsigned long)boots_nvs);
  o.str(reset_name(reason));
  // The last saved record (NVS), from whichever boot crashed last.
  Preferences p;
  size_t n = 0;
  uint32_t rec_boot = 0;
  static char rec[6144];
  if (p.begin("crash", true)) {
    n = p.getBytes("rec", rec, sizeof rec - 1);
    rec_boot = p.getUInt("rec_boot", 0);
    p.end();
  }
  rec[n] = 0;
  o.raw(",\"last_crash_boot\":%lu,\"last_crash\":", (unsigned long)rec_boot);
  if (n) o.raw("%s", rec); else o.raw("null");
  // ESP-IDF's core dump in flash.
  o.raw(",\"coredump\":");
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) == ESP_OK && esp_core_dump_image_check() == ESP_OK) {
    static esp_core_dump_summary_t s;
    o.raw("{\"size\":%u", (unsigned)size);
    if (esp_core_dump_get_summary(&s) == ESP_OK) {
      o.raw(",\"task\":");
      o.str(s.exc_task);
      o.raw(",\"pc\":\"0x%08lx\",\"ra\":\"0x%08lx\",\"sp\":\"0x%08lx\",\"mcause\":\"0x%08lx\",\"mtval\":\"0x%08lx\"",
            (unsigned long)s.exc_pc, (unsigned long)s.ex_info.ra, (unsigned long)s.ex_info.sp,
            (unsigned long)s.ex_info.mcause, (unsigned long)s.ex_info.mtval);
      char sha[sizeof s.app_elf_sha256 + 1];
      memcpy(sha, s.app_elf_sha256, sizeof s.app_elf_sha256);
      sha[sizeof s.app_elf_sha256] = 0;
      o.raw(",\"elf_sha256\":");
      o.str(sha);
    }
    o.raw("}");
  } else {
    o.raw("null");
  }
  o.raw("}");
  return o.len;
}
