// The graph on Big-P4 (HW2): see hub_graph.h for who runs what where.
#include "hub_graph.h"

#include <Arduino.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gr_hub.h"
#include "hub_log.h"
#include "hub_usb.h"
#include "json_out.h"

namespace {

gr_hub hub;  // ~140 KB, static (never on a stack)

// The two Wi-Fi robots by their USB serial (their MAC): robot.py's names.
struct Robot {
  char name;
  const char *serial;
};
constexpr Robot kRobots[] = {{'a', "44BD8D612B28"}, {'b', "A085E3F0A23C"}};
constexpr int kFan = 2;  // destinations per out endpoint

struct Ep {
  char robot;     // 'a', 'b', or 0 = none
  uint8_t cable;  // USB-MIDI cable number (0-based, as robot.py's scenarios)
};
struct Route {
  gr_hub_config cfg;
  Ep in[GR_HUB_ENDPOINTS];
  Ep out[GR_HUB_ENDPOINTS][kFan];
  char app_name[8];
  uint32_t gen;
};
Route route;

// Device table: address -> robot name, kept by the client task's device hook.
portMUX_TYPE map_mux = portMUX_INITIALIZER_UNLOCKED;
char addr_robot[128];
// Resolved: the current USB address of each endpoint (0 = not connected).
volatile uint8_t in_addr[GR_HUB_ENDPOINTS];
volatile uint8_t out_addr[GR_HUB_ENDPOINTS][kFan];

// Park protocol: the web task stops both hub tasks and the client task's feed
// before it re-inits the hub.
volatile bool live = false;
volatile bool park_want = false;
volatile bool graph_parked = false, out_parked = false;
int in_busy = 0;  // client task inside a hub call (atomic)
volatile uint32_t reset_gen = 0;

TaskHandle_t out_task_h = nullptr;
esp_timer_handle_t out_timer = nullptr;

uint32_t now_us() { return (uint32_t)esp_timer_get_time(); }
void put(volatile uint32_t &v, uint32_t x) { __atomic_store_n(&v, x, __ATOMIC_RELAXED); }

// ---- counters, each written by one task only (32-bit, read anywhere) ----
// core 1, graph task
constexpr int kWakeBins = 8;
const uint32_t kWakeEdges[kWakeBins - 1] = {500, 900, 1100, 1500, 2000, 3000, 10000};  // us
struct GraphCounters {
  uint32_t runs, wakes, wake_max_us, wake_min_us, frames_max, frames_min;
  uint32_t wake_bin[kWakeBins];
  uint32_t frames48, frames_other;
} gc;
// core 0, client task
struct InCounters {
  uint32_t pushed[GR_HUB_ENDPOINTS], refused[GR_HUB_ENDPOINTS];
  uint32_t sysex, unmapped, releases, released_notes, connects, disconnects;
} ic;
// core 0, out task
constexpr int kLateBins = 7;
const uint32_t kLateEdges[kLateBins - 1] = {50, 100, 200, 500, 1000, 2000};  // us after due
struct OutCounters {
  uint32_t sent[GR_HUB_ENDPOINTS], nodest, send_fail, non_midi1;
  uint32_t late_max_us, late_sum_us, late_n;
  uint32_t late_bin[kLateBins];
  uint32_t timer_wakes, tick_wakes;
} oc;

int bin_of(uint32_t v, const uint32_t *edges, int n) {
  int b = 0;
  while (b < n - 1 && v >= edges[b]) ++b;
  return b;
}

const char *robot_serial(char r) {
  for (const Robot &x : kRobots)
    if (x.name == r) return x.serial;
  return nullptr;
}

char robot_of_serial(const char *s) {
  for (const Robot &x : kRobots)
    if (strcasecmp(x.serial, s) == 0) return x.name;
  return 0;
}

uint8_t addr_of(char robot) {
  if (!robot) return 0;
  for (int a = 1; a < 128; ++a)
    if (addr_robot[a] == robot) return (uint8_t)a;
  return 0;
}

// Under map_mux.
void resolve_locked() {
  for (int i = 0; i < GR_HUB_ENDPOINTS; ++i) {
    in_addr[i] = addr_of(route.in[i].robot);
    for (int k = 0; k < kFan; ++k) out_addr[i][k] = addr_of(route.out[i][k].robot);
  }
}

int midi1_len(uint8_t cin) {
  switch (cin) {
    case 0x2: case 0xC: case 0xD: return 2;
    case 0x3: case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: return 3;
    case 0x5: case 0xF: return 1;
    default: return 0;  // SysEx (0x4, 0x6, 0x7) and reserved
  }
}

uint8_t cin_of(const uint8_t *m, int len) {
  const uint8_t s = m[0];
  if (s < 0xF0) return s >> 4;
  if (s >= 0xF8) return 0xF;
  if (s == 0xF2) return 0x3;
  if (s == 0xF1 || s == 0xF3) return 0x2;
  return len == 1 ? 0x5 : 0x0;
}

// ---- client task (core 0): MIDI in and device changes ----
void enter() { __atomic_add_fetch(&in_busy, 1, __ATOMIC_SEQ_CST); }
void leave() { __atomic_sub_fetch(&in_busy, 1, __ATOMIC_SEQ_CST); }

void on_midi(uint8_t address, const uint8_t pkt[4], uint32_t) {
  const uint32_t t = now_us();
  enter();
  if (__atomic_load_n(&live, __ATOMIC_SEQ_CST)) {
    const uint8_t cable = pkt[0] >> 4;
    const int len = midi1_len(pkt[0] & 0x0F);
    bool mapped = false;
    for (int i = 0; i < hub.n_in; ++i) {
      if (in_addr[i] != address || route.in[i].cable != cable) continue;
      mapped = true;
      if (!len) { put(ic.sysex, ic.sysex + 1); break; }
      if (gr_hub_in_midi1(&hub, i, t, pkt + 1, len)) put(ic.pushed[i], ic.pushed[i] + 1);
      else put(ic.refused[i], ic.refused[i] + 1);
    }
    if (!mapped) put(ic.unmapped, ic.unmapped + 1);
  }
  leave();
}

void on_device(uint8_t address, const char *serial, bool connected) {
  if (address >= 128) return;
  const uint32_t t = now_us();
  enter();
  const char r = connected ? robot_of_serial(serial) : addr_robot[address];
  if (!connected && __atomic_load_n(&live, __ATOMIC_SEQ_CST)) {
    // Its notes end where they went (docs/graph-p4.md, "Unplug and replug").
    for (int i = 0; i < hub.n_in; ++i) {
      if (in_addr[i] != address) continue;
      const int n = gr_hub_in_release(&hub, i, t);
      put(ic.releases, ic.releases + 1);
      put(ic.released_notes, ic.released_notes + (uint32_t)n);
    }
  }
  portENTER_CRITICAL(&map_mux);
  addr_robot[address] = connected ? r : 0;
  resolve_locked();
  portEXIT_CRITICAL(&map_mux);
  if (connected) put(ic.connects, ic.connects + 1);
  else put(ic.disconnects, ic.disconnects + 1);
  leave();
}

// ---- core 1: the graph ----
void graph_task(void *) {
  uint32_t prev = 0, seen_reset = 0;
  for (;;) {
    if (park_want) {
      graph_parked = true;
      while (park_want) vTaskDelay(1);
      graph_parked = false;
      prev = 0;
      continue;
    }
    if (reset_gen != seen_reset) {
      seen_reset = reset_gen;
      memset((void *)&gc, 0, sizeof gc);
      prev = 0;
    }
    const uint32_t t = now_us();
    if (prev) {
      const uint32_t w = t - prev;
      put(gc.wakes, gc.wakes + 1);
      if (w > gc.wake_max_us) put(gc.wake_max_us, w);
      if (!gc.wake_min_us || w < gc.wake_min_us) put(gc.wake_min_us, w);
      const int b = bin_of(w, kWakeEdges, kWakeBins);
      put(gc.wake_bin[b], gc.wake_bin[b] + 1);
    }
    prev = t;
    uint32_t f;
    while ((f = gr_hub_frames_due(&hub, t)) != 0) {
      gr_hub_run(&hub, f, t);
      put(gc.runs, gc.runs + 1);
      if (f > gc.frames_max) put(gc.frames_max, f);
      if (!gc.frames_min || f < gc.frames_min) put(gc.frames_min, f);
      if (f == 48) put(gc.frames48, gc.frames48 + 1);
      else put(gc.frames_other, gc.frames_other + 1);
    }
    vTaskDelay(1);
  }
}

// ---- core 0: MIDI out, sent at each event's due time ----
void out_timer_cb(void *) {
  if (out_task_h) xTaskNotifyGive(out_task_h);
}

void out_task(void *) {
  uint32_t seen_reset = 0;
  for (;;) {
    if (park_want) {
      out_parked = true;
      while (park_want) vTaskDelay(1);
      out_parked = false;
      continue;
    }
    if (reset_gen != seen_reset) {
      seen_reset = reset_gen;
      memset((void *)&oc, 0, sizeof oc);
    }
    uint32_t now = now_us();
    gr_tevent e;
    for (int o = 0; o < hub.n_out; ++o) {
      while (gr_hub_out_ump(&hub, o, now, &e)) {
        const uint32_t late = now - e.t_us;
        if (late > oc.late_max_us) put(oc.late_max_us, late);
        put(oc.late_sum_us, oc.late_sum_us + late);
        put(oc.late_n, oc.late_n + 1);
        const int b = bin_of(late, kLateEdges, kLateBins);
        put(oc.late_bin[b], oc.late_bin[b] + 1);
        uint8_t m[3] = {0, 0, 0};
        const int n = gr_ump_to_midi1(e.ump, m);
        if (n <= 0) { put(oc.non_midi1, oc.non_midi1 + 1); continue; }
        bool any = false;
        for (int k = 0; k < kFan; ++k) {
          const uint8_t a = out_addr[o][k];
          if (!route.out[o][k].robot) continue;
          if (!a) continue;
          any = true;
          const uint8_t pkt[4] = {(uint8_t)(route.out[o][k].cable << 4 | cin_of(m, n)), m[0], m[1], m[2]};
          if (hub_usb_send_now(a, pkt)) put(oc.sent[o], oc.sent[o] + 1);
          else put(oc.send_fail, oc.send_fail + 1);
        }
        if (!any) put(oc.nodest, oc.nodest + 1);  // its device is gone: thrown away, never stale
      }
    }
    // Sleep until the earliest head event is due, or a tick if none is queued.
    bool have = false;
    uint32_t next = 0;
    for (int o = 0; o < hub.n_out; ++o) {
      const gr_tevent *h = (const gr_tevent *)gr_spsc_peek(&hub.out_q[o]);
      if (!h) continue;
      if (!have || (int32_t)(h->t_us - next) < 0) next = h->t_us;
      have = true;
    }
    now = now_us();
    if (have) {
      const int32_t d = (int32_t)(next - now);
      if (d <= 0) continue;
      if (d < 3000) {
        esp_timer_stop(out_timer);
        esp_timer_start_once(out_timer, (uint64_t)d);
        ulTaskNotifyTake(pdTRUE, 4);
        put(oc.timer_wakes, oc.timer_wakes + 1);
        continue;
      }
    }
    ulTaskNotifyTake(pdTRUE, 1);
    put(oc.tick_wakes, oc.tick_wakes + 1);
  }
}

// ---- configuration (web task) ----
bool parse_ep(const char *v, Ep &ep) {
  if (!v[0] || !robot_serial(v[0]) || v[1] < '0' || v[1] > '9') return false;
  ep.robot = v[0];
  ep.cable = (uint8_t)(v[1] - '0');
  return true;
}

bool qget(const char *q, const char *key, char *v, size_t cap) {
  return httpd_query_key_value(q, key, v, cap) == ESP_OK;
}

int apply(const Route &r) {
  __atomic_store_n(&live, false, __ATOMIC_SEQ_CST);
  park_want = true;
  const uint32_t t0 = millis();
  while ((!graph_parked || !out_parked || __atomic_load_n(&in_busy, __ATOMIC_SEQ_CST)) && millis() - t0 < 1000) delay(1);
  if (!graph_parked || !out_parked) {
    park_want = false;
    return -100;
  }
  route = r;
  route.cfg.cpu_mhz = getCpuFrequencyMhz();
  const int rc = gr_hub_init(&hub, &route.cfg);
  portENTER_CRITICAL(&map_mux);
  resolve_locked();
  portEXIT_CRITICAL(&map_mux);
  reset_gen = reset_gen + 1;
  memset((void *)&ic, 0, sizeof ic);  // the client task is held off (live false, in_busy 0)
  if (rc == GR_OK) __atomic_store_n(&live, true, __ATOMIC_SEQ_CST);
  park_want = false;
  HLOG_MAIN("GRAPH app=%s rc=%d in=%u out=%u", route.app_name, rc, hub.n_in, hub.n_out);
  return rc;
}

void route_default(Route &r) {
  memset(&r, 0, sizeof r);
  gr_hub_config_default(&r.cfg, GR_HUB_APP_THRU);
  strlcpy(r.app_name, "thru", sizeof r.app_name);
  r.in[0] = {'a', 0};
  r.out[0][0] = {'a', 1};  // the loopback: robot a cable 0 -> robot a cable 1
}

void ep_json(JsonOut &o, const Ep &e) {
  if (e.robot) o.raw("\"%c%u\"", e.robot, e.cable);
  else o.raw("null");
}

}  // namespace

void graph_begin() {
  Route r;
  route_default(r);
  route = r;
  route.cfg.cpu_mhz = getCpuFrequencyMhz();
  const int rc = gr_hub_init(&hub, &route.cfg);
  hub_usb_on_device(on_device);
  hub_usb_on_midi(on_midi);
  esp_timer_create_args_t ta{};
  ta.callback = out_timer_cb;
  ta.name = "graph_out";
  esp_timer_create(&ta, &out_timer);
  xTaskCreatePinnedToCore(out_task, "graph_out", 4096, nullptr, 6, &out_task_h, 0);
  xTaskCreatePinnedToCore(graph_task, "graph", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
  if (rc == GR_OK) __atomic_store_n(&live, true, __ATOMIC_SEQ_CST);
  HLOG_MAIN("GRAPH begin rc=%d sizeof(gr_hub)=%u cpu=%lu MHz tick=%u Hz", rc, (unsigned)sizeof(gr_hub),
            (unsigned long)route.cfg.cpu_mhz, (unsigned)configTICK_RATE_HZ);
}

size_t graph_configure(const char *q, char *out, size_t cap) {
  JsonOut o{out, cap};
  char v[24];
  if (qget(q, "reset", v, sizeof v) && atoi(v)) {
    reset_gen = reset_gen + 1;
    o.raw("{\"ok\":true,\"reset\":true}");
    return o.len;
  }
  Route r;
  memset(&r, 0, sizeof r);
  static const char *names[] = {"thru", "merge", "split", "layer", "clock"};
  int app = -1;
  if (qget(q, "app", v, sizeof v))
    for (int i = 0; i < 5; ++i)
      if (strcmp(v, names[i]) == 0) app = i;
  if (app < 0) {
    o.raw("{\"ok\":false,\"error\":\"app=thru|merge|split|layer|clock\"}");
    return o.len;
  }
  gr_hub_config_default(&r.cfg, (gr_hub_app)app);
  strlcpy(r.app_name, names[app], sizeof r.app_name);
  if (qget(q, "ch_lower", v, sizeof v)) r.cfg.ch_lower = (uint8_t)atoi(v);
  if (qget(q, "ch_upper", v, sizeof v)) r.cfg.ch_upper = (uint8_t)atoi(v);
  if (qget(q, "transpose", v, sizeof v)) r.cfg.transpose = (int8_t)atoi(v);
  if (qget(q, "split_t", v, sizeof v)) r.cfg.split_transpose = (int8_t)atoi(v);
  if (qget(q, "split_key", v, sizeof v)) r.cfg.split_key = (uint8_t)atoi(v);
  if (qget(q, "bpm", v, sizeof v)) r.cfg.bpm = (float)atof(v);
  if (qget(q, "plain", v, sizeof v)) r.cfg.clock_plain_thru = (uint8_t)atoi(v);
  if (qget(q, "out_delay_us", v, sizeof v)) r.cfg.out_delay_us = (uint32_t)atol(v);
  if (qget(q, "latency_frames", v, sizeof v)) r.cfg.latency_frames = (uint32_t)atol(v);
  bool bad = false;
  for (int i = 0; i < GR_HUB_ENDPOINTS; ++i) {
    char key[8];
    snprintf(key, sizeof key, "in%d", i);
    if (qget(q, key, v, sizeof v)) bad |= !parse_ep(v, r.in[i]);
    snprintf(key, sizeof key, "out%d", i);
    if (qget(q, key, v, sizeof v)) {
      char *comma = strchr(v, ',');
      if (comma) *comma = 0;
      bad |= !parse_ep(v, r.out[i][0]);
      if (comma) bad |= !parse_ep(comma + 1, r.out[i][1]);
    }
  }
  if (bad) {
    o.raw("{\"ok\":false,\"error\":\"endpoint: a0..b9\"}");
    return o.len;
  }
  const int rc = apply(r);
  o.raw("{\"ok\":%s,\"rc\":%d}", rc == GR_OK ? "true" : "false", rc);
  return o.len;
}

size_t graph_state_json(char *out, size_t cap) {
  static gr_hub_stats snap;
  static bool have = false;
  if (gr_hub_stats_take(&hub, &snap)) have = true;  // the web task is the one consumer
  JsonOut o{out, cap};
  o.raw("{\"graph\":\"gr_hub\",\"app\":\"%s\",\"live\":%s,\"sizeof_hub\":%u,", route.app_name,
        live ? "true" : "false", (unsigned)sizeof(gr_hub));
  o.raw("\"config\":{\"split_key\":%u,\"ch_lower\":%u,\"ch_upper\":%u,\"transpose\":%d,\"split_t\":%d,\"bpm\":%.2f,"
        "\"plain\":%u,\"latency_frames\":%lu,\"out_delay_us\":%lu,\"sample_rate\":%lu},",
        route.cfg.split_key, route.cfg.ch_lower, route.cfg.ch_upper, route.cfg.transpose, route.cfg.split_transpose,
        (double)route.cfg.bpm, route.cfg.clock_plain_thru, (unsigned long)hub.clk.latency_frames,
        (unsigned long)hub.clk.out_delay_us, (unsigned long)route.cfg.sample_rate);
  o.raw("\"in\":[");
  for (int i = 0; i < hub.n_in; ++i) {
    o.raw(i ? ",{\"ep\":" : "{\"ep\":");
    ep_json(o, route.in[i]);
    o.raw(",\"addr\":%u,\"pushed\":%lu,\"refused\":%lu}", in_addr[i], (unsigned long)ic.pushed[i],
          (unsigned long)ic.refused[i]);
  }
  o.raw("],\"out\":[");
  for (int i = 0; i < hub.n_out; ++i) {
    o.raw(i ? ",{\"to\":[" : "{\"to\":[");
    for (int k = 0; k < kFan; ++k) {
      if (k) o.raw(",");
      ep_json(o, route.out[i][k]);
    }
    o.raw("],\"addr\":[%u,%u],\"sent\":%lu}", out_addr[i][0], out_addr[i][1], (unsigned long)oc.sent[i]);
  }
  o.raw("],\"client\":{\"sysex\":%lu,\"unmapped\":%lu,\"releases\":%lu,\"released_notes\":%lu,\"connects\":%lu,"
        "\"disconnects\":%lu},",
        (unsigned long)ic.sysex, (unsigned long)ic.unmapped, (unsigned long)ic.releases,
        (unsigned long)ic.released_notes, (unsigned long)ic.connects, (unsigned long)ic.disconnects);
  o.raw("\"wire\":{\"nodest\":%lu,\"send_fail\":%lu,\"non_midi1\":%lu,\"late_n\":%lu,\"late_mean_us\":%lu,"
        "\"late_max_us\":%lu,\"timer_wakes\":%lu,\"tick_wakes\":%lu,\"late_bins_us\":[0,50,100,200,500,1000,2000],"
        "\"late_hist\":[",
        (unsigned long)oc.nodest, (unsigned long)oc.send_fail, (unsigned long)oc.non_midi1, (unsigned long)oc.late_n,
        (unsigned long)(oc.late_n ? oc.late_sum_us / oc.late_n : 0), (unsigned long)oc.late_max_us,
        (unsigned long)oc.timer_wakes, (unsigned long)oc.tick_wakes);
  for (int b = 0; b < kLateBins; ++b) o.raw("%s%lu", b ? "," : "", (unsigned long)oc.late_bin[b]);
  o.raw("]},\"block_clock\":{\"runs\":%lu,\"frames48\":%lu,\"frames_other\":%lu,\"frames_min\":%lu,"
        "\"frames_max\":%lu,\"wakes\":%lu,\"wake_min_us\":%lu,\"wake_max_us\":%lu,"
        "\"wake_bins_us\":[0,500,900,1100,1500,2000,3000,10000],\"wake_hist\":[",
        (unsigned long)gc.runs, (unsigned long)gc.frames48, (unsigned long)gc.frames_other,
        (unsigned long)gc.frames_min, (unsigned long)gc.frames_max, (unsigned long)gc.wakes,
        (unsigned long)gc.wake_min_us, (unsigned long)gc.wake_max_us);
  for (int b = 0; b < kWakeBins; ++b) o.raw("%s%lu", b ? "," : "", (unsigned long)gc.wake_bin[b]);
  o.raw("]}");
  if (have) {
    const double tpu = snap.ticks_per_us ? (double)snap.ticks_per_us : 1.0;
    o.raw(",\"stats\":{\"blocks\":%lu,\"frames\":%lu,\"ticks_per_us\":%lu,\"block_max_us\":%.3f,\"ev_dropped\":%lu,"
          "\"in_dropped\":%lu,\"out_dropped\":%lu,\"late\":%lu,\"in_events\":%lu,\"out_events\":%lu,\"resyncs\":%lu,"
          "\"nodes\":[",
          (unsigned long)snap.blocks, (unsigned long)snap.frames, (unsigned long)snap.ticks_per_us,
          snap.block_max_ticks / tpu, (unsigned long)snap.ev_dropped, (unsigned long)snap.in_dropped,
          (unsigned long)snap.out_dropped, (unsigned long)snap.late, (unsigned long)snap.in_events,
          (unsigned long)snap.out_events, (unsigned long)snap.resyncs);
    for (int n = 0; n < snap.n_nodes; ++n)
      o.raw("%s{\"id\":%d,\"name\":\"%s\",\"mean_us\":%.3f,\"max_us\":%.3f,\"last_us\":%.3f}", n ? "," : "", n,
            snap.node[n].name ? snap.node[n].name : "?", snap.node[n].mean_ticks / tpu, snap.node[n].max_ticks / tpu,
            snap.node[n].last_ticks / tpu);
    o.raw("]}");
  }
  o.raw("}");
  return o.len;
}
