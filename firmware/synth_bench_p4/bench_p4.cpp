// `make synth-bench-p4`: Felucca's render-time bench on the ESP32-P4 (Big-P4,
// Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5). The same measurement as the Big-S3
// bench (firmware/synth_bench, docs/synth/s3-first-sound.md): microseconds per
// 64-frame block at 44.1 kHz, every engine's first preset with 1, 4 and 8
// notes held. Nothing here hosts USB or drives the screen; the console is the
// native USB Serial/JTAG port.
//
// There is no audio device on this bench, so a timer stands in for one: it
// takes a block from a ring of kRing blocks every 64 / 44.1 kHz, as a DAC
// would, and counts an underrun when the ring is empty. The render task on
// core 1 refills the ring as fast as it can, so an underrun means render time
// really fell behind the sound.
//
// Serial commands (one letter each):
//   f      Felucca running             s  stop
//   n / p  next engine / next preset   0 1 4 8  hold that many notes
//   k / K  renders per block down / up (Felucca run k times per block: how
//          many instances' worth of work fit on the core)
//   r      report now
//   B      the render-time sweep (13 engines x 1, 4, 8 notes, one BENCH line each)
//   C      the capacity sweep: per engine with 8 notes, the most renders per
//          block that still run with 0 underruns and 0 blocks over budget
//   F      float, double and integer cost on this core (cycles per operation)
//   i      board and build info
#include <Arduino.h>
#include <esp_cpu.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <math.h>

#include "felucca.h"

namespace {

constexpr size_t kBlock = 64;
constexpr uint32_t kRate = FM1_SYNTH_SAMPLE_RATE;            // 44,100
constexpr uint32_t kBudgetUs = uint32_t(1000000ull * kBlock / kRate);  // 1,451
constexpr uint32_t kRing = 4;                                 // 5.8 ms queued, as a DAC's buffer
constexpr uint32_t kReportMs = 10000;

const fm1_synth_api_t *felucca = nullptr;

// Commands for Felucca, applied by the render task between blocks: every
// Felucca call happens on that one task (fm1_synth.h).
struct Command {
  enum Kind : uint8_t { Midi, Preset, Reset, Renders } kind;
  uint8_t a, b, c;
};
QueueHandle_t commands;
TaskHandle_t render_handle;

volatile bool want_run = false;
volatile bool running = false;

// The stand-in sink: the timer takes, the render task gives.
volatile uint32_t ring_fill = 0;
volatile uint32_t underruns = 0;
portMUX_TYPE ring_lock = portMUX_INITIALIZER_UNLOCKED;

struct Stats {
  uint32_t blocks, over_budget, render_us_max, underruns_at_start;
  uint64_t render_us_total;
  uint32_t since_ms;
};
Stats stats;
portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
uint8_t held = 0;          // notes held, loop()'s view
uint8_t renders = 1;       // renders per block, loop()'s view

void reset_stats() {
  portENTER_CRITICAL(&stats_lock);
  stats = {};
  stats.underruns_at_start = underruns;
  stats.since_ms = millis();
  portEXIT_CRITICAL(&stats_lock);
}

void sink_tick(void *) {
  if (!running) return;
  portENTER_CRITICAL_ISR(&ring_lock);
  if (ring_fill) --ring_fill;
  else ++underruns;
  portEXIT_CRITICAL_ISR(&ring_lock);
  xTaskNotifyGive(render_handle);
}

// Core 1, high priority. It sleeps while the ring is full, so it takes CPU
// only for each block's render(s).
void render_task(void *) {
  static int16_t block[kBlock * 2];
  uint8_t k = 1;
  for (;;) {
    if (want_run != running) {
      if (want_run) {
        if (felucca->init(kRate) != 0) {
          Serial.println("AUDIO Felucca init failed");
          want_run = false;
          continue;
        }
        portENTER_CRITICAL(&ring_lock);
        ring_fill = kRing;  // starts full of silence, as the S3 bench's 8 silent blocks
        portEXIT_CRITICAL(&ring_lock);
        reset_stats();
        running = true;
        Serial.printf("AUDIO Felucca %s at %u Hz (stand-in sink, ring %u blocks)\n", felucca->revision,
                      unsigned(kRate), unsigned(kRing));
      } else {
        running = false;
        Serial.println("AUDIO stopped");
      }
    }
    if (!running) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    for (Command c; xQueueReceive(commands, &c, 0) == pdTRUE;) {
      if (c.kind == Command::Midi) felucca->midi(c.a, c.b, c.c);
      if (c.kind == Command::Preset) felucca->set_preset(c.a, c.b);
      if (c.kind == Command::Reset) reset_stats();
      if (c.kind == Command::Renders) k = c.a;
    }
    uint32_t fill;
    portENTER_CRITICAL(&ring_lock);
    fill = ring_fill;
    portEXIT_CRITICAL(&ring_lock);
    if (fill >= kRing) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
      continue;
    }
    const int64_t t0 = esp_timer_get_time();
    for (uint8_t i = 0; i < k; ++i) felucca->render(block, kBlock);
    const uint32_t us = uint32_t(esp_timer_get_time() - t0);
    portENTER_CRITICAL(&ring_lock);
    ++ring_fill;
    portEXIT_CRITICAL(&ring_lock);
    portENTER_CRITICAL(&stats_lock);
    stats.blocks++;
    stats.render_us_total += us;
    if (us > stats.render_us_max) stats.render_us_max = us;
    if (us > kBudgetUs) stats.over_budget++;
    portEXIT_CRITICAL(&stats_lock);
    // A block over budget never lets the ring fill, so this task would never
    // sleep and loop() (core 1, priority 1) would starve. That block already
    // failed, so a tick's pause changes no passing measurement.
    if (us > kBudgetUs) vTaskDelay(1);
  }
}

void send(Command c) { xQueueSend(commands, &c, portMAX_DELAY); }

// Chords from the bottom up, so 4 and 8 notes share their lowest notes (the S3 bench's).
constexpr uint8_t kNotes[8] = {60, 64, 67, 71, 48, 52, 55, 59};

void hold(uint8_t n) {
  for (uint8_t i = 0; i < held; ++i) send({Command::Midi, 0x80, kNotes[i], 0});
  for (uint8_t i = 0; i < n; ++i) send({Command::Midi, 0x90, kNotes[i], 100});
  held = n;
}

void set_renders(uint8_t k) {
  renders = k < 1 ? 1 : k;
  send({Command::Renders, renders, 0, 0});
}

Stats snapshot() {
  portENTER_CRITICAL(&stats_lock);
  const Stats s = stats;
  portEXIT_CRITICAL(&stats_lock);
  return s;
}

uint32_t window_underruns(const Stats &s) { return underruns - s.underruns_at_start; }

void line(const char *tag) {
  if (!running) {
    Serial.println("AUDIO idle");
    return;
  }
  const Stats s = snapshot();
  const uint32_t avg = s.blocks ? uint32_t(s.render_us_total / s.blocks) : 0;
  Serial.printf("%s engine=%s preset=%s notes=%u renders=%u t=%lus blocks=%lu underruns=%lu render_avg_us=%lu "
                "render_max_us=%lu over_budget=%lu budget_us=%lu cpu_avg=%lu%% cpu_max=%lu%% heap_internal=%u\n",
                tag, felucca->engine_name(felucca->current_engine()),
                felucca->preset_name(felucca->current_engine(), felucca->current_preset()), held, renders,
                (unsigned long)((millis() - s.since_ms) / 1000), (unsigned long)s.blocks,
                (unsigned long)window_underruns(s), (unsigned long)avg, (unsigned long)s.render_us_max,
                (unsigned long)s.over_budget, (unsigned long)kBudgetUs, (unsigned long)(100 * avg / kBudgetUs),
                (unsigned long)(100 * s.render_us_max / kBudgetUs),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

void wait_ms(uint32_t ms) {
  const uint32_t start = millis();
  while (millis() - start < ms) delay(10);
}

void start_felucca() {
  want_run = true;
  while (!running) delay(10);
}

// One engine's first preset with n notes: 200 ms after the preset, 1 s to
// settle (its fade and the attacks), then measure_ms measured. The S3 bench's
// timings exactly.
void measure(uint32_t e, uint8_t n, uint32_t measure_ms) {
  hold(0);
  send({Command::Preset, uint8_t(e), 0, 0});
  wait_ms(200);
  hold(n);
  wait_ms(1000);
  send({Command::Reset, 0, 0, 0});
  wait_ms(measure_ms);
}

void sweep() {
  start_felucca();
  set_renders(1);
  Serial.printf("BENCH sweep: %u engines x 1, 4, 8 notes\n", unsigned(felucca->engine_count()));
  for (uint32_t e = 0; e < felucca->engine_count(); ++e)
    for (uint8_t n : {1, 4, 8}) {
      measure(e, n, 4000);
      line("BENCH");
    }
  hold(0);
  Serial.println("BENCH sweep done");
}

// Per engine, 8 notes: one render's worst block sets a first guess at how
// many fit, then the guess is run and stepped down until a 4 s run has 0
// underruns and 0 blocks over budget, or up while it still passes.
void capacity() {
  start_felucca();
  Serial.printf("CAPACITY sweep: %u engines, 8 notes, renders per block until it misses\n",
                unsigned(felucca->engine_count()));
  uint32_t fewest = UINT32_MAX;
  for (uint32_t e = 0; e < felucca->engine_count(); ++e) {
    set_renders(1);
    measure(e, 8, 2000);
    Stats s = snapshot();
    uint32_t k = s.render_us_max ? kBudgetUs / s.render_us_max : 1;
    if (k < 1) k = 1;
    if (k > 40) k = 40;
    uint32_t best = 0;
    bool went_down = false;
    for (int tries = 0; tries < 12; ++tries) {
      set_renders(uint8_t(k));
      send({Command::Reset, 0, 0, 0});
      wait_ms(4000);
      s = snapshot();
      const bool pass = window_underruns(s) == 0 && s.over_budget == 0;
      line(pass ? "CAPACITY pass" : "CAPACITY miss");
      if (pass) {
        best = k;
        if (went_down || k >= 40) break;
        ++k;
      } else {
        went_down = true;
        if (best || k == 1) break;
        --k;
      }
    }
    Serial.printf("CAPACITY engine=%s fits=%lu\n", felucca->engine_name(e), (unsigned long)best);
    if (best < fewest) fewest = best;
  }
  set_renders(1);
  hold(0);
  Serial.printf("CAPACITY done fewest=%lu\n", (unsigned long)fewest);
}

// ---- float, double and integer cost (cycles per operation) ----

constexpr int kN = 256;
float fa[kN], fb[kN];
double da[kN], db[kN];
int32_t ia[kN], ib[kN];
volatile float fsink;
volatile double dsink;
volatile int64_t isink;

template <typename F>
uint32_t cycles(F &&body, uint32_t reps) {
  const uint32_t c0 = esp_cpu_get_cycle_count();
  body(reps);
  return esp_cpu_get_cycle_count() - c0;
}

void float_bench() {
  for (int i = 0; i < kN; ++i) {
    fa[i] = 0.001f * float(i + 1);
    fb[i] = 1.0f - 0.0007f * float(i);
    da[i] = fa[i];
    db[i] = fb[i];
    ia[i] = int32_t(fa[i] * 1073741824.0f);
    ib[i] = int32_t(fb[i] * 1073741824.0f);
  }
  const uint32_t reps = 64;  // 64 x 256 = 16,384 operations each
  const float ops = float(reps * kN);
  vTaskDelay(1);
  const uint32_t c_fmac = cycles([](uint32_t r) {
    float acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += fa[i] * fb[i];
    fsink = acc;
  }, reps);
  const uint32_t c_dmac = cycles([](uint32_t r) {
    double acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += da[i] * db[i];
    dsink = acc;
  }, reps);
  const uint32_t c_imac = cycles([](uint32_t r) {
    int32_t acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += int32_t((int64_t(ia[i]) * ib[i]) >> 31);
    isink = acc;
  }, reps);
  const uint32_t c_lmac = cycles([](uint32_t r) {
    int64_t acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += int64_t(ia[i]) * ib[i];
    isink = acc;
  }, reps);
  const uint32_t c_fdiv = cycles([](uint32_t r) {
    float acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += fa[i] / fb[i];
    fsink = acc;
  }, reps);
  const uint32_t c_ddiv = cycles([](uint32_t r) {
    double acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += da[i] / db[i];
    dsink = acc;
  }, reps);
  const uint32_t c_sinf = cycles([](uint32_t r) {
    float acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += sinf(fa[i]);
    fsink = acc;
  }, reps);
  const uint32_t c_sin = cycles([](uint32_t r) {
    double acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += sin(da[i]);
    dsink = acc;
  }, reps);
  const uint32_t c_expf = cycles([](uint32_t r) {
    float acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += expf(fa[i]);
    fsink = acc;
  }, reps);
  const uint32_t c_exp = cycles([](uint32_t r) {
    double acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += exp(da[i]);
    dsink = acc;
  }, reps);
  const uint32_t c_tanhf = cycles([](uint32_t r) {
    float acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += tanhf(fa[i]);
    fsink = acc;
  }, reps);
  const uint32_t c_tanh = cycles([](uint32_t r) {
    double acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += tanh(da[i]);
    dsink = acc;
  }, reps);
  // The easy mistake: a float expression with a double literal (0.5, not 0.5f).
  const uint32_t c_promo = cycles([](uint32_t r) {
    float acc = 0;
    for (uint32_t j = 0; j < r; ++j) for (int i = 0; i < kN; ++i) acc += fa[i] * 0.5;
    fsink = acc;
  }, reps);
  Serial.printf("FLOAT cpu_mhz=%u ops=%u cycles_per_op: float_mac=%.1f double_mac=%.1f q31_mac=%.1f int64_mac=%.1f "
                "float_div=%.1f double_div=%.1f sinf=%.1f sin=%.1f expf=%.1f exp=%.1f tanhf=%.1f tanh=%.1f "
                "float_times_double_literal=%.1f\n",
                unsigned(getCpuFrequencyMhz()), unsigned(reps * kN), c_fmac / ops, c_dmac / ops, c_imac / ops,
                c_lmac / ops, c_fdiv / ops, c_ddiv / ops, c_sinf / ops, c_sin / ops, c_expf / ops, c_exp / ops,
                c_tanhf / ops, c_tanh / ops, c_promo / ops);
}

void info() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  Serial.printf("BENCH P4 synth bench, MAC %02x:%02x:%02x:%02x:%02x:%02x, cpu %u MHz, chip rev %u, "
                "heap internal %u, psram %u free of %u\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], unsigned(getCpuFrequencyMhz()),
                unsigned(ESP.getChipRevision()), unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                unsigned(ESP.getFreePsram()), unsigned(ESP.getPsramSize()));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);  // time for the Mac to open the port
  info();
  felucca = felucca_synth_api();
  if (!felucca_memory_begin()) Serial.println("BENCH Felucca's PSRAM lines didn't fit");
  commands = xQueueCreate(64, sizeof(Command));
  xTaskCreatePinnedToCore(render_task, "render", 8192, nullptr, 10, &render_handle, 1);
  const esp_timer_create_args_t args = {.callback = sink_tick, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
                                        .name = "sink", .skip_unhandled_events = false};
  esp_timer_handle_t sink;
  esp_timer_create(&args, &sink);
  // 64 frames at 44.1 kHz is 1,451.247 us; the timer's 1,451 us runs 0.02% fast,
  // which is the stricter side.
  esp_timer_start_periodic(sink, kBudgetUs);
  Serial.println("BENCH ready: f Felucca, s stop, n/p engine/preset, 0/1/4/8 notes, k/K renders, r report, "
                 "B sweep, C capacity, F float, i info");
}

void loop() {
  static uint32_t last = 0;
  while (Serial.available()) {
    const int c = Serial.read();
    switch (c) {
      case 'f': start_felucca(); break;
      case 's': hold(0); want_run = false; break;
      case 'n': case 'p': {
        if (!running) break;
        uint32_t e = felucca->current_engine(), p = felucca->current_preset();
        if (c == 'n') e = (e + 1) % felucca->engine_count(), p = 0;
        else p = (p + 1) % felucca->preset_count(e);
        send({Command::Preset, uint8_t(e), uint8_t(p), 0});
        send({Command::Reset, 0, 0, 0});
        Serial.printf("AUDIO engine %s, preset %s\n", felucca->engine_name(e), felucca->preset_name(e, p));
        break;
      }
      case '0': case '1': case '4': case '8': hold(uint8_t(c - '0')); send({Command::Reset, 0, 0, 0}); break;
      case 'k': set_renders(renders - 1); send({Command::Reset, 0, 0, 0}); break;
      case 'K': set_renders(renders + 1); send({Command::Reset, 0, 0, 0}); break;
      case 'r': line("AUDIO"); break;
      case 'B': sweep(); break;
      case 'C': capacity(); break;
      case 'F': float_bench(); break;
      case 'i': info(); break;
    }
  }
  if (millis() - last >= kReportMs) {
    last = millis();
    if (running) line("AUDIO");
    else Serial.println("BENCH alive");
  }
  delay(10);
}
