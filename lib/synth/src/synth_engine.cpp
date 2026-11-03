#include "synth_engine.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <stdio.h>

#include "audio_out.h"
#include "felucca.h"

namespace synth {
namespace {

constexpr size_t kBlock = 64;  // 1.45 ms at 44.1 kHz
constexpr uint32_t kBudgetUs = uint32_t(1000000ull * kBlock / FM1_SYNTH_SAMPLE_RATE);

struct Midi {
  uint8_t status, data1, data2;
};
QueueHandle_t midi_queue = nullptr;
const fm1_synth_api_t *felucca = nullptr;

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t window_blocks = 0, window_max_us = 0, over_budget = 0, dropped = 0;
uint64_t window_total_us = 0;

// Core 1, above loop() and LVGL: it blocks on the I2S write, so it takes CPU
// only for each block's render. Every Felucca call happens here (fm1_synth.h).
void audio_task(void *) {
  static int16_t block[kBlock * 2];
  for (;;) {
    for (Midi m; xQueueReceive(midi_queue, &m, 0) == pdTRUE;) felucca->midi(m.status, m.data1, m.data2);
    const int64_t t0 = esp_timer_get_time();
    felucca->render(block, kBlock);
    const uint32_t us = uint32_t(esp_timer_get_time() - t0);
    portENTER_CRITICAL(&lock);
    ++window_blocks;
    window_total_us += us;
    if (us > window_max_us) window_max_us = us;
    if (us > kBudgetUs) ++over_budget;
    portEXIT_CRITICAL(&lock);
    audio_out_write(block, kBlock);
  }
}

}  // namespace

bool synth_begin() {
  felucca = felucca_synth_api();
  midi_queue = xQueueCreate(64, sizeof(Midi));
  if (!midi_queue || !felucca_memory_begin() || felucca->init(FM1_SYNTH_SAMPLE_RATE) != 0) return false;
  if (!audio_out_begin(FM1_SYNTH_SAMPLE_RATE)) return false;
  if (xTaskCreatePinnedToCore(audio_task, "synth", 8192, nullptr, 10, nullptr, 1) != pdPASS) return false;
  delay(50);  // the stream runs before the amplifier comes on, so it doesn't click
  audio_out_amp(true);
  return true;
}

bool synth_midi(uint8_t status, uint8_t data1, uint8_t data2) {
  const Midi m{status, data1, data2};
  if (midi_queue && xQueueSend(midi_queue, &m, 0) == pdTRUE) return true;
  portENTER_CRITICAL(&lock);
  ++dropped;
  portEXIT_CRITICAL(&lock);
  return false;
}

SynthStats synth_stats() {
  SynthStats s{};
  portENTER_CRITICAL(&lock);
  s.render_avg_us = window_blocks ? uint32_t(window_total_us / window_blocks) : 0;
  s.render_max_us = window_max_us;
  s.over_budget = over_budget;
  s.dropped = dropped;
  window_blocks = window_max_us = 0;
  window_total_us = 0;
  portEXIT_CRITICAL(&lock);
  s.budget_us = kBudgetUs;
  s.underruns = audio_out_underruns();
  return s;
}

void synth_status_line(char *out, size_t size) {
  const SynthStats s = synth_stats();
  snprintf(out, size, "Synth %lu/%lu us (%lu%%), max %lu, %lu underruns", (unsigned long)s.render_avg_us,
           (unsigned long)s.budget_us, (unsigned long)(100 * s.render_avg_us / s.budget_us),
           (unsigned long)s.render_max_us, (unsigned long)s.underruns);
}

}  // namespace synth
