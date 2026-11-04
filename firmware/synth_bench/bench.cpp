// `make synth-bench`: Outrigger Synth's bench for Big-S3 (Waveshare 3.5"). It
// runs the board's own drivers (board.cpp) and the synth's audio out
// (lib/synth), with the USB port left a serial device so the Mac reads the
// numbers. Nothing here hosts USB.
//
// Serial commands (115200, one letter each):
//   t / T  a 440 Hz tone at 48 / 44.1 kHz     s  stop (amp off, then silence)
//   f      Felucca at 44.1 kHz                n / p  next engine / next preset
//   0 1 4 8  hold that many notes             a  amp on / off (sound keeps running)
//   + / -  codec volume up / down 5           r  report now
//   B      the render-time sweep: every engine's first preset with 1, 4 and 8
//          notes held, amp off, one BENCH line each
//
// Every 10 s while sound plays, one AUDIO line: seconds since the last change,
// blocks rendered, underruns (the DMA ran dry), and render time per block
// (average, maximum, blocks over budget) against the block's budget.
#include <Arduino.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <math.h>

#include "audio_out.h"
#include "board.cpp"  // the drivers themselves, as the board probe does
#include "felucca.h"

namespace {

constexpr size_t kBlock = 64;
constexpr float kToneHz = 440.0f;
constexpr float kToneLevel = 0.125f;      // -18 dBFS
constexpr uint32_t kFadeFrames = 2400;    // 50 ms at 48 kHz
constexpr uint32_t kReportMs = 10000;

enum class Sound : uint8_t { None, Tone, Felucca };
volatile Sound want = Sound::None;
volatile uint32_t want_rate = 48000;
volatile bool want_amp = true;

const fm1_synth_api_t *felucca = nullptr;

// Commands for Felucca, applied by the audio task between blocks: every
// Felucca call happens on that one task (fm1_synth.h).
struct Command {
  enum Kind : uint8_t { Midi, Preset, Reset } kind;
  uint8_t a, b, c;
};
QueueHandle_t commands;

// Written by the audio task, read by loop() for the report.
struct Stats {
  uint32_t blocks, over_budget, render_us_max, underruns_at_start;
  uint64_t render_us_total;
  uint32_t since_ms;
};
Stats stats;
portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
uint8_t held = 0;  // notes held, loop()'s view

void reset_stats() {
  portENTER_CRITICAL(&stats_lock);
  stats = {};
  stats.underruns_at_start = synth::audio_out_underruns();
  stats.since_ms = millis();
  portEXIT_CRITICAL(&stats_lock);
}

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
  bool amp = false;
  float phase = 0;
  uint32_t fade = 0;
  for (;;) {
    if (want != playing || (playing != Sound::None && want_rate != synth::audio_out_rate())) {
      synth::audio_out_amp(amp = false);
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
      if (playing == Sound::Felucca && felucca->init(FM1_SYNTH_SAMPLE_RATE) != 0) {
        Serial.println("AUDIO Felucca init failed");
        synth::audio_out_end();
        want = playing = Sound::None;
        continue;
      }
      phase = 0;
      fade = 0;
      // A few blocks of silence through the stream before the amp comes on.
      memset(block, 0, sizeof block);
      for (int i = 0; i < 8; ++i) synth::audio_out_write(block, kBlock);
      reset_stats();
      if (playing == Sound::Tone)
        Serial.printf("AUDIO tone %.0f Hz at %u Hz, volume %d\n", kToneHz, unsigned(want_rate), synth::audio_out_volume());
      else
        Serial.printf("AUDIO Felucca %s at %u Hz, volume %d\n", felucca->revision, unsigned(want_rate), synth::audio_out_volume());
    }
    if (playing == Sound::None) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (want_amp != amp) synth::audio_out_amp(amp = want_amp);
    for (Command c; xQueueReceive(commands, &c, 0) == pdTRUE;) {
      if (playing != Sound::Felucca) continue;
      if (c.kind == Command::Midi) felucca->midi(c.a, c.b, c.c);
      if (c.kind == Command::Preset) felucca->set_preset(c.a, c.b);
      if (c.kind == Command::Reset) reset_stats();
    }
    const int64_t t0 = esp_timer_get_time();
    if (playing == Sound::Tone) tone_block(block, kBlock, want_rate, phase, fade);
    else felucca->render(block, kBlock);
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

void send(Command c) { xQueueSend(commands, &c, portMAX_DELAY); }

// Chords from the bottom up, so 4 and 8 notes share their lowest notes.
constexpr uint8_t kNotes[8] = {60, 64, 67, 71, 48, 52, 55, 59};

void hold(uint8_t n) {
  for (uint8_t i = 0; i < held; ++i) send({Command::Midi, 0x80, kNotes[i], 0});
  for (uint8_t i = 0; i < n; ++i) send({Command::Midi, 0x90, kNotes[i], 100});
  held = n;
}

void line(const char *tag) {
  if (!synth::audio_out_rate()) {
    Serial.println("AUDIO idle");
    return;
  }
  portENTER_CRITICAL(&stats_lock);
  const Stats s = stats;
  portEXIT_CRITICAL(&stats_lock);
  const uint32_t budget_us = uint32_t(1000000ull * kBlock / synth::audio_out_rate());
  const uint32_t avg = s.blocks ? uint32_t(s.render_us_total / s.blocks) : 0;
  Serial.printf("%s", tag);
  if (want == Sound::Felucca)
    Serial.printf(" engine=%s preset=%s notes=%u", felucca->engine_name(felucca->current_engine()),
                  felucca->preset_name(felucca->current_engine(), felucca->current_preset()), held);
  Serial.printf(" t=%lus blocks=%lu underruns=%lu render_avg_us=%lu render_max_us=%lu over_budget=%lu budget_us=%lu cpu_avg=%lu%% volume=%d heap_internal=%u\n",
                (unsigned long)((millis() - s.since_ms) / 1000), (unsigned long)s.blocks,
                (unsigned long)(synth::audio_out_underruns() - s.underruns_at_start), (unsigned long)avg,
                (unsigned long)s.render_us_max, (unsigned long)s.over_budget, (unsigned long)budget_us,
                (unsigned long)(100 * avg / budget_us), synth::audio_out_volume(),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

void wait_ms(uint32_t ms) {
  const uint32_t start = millis();
  while (millis() - start < ms) delay(10);
}

// Every engine's first preset with 1, 4 and 8 notes: 1 s to settle (the
// preset's fade and the note attacks), then 4 s measured.
void sweep() {
  const bool amp_was = want_amp;
  want_amp = false;
  want_rate = FM1_SYNTH_SAMPLE_RATE;
  want = Sound::Felucca;
  while (synth::audio_out_rate() != FM1_SYNTH_SAMPLE_RATE) delay(10);
  Serial.printf("BENCH sweep: %u engines x 1, 4, 8 notes\n", unsigned(felucca->engine_count()));
  for (uint32_t e = 0; e < felucca->engine_count(); ++e) {
    for (uint8_t n : {1, 4, 8}) {
      hold(0);
      send({Command::Preset, uint8_t(e), 0, 0});
      wait_ms(200);
      hold(n);
      wait_ms(1000);
      send({Command::Reset, 0, 0, 0});
      wait_ms(4000);
      line("BENCH");
    }
  }
  hold(0);
  Serial.println("BENCH sweep done");
  want_amp = amp_was;
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
  felucca = felucca_synth_api();
  if (!felucca_memory_begin()) Serial.println("BENCH Felucca's PSRAM lines didn't fit");
  commands = xQueueCreate(64, sizeof(Command));
  xTaskCreatePinnedToCore(audio_task, "audio", 8192, nullptr, 10, nullptr, 1);
  Serial.println("BENCH ready: t/T tone, f Felucca, n/p engine/preset, 0/1/4/8 notes, a amp, s stop, +/- volume, r report, B sweep");
}

void loop() {
  static uint32_t last = 0;
  while (Serial.available()) {
    const int c = Serial.read();
    switch (c) {
      case 't': want_rate = 48000; want = Sound::Tone; break;
      case 'T': want_rate = 44100; want = Sound::Tone; break;
      case 'f': want_rate = FM1_SYNTH_SAMPLE_RATE; want = Sound::Felucca; break;
      case 's': hold(0); want = Sound::None; break;
      case 'a': want_amp = !want_amp; Serial.printf("AUDIO amp %s\n", want_amp ? "on" : "off"); break;
      case 'n': case 'p': {
        if (want != Sound::Felucca) break;
        uint32_t e = felucca->current_engine(), p = felucca->current_preset();
        if (c == 'n') e = (e + 1) % felucca->engine_count(), p = 0;
        else p = (p + 1) % felucca->preset_count(e);
        send({Command::Preset, uint8_t(e), uint8_t(p), 0});
        send({Command::Reset, 0, 0, 0});
        Serial.printf("AUDIO engine %s, preset %s\n", felucca->engine_name(e), felucca->preset_name(e, p));
        break;
      }
      case '0': case '1': case '4': case '8': hold(uint8_t(c - '0')); send({Command::Reset, 0, 0, 0}); break;
      case '+': synth::audio_out_volume(synth::audio_out_volume() + 5); line("AUDIO"); break;
      case '-': synth::audio_out_volume(synth::audio_out_volume() - 5); line("AUDIO"); break;
      case 'r': line("AUDIO"); break;
      case 'B': sweep(); break;
    }
  }
  if (millis() - last >= kReportMs) {
    last = millis();
    if (want != Sound::None) line("AUDIO");
  }
  delay(10);
}
