// `make synth-bench`: Outrigger Synth's bench for Big-S3 (Waveshare 3.5"). It
// runs the board's own drivers (board.cpp) and the synth's audio out
// (lib/synth), with the USB port left a serial device so the Mac reads the
// numbers. Nothing here hosts USB.
//
// Serial commands (115200, one letter each):
//   t / T  a 440 Hz tone at 48 / 44.1 kHz     s  stop (amp off, then silence)
//   + / -  codec volume up / down 5           r  report now
//
// Every 10 s while sound plays, one AUDIO line: seconds playing, blocks
// rendered, underruns (the DMA ran dry), and render time per block (average,
// maximum, blocks over budget) against the block's budget.
#include <Arduino.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <math.h>

#include "audio_out.h"
#include "board.cpp"  // the drivers themselves, as the board probe does

namespace {

constexpr size_t kBlock = 64;
constexpr float kToneHz = 440.0f;
constexpr float kToneLevel = 0.125f;      // -18 dBFS
constexpr uint32_t kFadeFrames = 2400;    // 50 ms at 48 kHz
constexpr uint32_t kReportMs = 10000;

enum class Sound : uint8_t { None, Tone };
volatile Sound want = Sound::None;
volatile uint32_t want_rate = 48000;

// Written by the audio task, read by loop() for the report.
struct Stats {
  uint32_t blocks, over_budget, render_us_max;
  uint64_t render_us_total;
};
Stats stats;
portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t started_ms = 0;

void tone_block(int16_t *out, size_t frames, uint32_t rate, float &phase, uint32_t &fade) {
  const float step = 2.0f * float(M_PI) * kToneHz / rate;
  for (size_t i = 0; i < frames; ++i) {
    const float gain = kToneLevel * (fade < kFadeFrames ? float(fade++) / kFadeFrames : 1.0f);
    const int16_t s = int16_t(sinf(phase) * gain * 32767.0f);
    out[2 * i] = out[2 * i + 1] = s;
    phase += step;
    if (phase > 2.0f * float(M_PI)) phase -= 2.0f * float(M_PI);
  }
}

// Core 1, above loop() (priority 1): it blocks on the I2S write, so it takes
// CPU only for each block's render.
void audio_task(void *) {
  static int16_t block[kBlock * 2];
  Sound playing = Sound::None;
  float phase = 0;
  uint32_t fade = 0;
  for (;;) {
    if (want != playing || (playing != Sound::None && want_rate != synth::audio_out_rate())) {
      synth::audio_out_amp(false);
      playing = want;
      if (playing == Sound::None) {
        synth::audio_out_end();
        Serial.println("AUDIO stopped");
        continue;
      }
      if (!synth::audio_out_begin(want_rate)) {
        Serial.printf("AUDIO start failed at %u Hz\n", unsigned(want_rate));
        want = playing = Sound::None;
        continue;
      }
      phase = 0;
      fade = 0;
      portENTER_CRITICAL(&stats_lock);
      stats = {};
      portEXIT_CRITICAL(&stats_lock);
      started_ms = millis();
      // A few blocks of silence through the stream before the amp comes on.
      memset(block, 0, sizeof block);
      for (int i = 0; i < 8; ++i) synth::audio_out_write(block, kBlock);
      synth::audio_out_amp(true);
      Serial.printf("AUDIO tone %.0f Hz at %u Hz, volume %d\n", kToneHz, unsigned(want_rate), synth::audio_out_volume());
    }
    if (playing == Sound::None) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    const int64_t t0 = esp_timer_get_time();
    tone_block(block, kBlock, want_rate, phase, fade);
    const uint32_t us = uint32_t(esp_timer_get_time() - t0);
    const uint32_t budget_us = uint32_t(1000000ull * kBlock / want_rate);
    portENTER_CRITICAL(&stats_lock);
    stats.blocks++;
    stats.render_us_total += us;
    if (us > stats.render_us_max) stats.render_us_max = us;
    if (us > budget_us) stats.over_budget++;
    portEXIT_CRITICAL(&stats_lock);
    synth::audio_out_write(block, kBlock);
  }
}

void report() {
  if (!synth::audio_out_rate()) {
    Serial.println("AUDIO idle");
    return;
  }
  portENTER_CRITICAL(&stats_lock);
  const Stats s = stats;
  portEXIT_CRITICAL(&stats_lock);
  const uint32_t budget_us = uint32_t(1000000ull * kBlock / synth::audio_out_rate());
  Serial.printf("AUDIO t=%lus blocks=%lu underruns=%lu render_avg_us=%lu render_max_us=%lu over_budget=%lu budget_us=%lu volume=%d heap_internal=%u\n",
                (unsigned long)((millis() - started_ms) / 1000), (unsigned long)s.blocks,
                (unsigned long)synth::audio_out_underruns(), (unsigned long)(s.blocks ? s.render_us_total / s.blocks : 0),
                (unsigned long)s.render_us_max, (unsigned long)s.over_budget, (unsigned long)budget_us,
                synth::audio_out_volume(), unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);  // time for the Mac to open the port
  // Holds EXIO7 low (the amp off) and starts Wire on the touch bus.
  if (!board_display_begin()) Serial.println("BENCH display failed");
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  Serial.printf("BENCH synth bench, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  xTaskCreatePinnedToCore(audio_task, "audio", 6144, nullptr, 10, nullptr, 1);
  Serial.println("BENCH ready: t/T tone 48k/44.1k, s stop, +/- volume, r report");
}

void loop() {
  static uint32_t last = 0;
  while (Serial.available()) {
    switch (Serial.read()) {
      case 't': want_rate = 48000; want = Sound::Tone; break;
      case 'T': want_rate = 44100; want = Sound::Tone; break;
      case 's': want = Sound::None; break;
      case '+': synth::audio_out_volume(synth::audio_out_volume() + 5); report(); break;
      case '-': synth::audio_out_volume(synth::audio_out_volume() - 5); report(); break;
      case 'r': report(); break;
    }
  }
  if (millis() - last >= kReportMs) {
    last = millis();
    if (want != Sound::None) report();
  }
  delay(10);
}
