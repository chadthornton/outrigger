#include "apps.h"
#if APP_WEB_UI

#include "web.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include <stdarg.h>

#include <atomic>
#include <new>
#include <utility>

#include "crumbs.h"
#include "diag.h"
#include "hotspot_channel.h"
#include "wifi_fallback.h"
#include "json_field.h"
#include "log_ring.h"
#include "request_guard.h"
#include "routes.h"
#include <esp_mac.h>
#include "bench.h"  // web_host_name in every build; the bench API only with BENCH=1
#ifndef BENCH
#define BENCH 0
#endif

using web_core::json_escape;

extern char last_crash[3][40];  // app.cpp: the last abnormal reset, if any
#if __has_include("fw_version.h")
#include "fw_version.h"  // app.cpp says why
#endif
#ifndef FW_VERSION
#define FW_VERSION 0
#endif
#ifndef FW_DIRTY
#define FW_DIRTY 0
#endif

// The board's network name, set once in web_begin from its MAC: each of Chad's
// boards answers at its own name (small-v1.local ...), any other board at
// fm1hub.local, or fm1bench.local in a BENCH=1 build so soak_watch.py and
// bookmarks never reach a bench by mistake (web_core::web_host_name).
static char host_name[16] = "fm1hub", mdns_name[24] = "fm1hub.local", setup_ssid[24] = "fm1hub-setup";
static const char kHostMark[] = "@HOST@";  // in kSetupHtml: replaced by host_name as it's sent

// ---------- The log: one ring per producing task, drained by the server task ----------
static web_core::LineRing<32> *usb_ring;
static web_core::LineRing<64> *loader_ring;
static web_core::LineRing<32> *loop_ring;
static web_core::LogHistory<512> *history;  // server task only

void diag_log(char source, const char *fmt, ...) {
  char text[web_core::kLogText];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof text, fmt, args);
  va_end(args);
  const uint32_t now = millis();
  // Also into RTC memory, which a panic leaves alone (crumbs.h).
  const uint32_t slot = __atomic_fetch_add(&crumbs.tail_next, 1, __ATOMIC_RELAXED) % kCrumbTail;
  snprintf(crumbs.tail[slot], sizeof crumbs.tail[slot], "%lu %c %s", (unsigned long)now, source, text);
  switch (source) {
    case 'U': if (usb_ring) usb_ring->push(now, text); break;
    case 'L': if (loader_ring) loader_ring->push(now, text); break;
    default: if (loop_ring) loop_ring->push(now, text); break;
  }
}

static void drain_log() {
  const web_core::LineRingRef rings[] = {{usb_ring, 'U'}, {loader_ring, 'L'}, {loop_ring, 'M'}};
  if (history) web_core::drain(rings, 3, *history);
}

// ---------- State the loop and the loader publish, under state_mutex ----------
static SemaphoreHandle_t state_mutex;
struct Lock {
  Lock() { xSemaphoreTake(state_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(state_mutex); }
};

static DiagState *published;    // PSRAM
static DiagState *status_copy;  // PSRAM, server task only
static uint32_t published_at = 0;
struct TaskLine {
  char name[configMAX_TASK_NAME_LEN];
  uint32_t stack_free;  // bytes never used: the high-water mark
  uint8_t priority;
};
constexpr int kMaxTasks = 48;
static TaskLine tasks[kMaxTasks];
static uint8_t task_count = 0;
static std::string *store_body;  // GET /store
static uint32_t store_at = 0;

enum class Net : uint8_t { Connecting, Online };
static Net net = Net::Connecting;
static bool ap_on = false;
static char own_ip[16] = "", ap_ip[16] = "", ssid_now[33] = "", ap_pass[13] = "";
static uint32_t ap_ip_u32 = 0;  // network byte order, for the via-hotspot check
static int8_t rssi = 0;
static char network_line[48] = "";  // empty until web_begin()
static char scan_json[1200] = "{\"nets\":[]}";
static char creds_ssid[33], creds_pass[65];
static bool creds_pending = false;

// ---------- Loop-only state ----------
static bool started = false, safe = false;
static Preferences wifi_prefs;
static httpd_handle_t server = nullptr;
static DNSServer dns;
static bool mdns_on = false, joining_from_setup = false;
static web_core::WifiFallback fallback;  // nudges, the hotspot and retries of the saved network
static uint32_t join_at = 0, connect_started = 0, online_since = 0, last_drain = 0;
// The station's last disconnect reason, from Wi-Fi's event task; the loop logs
// it. The first join after a boot sometimes stalls with no reason at all
// (2026-10-30), and these lines say which kind of failure a slow boot was.
static std::atomic<uint8_t> disconnect_reason{0};
static std::atomic<uint32_t> disconnects{0};
static volatile bool drain_queued = false;
struct Seen {
  char ssid[33];
  int8_t rssi;
  uint8_t channel;
  uint8_t bssid[6];
};
static Seen seen[24];
static int seen_count = 0;
static uint8_t hotspot_channel = 0;
static char saved_ssid[33], saved_pass[65];
static char trial_ssid[33], trial_pass[65];  // from the setup page; saved once they work

// A safe-mode boot that brought Wi-Fi up sets this; if that boot crashes too,
// the next safe mode stays off the radio. RTC memory survives the reset.
RTC_NOINIT_ATTR static uint32_t safe_wifi_marker;
constexpr uint32_t kSafeWifiMagic = 0x5AFE3141;

// ---------- JSON output into a fixed buffer ----------
struct Out {
  char *buf;
  size_t len, k = 0;
  void raw(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (k + 1 >= len) return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf + k, len - k, fmt, args);
    va_end(args);
    k = n < 0 ? k : k + size_t(n) < len ? k + n : len - 1;
  }
  void str(const char *s) {  // a quoted, escaped string
    char e[256];
    json_escape(s, e, sizeof e);
    raw("\"%s\"", e);
  }
  void key_str(const char *key, const char *s, bool comma = true) {
    raw("%s\"%s\":", comma ? "," : "", key);
    str(s);
  }
};

// An age in ms as a number, or null for never (UINT32_MAX).
static void age_field(Out &o, const char *key, uint32_t ms) {
  if (ms == UINT32_MAX) o.raw(",\"%s\":null", key);
  else o.raw(",\"%s\":%lu", key, (unsigned long)ms);
}

// ---------- Request helpers (server task) ----------
static bool header(httpd_req_t *req, const char *name, char *out, size_t len) {
  const size_t n = httpd_req_get_hdr_value_len(req, name);
  if (n == 0 || n >= len) return false;
  return httpd_req_get_hdr_value_str(req, name, out, len) == ESP_OK;
}

// The local address the request came in on is the hotspot's.
static bool via_hotspot(httpd_req_t *req) {
  struct sockaddr_storage addr;
  socklen_t len = sizeof addr;
  if (getsockname(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&addr), &len) != 0) return false;
  uint32_t ip = 0;
  if (addr.ss_family == AF_INET) {
    ip = reinterpret_cast<sockaddr_in *>(&addr)->sin_addr.s_addr;
  } else if (addr.ss_family == AF_INET6) {
    const sockaddr_in6 *a6 = reinterpret_cast<sockaddr_in6 *>(&addr);
    memcpy(&ip, &a6->sin6_addr.un.u32_addr[3], 4);  // IPv4-mapped
  }
  Lock l;
  return ap_on && ip == ap_ip_u32;
}

static esp_err_t redirect_to_setup(httpd_req_t *req) {
  char where[40];
  {
    Lock l;
    snprintf(where, sizeof where, "http://%s/", ap_ip);
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", where);
  return httpd_resp_send(req, nullptr, 0);
}

// Every handler starts here: a foreign Host gets nothing (DNS rebinding), or a
// redirect to the setup page when it came through the hotspot (captive portal).
static bool guard(httpd_req_t *req) {
  char host[64] = "";
  header(req, "Host", host, sizeof host);
  bool ok;
  {
    Lock l;
    ok = web_core::host_allowed(host, {mdns_name, own_ip, ap_ip});
  }
  if (ok) return true;
  if (via_hotspot(req)) redirect_to_setup(req);
  else httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "unknown host");
  return false;
}

static esp_err_t send_json(httpd_req_t *req, const char *json, size_t len) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, len);
}

static char *json_buf() {  // server task only: one request at a time
  static char *buf = static_cast<char *>(heap_caps_malloc(16384, MALLOC_CAP_SPIRAM));
  return buf;
}
constexpr size_t kJsonBuf = 16384;

static const char *mode_name(platform::Mode m) {
  return m == platform::Mode::Load ? "load" : m == platform::Mode::Record ? "record" : "play";
}

static const char *reset_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
  }
}

// ---------- Pages ----------
static const char kIndexHtml[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>fm1hub diagnostics</title>
<style>
body{font:13px ui-monospace,Menlo,monospace;margin:12px;background:#111;color:#ddd}
h2{font-size:14px;color:#8cf;margin:14px 0 4px}pre{margin:0;white-space:pre-wrap}
.bad{color:#f66}.warn{color:#fd4}.ok{color:#6d6}a{color:#8cf}
#log{max-height:45vh;overflow:auto;border:1px solid #333;padding:4px}
</style></head><body>
<div>fm1hub diagnostics, read only. <span id="age"></span> &middot; <a href="/coredump">coredump</a></div>
<h2>Board</h2><pre id="board"></pre>
<h2>USB</h2><pre id="usb"></pre>
<h2>Firmware page</h2><pre id="fw"></pre>
<h2>Last crash</h2><pre id="crash"></pre>
<h2>Store</h2><pre id="store"></pre>
<h2>Log</h2><pre id="log"></pre>
<script>
let since=0,last=0;const $=id=>document.getElementById(id);
const kb=b=>(b/1024).toFixed(0)+' KB';
async function get(u){const r=await fetch(u,{cache:'no-store'});return r.json()}
function board(s){const h=s.heap;let t=`commit ${s.build.commit}${s.build.dirty?'+':''} ${s.build.apps}`+
 ` writes ${s.build.writes?'ON':'off'}  up ${(s.uptime_ms/1000).toFixed(0)} s\n`+
 `internal ${kb(h.internal_free)} (min ${kb(h.internal_min)}), DMA largest ${kb(h.dma_largest)} (free ${kb(h.dma_free)})\n`+
 `PSRAM ${kb(h.psram_free)} (min ${kb(h.psram_min)})\n`+
 `net ${s.net.ip||'-'} ${s.net.ssid} ${s.net.rssi} dBm${s.net.hotspot?' hotspot on':''}\n`+
 `log dropped U${s.log.dropped.U} L${s.log.dropped.L} M${s.log.dropped.M}\n`+
 `now: loop ${s.crumbs.loop} | usb ${s.crumbs.usb} | loader ${s.crumbs.loader}\ntasks (stack never used):\n`;
 for(const k of s.tasks)t+=`  ${k.name.padEnd(16)} ${String(k.stack_free).padStart(6)} B  prio ${k.prio}\n`;
 $('board').textContent=t}
function usb(s){const u=s.usb;let t=`${u.running?'host on':'host OFF'}  mode ${u.mode}  ${u.channels}/8 channels  `+
 `sent ${u.sent} blocked ${u.blocked} failed ${u.failed_sends}\nhub ${u.hub}\n`;
 if(u.problem)t+=`problem: ${u.problem.join(' / ')}\n`;
 for(const d of s.devices)t+=`  #${d.addr} ${d.vid}:${d.pid} ${d.name}${d.hub?' (hub)':''} `+
  `${d.channels} ch, ${d.claimed}/${d.interfaces} itf claimed${d.refusal[0]?'  REFUSED: '+d.refusal.join(' / '):''}\n`;
 for(const c of u.controllers)t+=`  ${c.tag}: ${c.detail}\n`;
 t+=`FM-1 ${u.fm1_in}  ${u.fm1_own}`;$('usb').textContent=t}
function fw(s){const f=s.firmware;if(!f){$('fw').textContent='(no Loader in this build)';return}
 let t=`${f.title}${f.busy?'  [busy '+f.percent+'%]':''}\n`+f.lines.filter(x=>x).join('\n')+'\n';
 if(f.danger)t+='DANGER: don\'t unplug or switch off the FM-1\n';
 f.items.forEach((it,i)=>t+=`${i==f.plan.item?'>':' '} ${it.label}  ${it.note}\n`);
 if(f.plan.item>=0)t+=`plan: ${f.plan.lines.join(' / ')}\nhold: ${f.plan.hold}${f.plan.can_act?'':' (off)'}\n`;
 for(const [k,r] of [['blocked',f.blocked],['result',f.result]])if(r.headline)
  t+=`${k}: ${r.headline}. ${r.safety} ${r.why} ${r.todo}${r.detail?' ['+r.detail+']':''}${r.fix?' hold: '+r.fix:''}\n`;
 $('fw').textContent=t}
function store(s){if(!s.packages){$('store').textContent=s.error||'-';return}
 let t=`${kb(s.fs_used)} of ${kb(s.fs_total)} used, built ${(s.age_ms/1000).toFixed(0)} s ago\n`;
 for(const u of s.units){t+=`unit ${u.id}${u.id==s.last_unit?' (last)':''} installed ${u.installed||'?'}`+
  `${u.pending?' PENDING '+u.pending:''}\n  baseline ${u.baseline}\n  latest   ${u.latest}\n`;
  for(const v of u.saves)t+=`  save ${v.id} ${v.identity} ${v.label} ${kb(v.bytes)}\n`}
 t+=`backups: ${s.backups.join(', ')}\n`;
 for(const p of s.packages)t+=`${p.pinned?'*':' '} ${p.id}  ${p.name} ${p.version}  ${p.identity}  ${kb(p.bytes)}\n`;
 $('store').textContent=t}
async function tick(){try{const s=await get('/status');board(s);usb(s);fw(s);
 const l=await get('/log?since='+since);const el=$('log');const atEnd=el.scrollTop+el.clientHeight>=el.scrollHeight-4;
 if(l.lost)el.textContent+=`... ${l.lost} lines lost\n`;
 for(const [seq,ms,src,text] of l.lines)el.textContent+=`${(ms/1000).toFixed(3).padStart(9)} ${src} ${text}\n`;
 since=l.next-1;if(atEnd)el.scrollTop=el.scrollHeight;
 if(Date.now()-last>5000){last=Date.now();const c=await get('/crash');
  $('crash').textContent=`reset: ${c.reset_reason}\n${c.lines.filter(x=>x).join('\n')||'(none)'}\n`+
  `core dump: ${c.coredump.present?c.coredump.size+' bytes':'none'}`;store(await get('/store'))}
 $('age').textContent='updated '+new Date().toLocaleTimeString()}catch(e){$('age').textContent='no answer: '+e}}
tick();setInterval(tick,1000);
</script></body></html>
)HTML";

static const char kSetupHtml[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>@HOST@ setup</title>
<style>body{font:16px system-ui;margin:16px;max-width:28em}input,select,button{font:inherit;width:100%;margin:6px 0;padding:6px}</style>
</head><body><h3>Put @HOST@ on your Wi-Fi</h3>
<select id="ssid"></select><input id="pass" type="password" placeholder="Password">
<button id="go">Join</button><p id="msg">The hub saves this network and joins it. Your phone may drop off this
hotspot; then open <b>http://@HOST@.local</b> on the same Wi-Fi as the hub.</p>
<script>
const $=id=>document.getElementById(id);
fetch('/setup/scan').then(r=>r.json()).then(j=>{for(const n of j.nets){const o=document.createElement('option');
 o.value=n.ssid;o.textContent=`${n.ssid} (${n.rssi} dBm)`;$('ssid').appendChild(o)}});
$('go').onclick=async()=>{$('msg').textContent='Joining...';try{const r=await fetch('/setup/join',{method:'POST',
 body:JSON.stringify({ssid:$('ssid').value,pass:$('pass').value})});$('msg').textContent=(await r.json()).msg}
 catch(e){$('msg').textContent='The hotspot dropped, as expected. Open http://@HOST@.local on your Wi-Fi.'}};
</script></body></html>
)HTML";

// ---------- Handlers (server task) ----------
static esp_err_t on_index(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  if (via_hotspot(req)) {
    // Chunks: the page between marks, then the board's name in place of each mark.
    for (const char *p = kSetupHtml;;) {
      const char *m = strstr(p, kHostMark);
      httpd_resp_send_chunk(req, p, m ? m - p : strlen(p));
      if (!m) break;
      httpd_resp_send_chunk(req, host_name, strlen(host_name));
      p = m + sizeof kHostMark - 1;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
  }
  return httpd_resp_send(req, kIndexHtml, sizeof kIndexHtml - 1);
}

static esp_err_t on_status(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  char *buf = json_buf();
  DiagState &s = *status_copy;
  static TaskLine t[kMaxTasks];
  uint8_t tn;
  uint32_t at;
  char ip[16], ssid[33];
  int8_t r;
  bool ap;
  {
    Lock l;
    if (published) s = *published;
    at = published_at;
    tn = task_count;
    memcpy(t, tasks, sizeof t);
    memcpy(ip, own_ip, sizeof ip);
    memcpy(ssid, ssid_now, sizeof ssid);
    r = rssi;
    ap = ap_on;
  }
  if (!buf) return httpd_resp_send_500(req);
  Out o{buf, kJsonBuf};
  char commit[12];
  snprintf(commit, sizeof commit, "%07lx", (unsigned long)FW_VERSION);
  o.raw("{\"build\":{\"commit\":\"%s\",\"dirty\":%s,\"apps\":\"%s\",\"writes\":%s,\"safe_mode\":%s}", commit,
        FW_DIRTY ? "true" : "false", platform::kAppSetLabel,
#ifdef FM1_LOADER_WRITES
        FM1_LOADER_WRITES ? "true" : "false",
#else
        "false",
#endif
        safe ? "true" : "false");
  o.raw(",\"uptime_ms\":%lu,\"published_ms_ago\":%lu", (unsigned long)millis(), (unsigned long)(millis() - at));
  o.raw(",\"heap\":{\"internal_free\":%u,\"internal_min\":%u,\"internal_largest\":%u,\"dma_free\":%u,"
        "\"dma_largest\":%u,\"dma_min\":%u,\"psram_free\":%u,\"psram_min\":%u}",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_DMA),
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA), heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
  o.raw(",\"tasks\":[");
  for (int i = 0; i < tn; ++i) {
    o.raw("%s{\"name\":", i ? "," : "");
    o.str(t[i].name);
    o.raw(",\"stack_free\":%lu,\"prio\":%u}", (unsigned long)t[i].stack_free, t[i].priority);
  }
  o.raw("],\"crumbs\":{");
  o.key_str("loop", crumbs.loop, false);  // what each task is doing now (crumbs.h)
  o.key_str("usb", crumbs.usb);
  o.key_str("loader", crumbs.loader);
  o.raw("},\"net\":{");
  o.key_str("ip", ip, false);
  o.key_str("ssid", ssid);
  o.raw(",\"rssi\":%d,\"hotspot\":%s}", r, ap ? "true" : "false");
  o.raw(",\"log\":{\"dropped\":{\"U\":%lu,\"L\":%lu,\"M\":%lu}}", (unsigned long)(usb_ring ? usb_ring->dropped() : 0),
        (unsigned long)(loader_ring ? loader_ring->dropped() : 0), (unsigned long)(loop_ring ? loop_ring->dropped() : 0));

  const UiFrame &f = s.frame;
  o.raw(",\"usb\":{\"running\":%s,\"mode\":\"%s\",\"channels\":%u,\"sent\":%lu,\"blocked\":%lu,\"failed_sends\":%lu",
        s.host_running ? "true" : "false", mode_name(s.mode), f.snapshot.channels_used, (unsigned long)f.sent,
        (unsigned long)f.blocked, (unsigned long)s.failed_sends);
  age_field(o, "probe_ms_ago", s.probe_age_ms);
  o.key_str("hub", f.hub_detail);
  o.key_str("fm1_in", f.fm1_in);
  o.key_str("fm1_own", f.fm1_own);
  if (f.view.has_problem) {
    o.raw(",\"problem\":[");
    o.str(f.view.problem.line1);
    o.raw(",");
    o.str(f.view.problem.line2);
    o.raw("]");
  } else {
    o.raw(",\"problem\":null");
  }
  o.raw(",\"controllers\":[");
  for (int i = 0; i < f.controller_count; ++i) {
    o.raw("%s{", i ? "," : "");
    o.key_str("tag", f.controllers[i].tag, false);
    o.key_str("detail", f.controllers[i].detail);
    o.raw(",\"open\":%s}", f.controllers[i].open ? "true" : "false");
  }
  o.raw("]},\"devices\":[");
  for (int i = 0; i < s.device_count; ++i) {
    const DiagDevice &d = s.devices[i];
    o.raw("%s{\"addr\":%u,\"vid\":\"%04x\",\"pid\":\"%04x\",\"hub\":%s,\"channels\":%u,\"interfaces\":%u,\"claimed\":%u",
          i ? "," : "", d.address, d.vid, d.pid, d.hub ? "true" : "false", d.channels, d.interfaces, d.claimed);
    o.key_str("name", d.name);
    o.raw(",\"refusal\":[");
    o.str(d.refusal.line1);
    o.raw(",");
    o.str(d.refusal.line2);
    o.raw("],\"router\":\"%s\"", d.router == 1 ? "controller" : d.router == 2 ? "fm1" : "unknown");
    age_field(o, "rx_ms_ago", d.rx_age_ms);
    age_field(o, "sysex_ms_ago", d.sysex_age_ms);
    o.raw("}");
  }
  o.raw("]");
  if (s.has_loader) {
    const UiLoader &L = s.loader;
    o.raw(",\"firmware\":{");
    o.key_str("title", L.title, false);
    o.raw(",\"lines\":[");
    for (int i = 0; i < kLoaderLines; ++i) {
      if (i) o.raw(",");
      o.str(L.lines[i]);
    }
    o.raw("],\"items\":[");
    static const char *const kNotes[] = {"", "running", "your songs", "not yet"};
    for (int i = 0; i < L.item_count; ++i) {
      o.raw("%s{", i ? "," : "");
      o.key_str("label", L.items[i].label, false);
      o.key_str("note", kNotes[uint8_t(L.items[i].note) & 3]);
      o.raw("}");
    }
    o.raw("],\"plan\":{\"item\":%d,\"lines\":[", L.plan.item);
    for (int i = 0; i < L.plan.line_count; ++i) {
      if (i) o.raw(",");
      o.str(L.plan.lines[i]);
    }
    o.raw("]");
    o.key_str("hold", L.plan.hold);
    o.raw(",\"can_act\":%s}", L.plan.can_act ? "true" : "false");
    // The result page and what stands in the list's way, as the screen says them.
    const UiResult *const pages[] = {&L.result, &L.blocked};
    static const char *const kPageKeys[] = {"result", "blocked"};
    for (int i = 0; i < 2; ++i) {
      const UiResult &r = *pages[i];
      o.raw(",\"%s\":{", kPageKeys[i]);
      o.key_str("headline", r.headline, false);
      o.key_str("safety", r.safety_text);
      o.key_str("why", r.why);
      o.key_str("todo", r.todo);
      o.key_str("detail", r.detail);
      o.key_str("fix", r.fix_label);
      o.raw(",\"ok\":%s,\"seq\":%lu}", r.ok ? "true" : "false", (unsigned long)r.seq);
    }
    o.raw(",\"unfinished\":%s,\"can_back_up\":%s,\"can_retry\":%s", L.unfinished ? "true" : "false",
          L.can_back_up ? "true" : "false", L.can_retry ? "true" : "false");
    o.raw(",\"percent\":%u,\"busy\":%s,\"danger\":%s}", L.percent, L.job.active ? "true" : "false",
          L.job.danger ? "true" : "false");
  }
  o.raw("}");
  return send_json(req, buf, o.k);
}

static esp_err_t on_log(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  drain_log();
  char *buf = json_buf();
  if (!buf || !history) return httpd_resp_send_500(req);
  char query[32] = "", value[12] = "0";
  httpd_req_get_url_query_str(req, query, sizeof query);
  httpd_query_key_value(query, "since", value, sizeof value);
  const uint32_t since = strtoul(value, nullptr, 10);
  // Sends at most what fits; the client asks again from "next".
  Out o{buf, kJsonBuf - 256};
  uint32_t lost = 0, last = since;
  o.raw("{\"lines\":[");
  bool first = true;
  history->read(since, &lost, [&](uint32_t seq, const web_core::LogLine &l) {
    if (o.k + 4 * web_core::kLogText + 64 >= o.len) return;
    o.raw("%s[%lu,%lu,\"%c\",", first ? "" : ",", (unsigned long)seq, (unsigned long)l.ms, l.source);
    o.str(l.text);
    o.raw("]");
    first = false;
    last = seq;
  });
  o.len = kJsonBuf;
  if (first && since >= history->next()) last = history->next() - 1;  // a cursor from before a reboot
  o.raw("],\"next\":%lu,\"lost\":%lu}", (unsigned long)(last + 1), (unsigned long)lost);
  return send_json(req, buf, o.k);
}

static const esp_partition_t *coredump_partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
}

static esp_err_t on_crash(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  // The tail is up to 40 lines of 96 bytes: a PSRAM buffer, not this task's stack.
  constexpr size_t kCrashJson = 8192;
  static char *buf = static_cast<char *>(heap_caps_malloc(kCrashJson, MALLOC_CAP_SPIRAM));
  if (!buf) return httpd_resp_send_500(req);
  Out o{buf, kCrashJson};
  o.raw("{\"reset_reason\":");
  o.str(reset_name(esp_reset_reason()));
  o.raw(",\"safe_mode\":%s,\"lines\":[", safe ? "true" : "false");
  for (int i = 0; i < 3; ++i) {
    if (i) o.raw(",");
    o.str(last_crash[i]);
  }
  o.raw("],\"tail\":[");
  // Oldest first, read from RTC memory as it is now (this boot's lines too).
  bool first = true;
  const uint32_t next = __atomic_load_n(&crumbs.tail_next, __ATOMIC_RELAXED);
  for (int i = 0; i < kCrumbTail; ++i) {
    char line[kCrumbLine];
    memcpy(line, crumbs.tail[(next + i) % kCrumbTail], sizeof line);
    line[sizeof line - 1] = 0;  // a line being written may be torn, never unterminated
    if (!line[0]) continue;
    if (!first) o.raw(",");
    first = false;
    o.str(line);
  }
  size_t addr = 0, size = 0;
  const bool present = esp_core_dump_image_get(&addr, &size) == ESP_OK;
  o.raw("],\"coredump\":{\"present\":%s,\"size\":%u}}", present ? "true" : "false", unsigned(present ? size : 0));
  return send_json(req, buf, o.k);
}

// The raw partition, for `esp-coredump info_corefile --core-format raw` on the Mac.
// Not on the setup hotspot: task stacks are no business of whoever joined it.
static esp_err_t on_coredump(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  if (via_hotspot(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not on the setup hotspot");
  const esp_partition_t *p = coredump_partition();
  if (!p) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no coredump partition");
  static uint8_t *chunk = static_cast<uint8_t *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
  if (!chunk) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
  for (uint32_t off = 0; off < p->size; off += 4096) {
    const size_t n = p->size - off < 4096 ? p->size - off : 4096;
    if (esp_partition_read(p, off, chunk, n) != ESP_OK) break;
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(chunk), n) != ESP_OK) return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t on_store(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  std::string body;
  uint32_t at;
  {
    Lock l;
    if (store_body) body = *store_body;
    at = store_at;
  }
  if (body.empty()) {
    static const char kNone[] = "{\"error\":\"no store summary yet (no Loader, or it hasn't started)\"}";
    return send_json(req, kNone, sizeof kNone - 1);
  }
  char age[40];
  snprintf(age, sizeof age, "{\"age_ms\":%lu,", (unsigned long)(millis() - at));
  body.replace(0, 1, age);
  return send_json(req, body.data(), body.size());
}

static esp_err_t on_setup_scan(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  if (!via_hotspot(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "setup is on the hotspot only");
  char buf[sizeof scan_json];
  {
    Lock l;
    memcpy(buf, scan_json, sizeof buf);
  }
  return send_json(req, buf, strlen(buf));
}

// The one write: a Wi-Fi network from the setup page, on the hotspot only.
static esp_err_t on_setup_join(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  if (!via_hotspot(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "setup is on the hotspot only");
  char body[192], ssid[33] = "", pass[65] = "";
  if (req->content_len >= sizeof body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
  size_t got = 0;
  while (got < req->content_len) {
    const int r = httpd_req_recv(req, body + got, req->content_len - got);
    if (r <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    got += r;
  }
  body[got] = 0;
  if (!web_core::json_string(body, "ssid", ssid, sizeof ssid) || !ssid[0])
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid");
  web_core::json_string(body, "pass", pass, sizeof pass);
  {
    Lock l;
    memcpy(creds_ssid, ssid, sizeof creds_ssid);
    memcpy(creds_pass, pass, sizeof creds_pass);
    creds_pending = true;
  }
  char ok[160];
  snprintf(ok, sizeof ok, "{\"msg\":\"Joining. If it works, the hub's settings page shows its address; open http://%s.\"}",
           mdns_name);
  return send_json(req, ok, strlen(ok));
}

#if BENCH
// ---------- Bench API (BENCH=1 builds on a listed spare only) ----------
// One command waits at a time; the loop takes it (web_take_bench_command) and
// presses what the screen would. The Mac reads the outcome from /status.
static const char *bench_board = nullptr;  // "waveshare-1", or null: no bench API
static char bench_mac[18] = "";
static web_core::BenchCommand bench_pending;
static bool bench_has_pending = false;
static uint32_t bench_seq = 0, bench_taken_seq = 0;
static char bench_last[64] = "";

static esp_err_t on_bench_state(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  char buf[256];
  {
    Lock l;
    snprintf(buf, sizeof buf,
             "{\"board\":\"%s\",\"mac\":\"%s\",\"seq\":%lu,\"taken\":%lu,\"pending\":%s,\"last\":\"%s\"}",
             bench_board ? bench_board : "unknown", bench_mac, (unsigned long)bench_seq,
             (unsigned long)bench_taken_seq, bench_has_pending ? "true" : "false", bench_last);
  }
  return send_json(req, buf, strlen(buf));
}

static esp_err_t on_bench_command(httpd_req_t *req) {
  if (!guard(req)) return ESP_OK;
  char path[32] = "", query[48] = "";
  const char *q = strchr(req->uri, '?');
  const size_t plen = q ? size_t(q - req->uri) : strlen(req->uri);
  if (plen >= sizeof path) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
  memcpy(path, req->uri, plen);
  httpd_req_get_url_query_str(req, query, sizeof query);
  web_core::BenchCommand c;
  const char *why = "";
  if (!web_core::parse_bench_command(path, query, c, &why)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
  uint32_t seq;
  {
    Lock l;
    if (bench_has_pending) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a command is still waiting");
    bench_pending = c;
    bench_has_pending = true;
    seq = ++bench_seq;
    snprintf(bench_last, sizeof bench_last, "%s %d queued", web_core::bench_kind_name(c.kind), c.index);
  }
  DLOG_LOOP("bench: %s %d queued as %lu", web_core::bench_kind_name(c.kind), c.index, (unsigned long)seq);
  char buf[48];
  snprintf(buf, sizeof buf, "{\"seq\":%lu}", (unsigned long)seq);
  return send_json(req, buf, strlen(buf));
}

bool web_take_bench_command(web_core::BenchCommand &out) {
  if (!started) return false;  // Computer mode never runs web_begin: no state_mutex to take
  Lock l;
  if (!bench_has_pending) return false;
  out = bench_pending;
  bench_has_pending = false;
  bench_taken_seq = bench_seq;
  snprintf(bench_last, sizeof bench_last, "%s %d taken", web_core::bench_kind_name(out.kind), out.index);
  return true;
}

static void bench_check_board() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  bench_board = web_core::waveshare_name(mac);
  snprintf(bench_mac, sizeof bench_mac, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5]);
  DLOG_LOOP("bench: board %s is %s", bench_mac, bench_board ? bench_board : "not a listed Waveshare: no bench API");
}
#endif  // BENCH

static esp_err_t on_not_found(httpd_req_t *req, httpd_err_code_t) {
  if (via_hotspot(req)) return redirect_to_setup(req);  // captive-portal probes
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
  return ESP_OK;
}

static void drain_work(void *) {
  drain_queued = false;
  drain_log();
}

// ---------- Server and Wi-Fi (loop) ----------
static void start_server() {
  if (server) return;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.core_id = 0;
  cfg.task_priority = 3;  // below the USB host tasks (5)
  cfg.stack_size = 8192;
  cfg.max_open_sockets = 7;
  cfg.max_uri_handlers = web_core::kRouteCount;
#if BENCH
  cfg.max_uri_handlers += web_core::kBenchRouteCount;
#endif
  cfg.lru_purge_enable = true;
  cfg.send_wait_timeout = 2;
  cfg.recv_wait_timeout = 2;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    server = nullptr;
    DLOG_LOOP("web: server didn't start");
    return;
  }
  // Same order as web_core::kRoutes, whose methods the tests check.
  esp_err_t (*const handlers[])(httpd_req_t *) = {on_index, on_status, on_log, on_crash,
                                                  on_coredump, on_store, on_setup_scan, on_setup_join};
  static_assert(sizeof handlers / sizeof handlers[0] == web_core::kRouteCount, "a handler per route");
  for (int i = 0; i < web_core::kRouteCount; ++i) {
    httpd_uri_t u{};
    u.uri = web_core::kRoutes[i].uri;
    u.method = web_core::kRoutes[i].method == web_core::Method::Get ? HTTP_GET : HTTP_POST;
    u.handler = handlers[i];
    httpd_register_uri_handler(server, &u);
  }
#if BENCH
  // Only on one of Chad's Waveshares; tools/bench.py checks it's the one it asked for.
  if (bench_board) {
    for (const web_core::Route &r : web_core::kBenchRoutes) {
      httpd_uri_t u{};
      u.uri = r.uri;
      u.method = r.method == web_core::Method::Get ? HTTP_GET : HTTP_POST;
      u.handler = r.method == web_core::Method::Get ? on_bench_state : on_bench_command;
      httpd_register_uri_handler(server, &u);
    }
  }
#endif
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, on_not_found);
}

static void set_ips() {
  Lock l;
  if (WiFi.status() == WL_CONNECTED) {
    strlcpy(own_ip, WiFi.localIP().toString().c_str(), sizeof own_ip);
    strlcpy(ssid_now, WiFi.SSID().c_str(), sizeof ssid_now);
    rssi = WiFi.RSSI();
  } else {
    own_ip[0] = ssid_now[0] = 0;
  }
  if (ap_on) {
    const IPAddress ip = WiFi.softAPIP();
    strlcpy(ap_ip, ip.toString().c_str(), sizeof ap_ip);
    ap_ip_u32 = static_cast<uint32_t>(ip);
  } else {
    ap_ip[0] = 0;
    ap_ip_u32 = 0;
  }
  if (own_ip[0]) snprintf(network_line, sizeof network_line, "%s  %s", mdns_name, own_ip);
  else if (ap_on) snprintf(network_line, sizeof network_line, "Wi-Fi: join %s pw %s", setup_ssid, ap_pass);
  else snprintf(network_line, sizeof network_line, "Wi-Fi: joining %s", saved_ssid);
}

static void remember_scan(int found) {
  seen_count = 0;
  for (int i = 0; i < found && seen_count < int(sizeof seen / sizeof seen[0]); ++i) {
    if (WiFi.SSID(i).length() == 0) continue;
    Seen &n = seen[seen_count++];
    strlcpy(n.ssid, WiFi.SSID(i).c_str(), sizeof n.ssid);
    n.rssi = WiFi.RSSI(i);
    n.channel = WiFi.channel(i);
    memcpy(n.bssid, WiFi.BSSID(i), sizeof n.bssid);
  }
  WiFi.scanDelete();
  char buf[sizeof scan_json], name[70];
  size_t k = snprintf(buf, sizeof buf, "{\"nets\":[");
  for (int i = 0; i < seen_count && k + 100 < sizeof buf; ++i) {
    json_escape(seen[i].ssid, name, sizeof name);
    k += snprintf(buf + k, sizeof buf - k, "%s{\"ssid\":\"%s\",\"rssi\":%d}", i ? "," : "", name, seen[i].rssi);
  }
  snprintf(buf + k, sizeof buf - k, "]}");
  Lock l;
  memcpy(scan_json, buf, sizeof scan_json);
}

static const Seen *find_seen(const char *ssid) {
  const Seen *best = nullptr;
  for (int i = 0; i < seen_count; ++i)
    if (strcmp(seen[i].ssid, ssid) == 0 && (!best || seen[i].rssi > best->rssi)) best = &seen[i];
  return best;
}

// The hotspot starts on the channel of the network Chad will most likely pick,
// so joining it doesn't move the radio and drop the phone (web-UI spike).
// The scan blocks the loop ~2 s; USB routing runs on its own task.
static void start_hotspot(uint32_t now) {
  if (ap_on) return;
  WiFi.mode(WIFI_AP_STA);  // the radio is on now, so esp_random() is truly random
  // 12 characters from 32 easy-to-read ones: 60 bits, so a captured handshake
  // can't be cracked offline the way 8 digits can.
  static const char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (size_t i = 0; i + 1 < sizeof ap_pass; ++i) ap_pass[i] = kAlphabet[esp_random() % 31];
  ap_pass[sizeof ap_pass - 1] = 0;
  WiFi.disconnect();  // a station still retrying hops channels and drags the hotspot along
  remember_scan(WiFi.scanNetworks(false));
  web_core::ScanEntry entries[sizeof seen / sizeof seen[0]];
  for (int i = 0; i < seen_count; ++i) entries[i] = {seen[i].ssid, seen[i].rssi, seen[i].channel};
  hotspot_channel = web_core::pick_hotspot_channel(entries, seen_count, saved_ssid);
  WiFi.softAP(setup_ssid, ap_pass, hotspot_channel);
  dns.start(53, "*", WiFi.softAPIP());
  {
    Lock l;
    ap_on = true;
  }
  set_ips();
  DLOG_LOOP("wifi: hotspot %s on channel %u", setup_ssid, hotspot_channel);
}

static void stop_hotspot() {
  if (!ap_on) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  {
    Lock l;
    ap_on = false;
  }
  set_ips();
  DLOG_LOOP("wifi: hotspot off");
}

// Joins without scanning when the network was seen: a scan hops every channel.
static void join(const char *ssid, const char *pass) {
  WiFi.enableIPv6(true);  // link-local only: lets mDNS answer the AAAA query macOS sends first
  const Seen *n = find_seen(ssid);
  if (n) WiFi.begin(ssid, pass, n->channel, n->bssid);
  else WiFi.begin(ssid, pass);
  connect_started = millis();
  net = Net::Connecting;
  DLOG_LOOP("wifi: joining %s", ssid);
}

void web_begin(bool safe_mode) {
  if (started) return;
  safe = safe_mode;
  {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    web_core::web_host_name(mac, BENCH, host_name, sizeof host_name);
    snprintf(mdns_name, sizeof mdns_name, "%s.local", host_name);
    snprintf(setup_ssid, sizeof setup_ssid, "%s-setup", host_name);
  }
#if BENCH
  bench_check_board();
#endif
  const esp_reset_reason_t r = esp_reset_reason();
  const bool abnormal = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                        r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
  if (safe_mode && abnormal && safe_wifi_marker == kSafeWifiMagic) {
    safe_wifi_marker = 0;  // safe mode with Wi-Fi crashed last time: stay off the radio
    strlcpy(network_line, "Wi-Fi off: it crashed in safe mode", sizeof network_line);
    return;
  }
  safe_wifi_marker = safe_mode ? kSafeWifiMagic : 0;

  state_mutex = xSemaphoreCreateMutex();
  auto psram = [](size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM); };
  void *a = psram(sizeof *usb_ring), *b = psram(sizeof *loader_ring), *c = psram(sizeof *loop_ring),
       *d = psram(sizeof *history), *e = psram(sizeof *published), *f = psram(sizeof *status_copy);
  if (!state_mutex || !a || !b || !c || !d || !e || !f) return;
  usb_ring = new (a) web_core::LineRing<32>();
  loader_ring = new (b) web_core::LineRing<64>();
  loop_ring = new (c) web_core::LineRing<32>();
  history = new (d) web_core::LogHistory<512>();
  published = new (e) DiagState();
  status_copy = new (f) DiagState();
  store_body = new std::string();
  started = true;
  DLOG_LOOP("boot: %07lx%s %s, reset %s%s", (unsigned long)FW_VERSION, FW_DIRTY ? "+" : "", platform::kAppSetLabel,
            reset_name(r), safe_mode ? ", safe mode" : "");

  wifi_prefs.begin("wifi", false);  // the web-UI spike's namespace: a network it saved still works
  strlcpy(saved_ssid, wifi_prefs.getString("ssid", "").c_str(), sizeof saved_ssid);
  strlcpy(saved_pass, wifi_prefs.getString("pass", "").c_str(), sizeof saved_pass);
  WiFi.persistent(false);  // credentials live in our own NVS namespace
  WiFi.setHostname(host_name);
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t info) {
        disconnect_reason = info.wifi_sta_disconnected.reason;
        ++disconnects;
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  if (saved_ssid[0]) {
    WiFi.mode(WIFI_STA);
    join(saved_ssid, saved_pass);
    fallback.boot_join(millis());
  } else {
    start_hotspot(millis());
  }
  set_ips();
  start_server();
}

// What the loop samples once a second: each task's stack high-water mark. On
// the loop, because restart_host() deletes the USB tasks from this same task,
// so none can disappear while their names are copied.
static void sample_tasks() {
  // FreeRTOS fills nothing (returns 0) if more tasks exist than fit, so the
  // array has room to spare; the log says so if it ever runs out.
  static TaskStatus_t status[kMaxTasks];
  static TaskLine lines[kMaxTasks];
  const UBaseType_t n = uxTaskGetSystemState(status, kMaxTasks, nullptr);
  if (n == 0) DLOG_LOOP("tasks: more than %d, stack marks not shown", kMaxTasks);
  for (UBaseType_t i = 0; i < n; ++i) {
    strlcpy(lines[i].name, status[i].pcTaskName, sizeof lines[i].name);
    lines[i].stack_free = status[i].usStackHighWaterMark;  // bytes on ESP-IDF
    lines[i].priority = uint8_t(status[i].uxCurrentPriority);
  }
  Lock l;
  memcpy(tasks, lines, sizeof lines);
  task_count = uint8_t(n);
}

void web_loop(uint32_t now) {
  if (!started) return;
  static uint32_t last_tasks = 0;
  if (now - last_tasks >= 1000) {
    last_tasks = now;
    sample_tasks();
  }

  bool new_creds = false;
  {
    Lock l;
    std::swap(new_creds, creds_pending);
  }
  if (new_creds) {
    {
      Lock l;
      memcpy(trial_ssid, creds_ssid, sizeof trial_ssid);
      memcpy(trial_pass, creds_pass, sizeof trial_pass);
    }
    // Joining may move the radio off the hotspot's channel and drop the phone:
    // wait so the page's reply gets out first.
    join_at = now + 1500;
  }
  if (join_at && int32_t(now - join_at) >= 0) {
    join_at = 0;
    if (ap_on) WiFi.mode(WIFI_AP_STA);
    joining_from_setup = ap_on;
    WiFi.disconnect();
    join(trial_ssid, trial_pass);
  }
  if (ap_on) dns.processNextRequest();

  const wl_status_t status = WiFi.status();
  static wl_status_t logged_status = WL_NO_SHIELD;
  if (status != logged_status) {
    DLOG_LOOP("wifi: status %d -> %d", int(logged_status), int(status));
    logged_status = status;
  }
  static uint32_t logged_disconnects = 0;
  if (disconnects.load() != logged_disconnects) {
    logged_disconnects = disconnects.load();
    const uint8_t r = disconnect_reason.load();
    DLOG_LOOP("wifi: station disconnected, reason %u %s (%lu so far)", r,
              WiFi.disconnectReasonName(wifi_err_reason_t(r)), (unsigned long)logged_disconnects);
  }
  const bool connected = status == WL_CONNECTED;
  if (connected && net != Net::Online) {
    net = Net::Online;
    online_since = now;
    if (trial_ssid[0] && WiFi.SSID() == trial_ssid) {  // they work: keep them
      memcpy(saved_ssid, trial_ssid, sizeof saved_ssid);
      memcpy(saved_pass, trial_pass, sizeof saved_pass);
      wifi_prefs.putString("ssid", saved_ssid);
      wifi_prefs.putString("pass", saved_pass);
      trial_ssid[0] = trial_pass[0] = 0;
    }
    if (!mdns_on && MDNS.begin(host_name)) {
      MDNS.addService("http", "tcp", 80);
      mdns_on = true;
    }
    set_ips();
    DLOG_LOOP("wifi: online %s %s %d dBm", ssid_now, own_ip, rssi);
  } else if (!connected && net == Net::Online) {
    net = Net::Connecting;
    connect_started = now;
    if (mdns_on) MDNS.end();
    mdns_on = false;
    set_ips();
    DLOG_LOOP("wifi: lost the network");
  }

  if (ap_on) {
    if (connected) {
      // Joined from the setup page: keep the hotspot a minute so the phone can
      // show the result; a background retry closes it at once.
      if (!joining_from_setup || now - online_since > 60000) {
        joining_from_setup = false;
        stop_hotspot();
      }
    } else if (joining_from_setup && connect_started && now - connect_started > 15000) {
      // A join from the setup page that didn't work. Stop the station retrying:
      // its channel hops would drag the hotspot (and the phone) around.
      WiFi.disconnect();
      connect_started = 0;
      joining_from_setup = false;
      DLOG_LOOP("wifi: join failed");
    }
  }
  // Not associated. Status 6 also covers a join still under way, but by the
  // first nudge (20 s) a working join has long finished (~1-5 s).
  const bool stalled = status == WL_DISCONNECTED || status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL;
  const int phones = ap_on ? WiFi.softAPgetStationNum() : 0;
  switch (fallback.tick(now, {saved_ssid[0] != 0, connected, stalled, ap_on, phones, joining_from_setup})) {
    case web_core::WifiFallback::Do::Nothing:
      break;
    case web_core::WifiFallback::Do::Nudge: {
      const bool ok = WiFi.reconnect();  // esp_wifi_connect() only: never a disconnect over a join
      fallback.nudge_result(now, ok);
      DLOG_LOOP("wifi: nudge at status %d, reconnect %s", int(status), ok ? "ok" : "failed");
      break;
    }
    case web_core::WifiFallback::Do::OpenHotspot:
      start_hotspot(now);
      break;
    case web_core::WifiFallback::Do::Join:
      join(saved_ssid, saved_pass);
      break;
    case web_core::WifiFallback::Do::StopJoin:
      WiFi.disconnect();  // as above: a retry still hopping would drag the hotspot along
      connect_started = 0;
      DLOG_LOOP("wifi: join failed");
      break;
  }
  static uint32_t last_ips = 0;
  if (connected && now - last_ips > 5000) {  // refresh RSSI
    last_ips = now;
    set_ips();
  }

  if (server && !drain_queued && now - last_drain >= 100) {
    last_drain = now;
    drain_queued = true;
    if (httpd_queue_work(server, drain_work, nullptr) != ESP_OK) drain_queued = false;
  }
}

void web_publish(const DiagState &s) {
  if (!started) return;
  Lock l;
  *published = s;
  published_at = millis();
}

void diag_publish_store(std::string &&json) {
  if (!started) return;
  Lock l;
  store_body->swap(json);
  store_at = millis();
}  // the old body, now in json, is freed by the caller, after the lock

const char *web_network_line() { return network_line; }

#endif  // APP_WEB_UI
