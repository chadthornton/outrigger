#include "hub_web.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_partition.h>

#include "hub_crash.h"
#include "hub_graph.h"
#include "hub_log.h"
#include "hub_screen.h"
#include "hub_usb.h"
#include "json_out.h"

#define HUB_STR2(x) #x
#define HUB_STR(x) HUB_STR2(x)
#ifdef HUB_GIT_SHA_RAW
#define HUB_GIT_SHA HUB_STR(HUB_GIT_SHA_RAW)
#else
#define HUB_GIT_SHA "dev"
#endif

namespace {

httpd_handle_t server = nullptr;
char ip[20] = "";
int rssi = 0;
uint32_t wifi_joins = 0, wifi_join_ms = 0, wifi_lost = 0;
bool was_connected = false;
uint32_t join_started = 0;
char *jbuf = nullptr;  // PSRAM; the server task answers one request at a time
constexpr size_t kJson = 32768;
uint16_t *shot = nullptr;
uint32_t requests = 0;

const char kPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>p4hub diag</title><style>body{font:13px ui-monospace,monospace;margin:12px;background:#111;color:#ddd}
h2{font-size:14px;margin:14px 0 4px;color:#9cf}pre{background:#1b1b1b;padding:8px;overflow:auto;max-height:340px;margin:0}
.row{display:flex;gap:16px;flex-wrap:wrap}.col{flex:1;min-width:320px}img{border:1px solid #333;width:240px}</style></head><body>
<div>Big-P4 hub diagnostics &middot; <span id="age"></span> &middot; <a href="/coredump">coredump</a></div>
<div class="row"><div class="col"><h2>status</h2><pre id="status"></pre><h2>usb</h2><pre id="usb"></pre></div>
<div class="col"><h2>screen</h2><img id="shot"><h2>graph</h2><pre id="graph"></pre><h2>crash</h2><pre id="crash"></pre></div></div>
<h2>log</h2><pre id="log"></pre>
<script>let since=0,lines=[];const j=u=>fetch(u).then(r=>r.json());
async function tick(){try{for(const k of["status","usb","graph","crash"])document.getElementById(k).textContent=JSON.stringify(await j("/"+k),null,1);
const l=await j("/log?since="+since);since=l.next-1;for(const x of l.lines)lines.push(x[0]+" "+x[1]+" "+x[2]+" "+x[3]);lines=lines.slice(-300);
const e=document.getElementById("log");e.textContent=lines.join("\n");e.scrollTop=1e9;
document.getElementById("shot").src="/screen.bmp?"+Date.now();document.getElementById("age").textContent=new Date().toLocaleTimeString()}catch(x){document.getElementById("age").textContent="offline "+x}}
tick();setInterval(tick,3000)</script></body></html>)HTML";

esp_err_t send_json(httpd_req_t *req, const char *body, size_t n) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, n);
}

bool query_int(httpd_req_t *req, const char *key, long &out) {
  char q[64], v[16];
  if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
  if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return false;
  out = strtol(v, nullptr, 10);
  return true;
}

esp_err_t on_index(httpd_req_t *req) {
  ++requests;
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kPage, sizeof kPage - 1);
}

esp_err_t on_status(httpd_req_t *req) {
  ++requests;
  JsonOut o{jbuf, kJson};
  multi_heap_info_t in{}, ps{};
  heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
  heap_caps_get_info(&ps, MALLOC_CAP_SPIRAM);
  o.raw("{\"board\":\"Big-P4\",\"build\":\"%s\",\"built\":\"%s %s\",\"uptime_ms\":%lu,\"boot\":%lu,\"reset_reason\":",
        HUB_GIT_SHA, __DATE__, __TIME__, (unsigned long)millis(), (unsigned long)hub_boot_count());
  o.str(hub_reset_name());
  o.raw(",\"mac\":\"%s\",\"cpu_mhz\":%lu,\"chip_rev\":%u", WiFi.macAddress().c_str(), (unsigned long)getCpuFrequencyMhz(),
        (unsigned)ESP.getChipRevision());
  o.raw(",\"heap\":{\"internal_free\":%u,\"internal_min\":%u,\"internal_largest\":%u,\"psram_free\":%u,\"psram_min\":%u}",
        (unsigned)in.total_free_bytes, (unsigned)in.minimum_free_bytes, (unsigned)in.largest_free_block,
        (unsigned)ps.total_free_bytes, (unsigned)ps.minimum_free_bytes);
  o.raw(",\"tasks\":%u,\"wifi\":{\"ip\":\"%s\",\"rssi\":%d,\"joins\":%lu,\"join_ms\":%lu,\"lost\":%lu}",
        (unsigned)uxTaskGetNumberOfTasks(), ip, rssi, (unsigned long)wifi_joins, (unsigned long)wifi_join_ms,
        (unsigned long)wifi_lost);
  const HubLogCounters c = hub_log_counters();
  o.raw(",\"usb\":{\"devices\":%u,\"attached\":%s,\"channels_used\":%u,\"channels_max\":%u,\"short_desc_failed\":%lu,"
        "\"port_disabled\":%lu,\"echo\":%s}",
        hub_usb_device_count(), hub_usb_device_count() ? "true" : "false", hub_usb_channels_used(),
        hub_usb_channels_max(), (unsigned long)c.short_desc_failed, (unsigned long)c.port_disabled,
        hub_usb_echo() ? "true" : "false");
  o.raw(",\"screen\":{\"frames\":%lu,\"flush_us\":%lu},\"log\":{\"next\":%lu},\"http_requests\":%lu}",
        (unsigned long)hub_screen_frames(), (unsigned long)hub_screen_flush_us(), (unsigned long)hub_log_next(),
        (unsigned long)requests);
  return send_json(req, jbuf, o.len);
}

esp_err_t on_log(httpd_req_t *req) {
  ++requests;
  long since = 0;
  query_int(req, "since", since);
  const size_t n = hub_log_json(since < 0 ? 0 : (uint32_t)since, jbuf, kJson);
  return send_json(req, jbuf, n);
}

esp_err_t on_crash(httpd_req_t *req) {
  ++requests;
  return send_json(req, jbuf, hub_crash_json(jbuf, kJson));
}

esp_err_t on_graph(httpd_req_t *req) {
  ++requests;
  return send_json(req, jbuf, graph_state_json(jbuf, kJson));
}

esp_err_t on_usb(httpd_req_t *req) {
  ++requests;
  return send_json(req, jbuf, hub_usb_json(jbuf, kJson));
}

esp_err_t on_coredump(httpd_req_t *req) {
  ++requests;
  const esp_partition_t *p =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!p) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no coredump partition");
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK || !size)
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no core dump stored");
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.elf\"");
  const size_t off0 = addr - p->address;
  for (size_t off = 0; off < size; off += 4096) {
    const size_t n = size - off < 4096 ? size - off : 4096;
    if (esp_partition_read(p, off0 + off, jbuf, n) != ESP_OK) break;
    if (httpd_resp_send_chunk(req, jbuf, n) != ESP_OK) return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// 16-bit BMP (BI_BITFIELDS, RGB565), top-down.
esp_err_t on_screen(httpd_req_t *req) {
  ++requests;
  if (!shot || !hub_screen_snapshot(shot)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no screen");
  const uint32_t pixels = kScreenW * kScreenH * 2;
  uint8_t h[66] = {'B', 'M'};
  auto u32 = [&](int at, uint32_t v) { memcpy(h + at, &v, 4); };
  auto u16 = [&](int at, uint16_t v) { memcpy(h + at, &v, 2); };
  u32(2, 66 + pixels);
  u32(10, 66);
  u32(14, 40);
  u32(18, kScreenW);
  u32(22, (uint32_t)(-kScreenH));
  u16(26, 1);
  u16(28, 16);
  u32(30, 3);  // BI_BITFIELDS
  u32(34, pixels);
  u32(54, 0xF800);
  u32(58, 0x07E0);
  u32(62, 0x001F);
  httpd_resp_set_type(req, "image/bmp");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (httpd_resp_send_chunk(req, (const char *)h, sizeof h) != ESP_OK) return ESP_FAIL;
  const char *p = (const char *)shot;
  for (uint32_t off = 0; off < pixels; off += 8192) {
    const uint32_t n = pixels - off < 8192 ? pixels - off : 8192;
    if (httpd_resp_send_chunk(req, p + off, n) != ESP_OK) return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t ok(httpd_req_t *req, const char *what) {
  JsonOut o{jbuf, kJson};
  o.raw("{\"ok\":true,\"did\":");
  o.str(what);
  o.raw("}");
  return send_json(req, jbuf, o.len);
}

esp_err_t on_bench_replug(httpd_req_t *req) {
  ++requests;
  hub_usb_request_replug();
  return ok(req, "replug");
}

esp_err_t on_bench_echo(httpd_req_t *req) {
  ++requests;
  long on = 1;
  query_int(req, "on", on);
  hub_usb_set_echo(on != 0);
  return ok(req, on ? "echo on" : "echo off");
}

esp_err_t on_bench_reset(httpd_req_t *req) {
  ++requests;
  hub_usb_reset_counters();
  return ok(req, "counters reset");
}

esp_err_t on_bench_hubport(httpd_req_t *req) {
  ++requests;
  long port = 0, power = -1;
  if (!query_int(req, "port", port) || port < 1 || port > 15)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "port=1..15");
  JsonOut o{jbuf, kJson};
  if (req->method == HTTP_POST && query_int(req, "power", power)) {
    const int r = hub_usb_port_power((uint8_t)port, power != 0);
    o.raw("{\"port\":%ld,\"power\":%ld,\"result\":%d}", port, power, r);
  } else {
    uint16_t st = 0, ch = 0;
    const int r = hub_usb_port_status((uint8_t)port, st, ch);
    o.raw("{\"port\":%ld,\"result\":%d,\"status\":\"0x%04x\",\"change\":\"0x%04x\",\"connected\":%u,\"enabled\":%u,"
          "\"suspend\":%u,\"overcurrent\":%u,\"reset\":%u,\"power\":%u,\"low_speed\":%u,\"high_speed\":%u}",
          port, r, st, ch, st & 1, (st >> 1) & 1, (st >> 2) & 1, (st >> 3) & 1, (st >> 4) & 1, (st >> 8) & 1,
          (st >> 9) & 1, (st >> 10) & 1);
  }
  return send_json(req, jbuf, o.len);
}

void crash_task(void *arg) {
  const long kind = (long)arg;
  vTaskDelay(pdMS_TO_TICKS(300));  // let the reply go out first
  HLOG_MAIN("BENCH crash on purpose kind=%ld", kind);
  vTaskDelay(pdMS_TO_TICKS(100));
  if (kind == 1) {
    portDISABLE_INTERRUPTS();
    for (;;) {}
  }
  abort();
}

esp_err_t on_bench_crash(httpd_req_t *req) {
  ++requests;
  char q[48], v[12] = "abort";
  if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) httpd_query_key_value(q, "kind", v, sizeof v);
  const long kind = strcmp(v, "wdt") == 0 ? 1 : 0;
  xTaskCreatePinnedToCore(crash_task, "crash", 4096, (void *)kind, 2, nullptr, 1);
  return ok(req, kind ? "crash wdt in 0.4 s" : "crash abort in 0.4 s");
}

void start_server() {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.core_id = 0;
  c.task_priority = 3;
  c.stack_size = 8192;
  c.max_uri_handlers = 16;
  c.lru_purge_enable = true;
  c.server_port = 80;
  if (httpd_start(&server, &c) != ESP_OK) {
    HLOG_NET("HTTPD start failed");
    return;
  }
  struct R {
    const char *uri;
    httpd_method_t m;
    esp_err_t (*h)(httpd_req_t *);
  } routes[] = {
      {"/", HTTP_GET, on_index},           {"/status", HTTP_GET, on_status},
      {"/log", HTTP_GET, on_log},          {"/crash", HTTP_GET, on_crash},
      {"/coredump", HTTP_GET, on_coredump}, {"/graph", HTTP_GET, on_graph},
      {"/screen.bmp", HTTP_GET, on_screen}, {"/usb", HTTP_GET, on_usb},
      {"/bench/replug", HTTP_POST, on_bench_replug}, {"/bench/echo", HTTP_POST, on_bench_echo},
      {"/bench/reset", HTTP_POST, on_bench_reset},   {"/bench/hubport", HTTP_GET, on_bench_hubport},
      {"/bench/hubport", HTTP_POST, on_bench_hubport}, {"/bench/crash", HTTP_POST, on_bench_crash},
  };
  for (const R &r : routes) {
    httpd_uri_t u{};
    u.uri = r.uri;
    u.method = r.m;
    u.handler = r.h;
    httpd_register_uri_handler(server, &u);
  }
  HLOG_NET("HTTPD up port=80 routes=%u", (unsigned)(sizeof routes / sizeof routes[0]));
}

}  // namespace

void hub_web_begin() {
  jbuf = static_cast<char *>(heap_caps_malloc(kJson, MALLOC_CAP_SPIRAM));
  shot = static_cast<uint16_t *>(heap_caps_malloc(kScreenW * kScreenH * 2, MALLOC_CAP_SPIRAM));
  char ssid[33] = "", pass[65] = "";
  Preferences p;
  if (p.begin("wifi", true)) {
    strlcpy(ssid, p.getString("ssid", "").c_str(), sizeof ssid);
    strlcpy(pass, p.getString("pass", "").c_str(), sizeof pass);
    p.end();
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("p4hub");
  WiFi.setAutoReconnect(true);
  if (!ssid[0]) {
    HLOG_NET("WIFI no saved network (NVS wifi/ssid); web off");
    return;
  }
  join_started = millis();
  WiFi.begin(ssid, pass);
  memset(pass, 0, sizeof pass);
  HLOG_NET("WIFI joining saved network");
  start_server();
}

void hub_web_loop(uint32_t now_ms) {
  static uint32_t last = 0;
  if (now_ms - last < 500) return;
  last = now_ms;
  const bool up = WiFi.status() == WL_CONNECTED;
  if (up && !was_connected) {
    ++wifi_joins;
    wifi_join_ms = now_ms - join_started;
    strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof ip);
    HLOG_NET("WIFI up ip=%s rssi=%d after %lu ms", ip, (int)WiFi.RSSI(), (unsigned long)wifi_join_ms);
    if (wifi_joins == 1) {
      if (MDNS.begin("p4hub")) {
        MDNS.addService("http", "tcp", 80);
        HLOG_NET("MDNS p4hub.local");
      } else {
        HLOG_NET("MDNS failed");
      }
    }
  } else if (!up && was_connected) {
    ++wifi_lost;
    join_started = now_ms;
    HLOG_NET("WIFI lost");
    ip[0] = 0;
  }
  was_connected = up;
  if (up) rssi = WiFi.RSSI();
}

const char *hub_web_ip() { return ip; }
int hub_web_rssi() { return rssi; }
