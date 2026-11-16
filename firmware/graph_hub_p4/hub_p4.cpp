// The graph hub on Big-P4 (spike night 1, track S2): a standalone build and
// smoke test of lib/graph's gr_hub, and the template HW2 copies into HW1's
// firmware (docs/graph-p4.md).
//
// - Core 1: the graph task. Every FreeRTOS tick (1 ms) it runs the frames the
//   block clock is behind esp_timer (48 at 48 kHz), so blocks follow real time.
// - Core 0: a stand-in for the USB host task. It pushes a synthetic stream
//   (GRAPH_HUB_LOAD_HZ messages a second, round robin over the app's inputs)
//   and drains every output when due, measuring each message's latency.
//   HW2 replaces it with EspUsbHost's MIDI callbacks and sends.
// - loop(): one STATS line a second on UART0 (Big-P4's UART-C port).
//
// Console commands (UART0, 115200): s = stats now, r = reset maxima.
#include <Arduino.h>
#include <esp_timer.h>

#include "gr_hub.h"

#ifndef GRAPH_HUB_APP
#define GRAPH_HUB_APP GR_HUB_APP_MERGE
#endif
#ifndef GRAPH_HUB_LOAD_HZ
#define GRAPH_HUB_LOAD_HZ 2000
#endif

static gr_hub hub;  // ~129 KB: static, never on a stack

static uint32_t now_us() { return (uint32_t)esp_timer_get_time(); }

// Latency seen by the stand-in USB task: it alone writes these (core 0), as
// 32-bit atomic values; loop() only reads them (CONTRACT.md rule 1).
static uint32_t lat_min = UINT32_MAX, lat_max = 0, lat_n = 0, lat_mean = 0;
static uint64_t lat_sum = 0;  // the USB task's own
static void put(uint32_t *v, uint32_t x) { __atomic_store_n(v, x, __ATOMIC_RELAXED); }
static uint32_t get(const uint32_t *v) { return __atomic_load_n(v, __ATOMIC_RELAXED); }
// loop() asks for a reset with a counter it alone writes; the USB task does
// the reset and remembers the last request it handled (CONTRACT.md rule 1).
static uint32_t reset_req = 0;  // loop() writes, atomically
static uint32_t reset_done = 0;
static uint32_t sent_us[256];  // send time by sequence number (the CC value + MSB in the note)

static void graph_task(void *) {
  for (;;) {
    uint32_t t = now_us(), f;
    while ((f = gr_hub_frames_due(&hub, t)) != 0) gr_hub_run(&hub, f, t);
    vTaskDelay(1);
  }
}

static void usb_standin_task(void *) {
  const uint32_t period = 1000000u / GRAPH_HUB_LOAD_HZ;
  uint32_t next = now_us(), seq = 0;
  for (;;) {
    uint32_t t = now_us();
    if (get(&reset_req) != reset_done) {
      reset_done = get(&reset_req);
      put(&lat_min, UINT32_MAX), put(&lat_max, 0), put(&lat_n, 0), put(&lat_mean, 0);
      lat_sum = 0;
    }
    while (gr_us_reached(t, next)) {
      uint8_t m[3] = {(uint8_t)(0xB0 | (seq >> 7 & 1)), 20, (uint8_t)(seq & 0x7F)};
      sent_us[seq & 0xFF] = t;
      gr_hub_in_midi1(&hub, (int)(seq % hub.n_in), t, m, 3);
      seq = (seq + 1) & 0xFF;
      next += period;
    }
    for (int o = 0; o < hub.n_out; ++o) {
      uint8_t m[3];
      int len;
      while (gr_hub_out_midi1(&hub, o, t, m, &len)) {
        if ((m[0] & 0xF0) != 0xB0) continue;  // the clock app's clocks
        uint32_t s = ((m[0] & 1u) << 7) | m[2];
        uint32_t lat = t - sent_us[s];
        if (lat < get(&lat_min)) put(&lat_min, lat);
        if (lat > get(&lat_max)) put(&lat_max, lat);
        lat_sum += lat;
        put(&lat_n, get(&lat_n) + 1);
        put(&lat_mean, (uint32_t)(lat_sum / get(&lat_n)));
      }
    }
    vTaskDelay(1);
  }
}

static void print_stats() {
  gr_hub_stats s;
  if (!gr_hub_stats_take(&hub, &s)) {
    Serial.println("STATS none yet");
    return;
  }
  const uint32_t tpu = s.ticks_per_us ? s.ticks_per_us : 1;
  Serial.printf("STATS app=%d blocks=%lu frames=%lu in=%lu out=%lu late=%lu drop_in=%lu drop_out=%lu "
                "drop_ev=%lu resyncs=%lu block_max_us=%.2f lat_us=%lu/%lu/%lu n=%lu\n",
                (int)hub.cfg.app, (unsigned long)s.blocks, (unsigned long)s.frames,
                (unsigned long)s.in_events, (unsigned long)s.out_events, (unsigned long)s.late,
                (unsigned long)s.in_dropped, (unsigned long)s.out_dropped, (unsigned long)s.ev_dropped,
                (unsigned long)s.resyncs, s.block_max_ticks / (double)tpu,
                (unsigned long)(get(&lat_n) ? get(&lat_min) : 0), (unsigned long)get(&lat_mean),
                (unsigned long)get(&lat_max), (unsigned long)get(&lat_n));
  for (int n = 0; n < s.n_nodes; ++n)
    Serial.printf("NODE %d %s mean_us=%.3f max_us=%.3f last_us=%.3f\n", n, s.node[n].name,
                  s.node[n].mean_ticks / (double)tpu, s.node[n].max_ticks / (double)tpu,
                  s.node[n].last_ticks / (double)tpu);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  gr_hub_config c;
  gr_hub_config_default(&c, (gr_hub_app)GRAPH_HUB_APP);
  c.cpu_mhz = getCpuFrequencyMhz();
  int r = gr_hub_init(&hub, &c);
  Serial.printf("HUB ready app=%d rc=%d cpu=%lu MHz sizeof(gr_hub)=%u in=%d out=%d latency=%lu frames + %lu us\n",
                (int)c.app, r, (unsigned long)c.cpu_mhz, (unsigned)sizeof(gr_hub), hub.n_in, hub.n_out,
                (unsigned long)hub.clk.latency_frames, (unsigned long)hub.clk.out_delay_us);
  if (r != GR_OK) return;
  xTaskCreatePinnedToCore(graph_task, "graph", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
  xTaskCreatePinnedToCore(usb_standin_task, "usb-standin", 4096, nullptr, 5, nullptr, 0);
}

void loop() {
  static uint32_t last = 0;
  while (Serial.available()) {
    int ch = Serial.read();
    if (ch == 's') print_stats();
    if (ch == 'r') put(&reset_req, get(&reset_req) + 1);
  }
  if (millis() - last >= 1000) {
    last = millis();
    print_stats();
  }
  delay(10);
}
