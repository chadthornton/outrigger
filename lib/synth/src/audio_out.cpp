#include "audio_out.h"

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_attr.h>

#include "es8311/es8311.h"

// board.cpp, in builds with APP_SYNTH: a GPIO or expander pin (100 + n is EXIOn).
void board_pin_write(int16_t pin, bool high);

namespace synth {
namespace {

// Big-S3's audio wiring (Waveshare's 01_audio_out and 04_es8311_example, and
// boards.json's hold_why for EXIO7). Pins stay here, not in the board profile,
// until a second board plays sound.
constexpr int kMclk = 12, kBclk = 13, kLrck = 15, kDout = 16;
constexpr int16_t kAmpPin = 100 + 7;  // EXIO7, PA_CTRL
constexpr uint32_t kMclkMultiple = 256;
// -19.5 dB at the DAC: quiet on first power-up. Waveshare's examples use 70 (-6.5 dB).
constexpr int kStartVolume = 60;

// 4 x 128 frames: 10.7 ms of sound queued at 48 kHz, 11.6 ms at 44.1 kHz.
constexpr uint32_t kDmaBuffers = 4, kDmaFrames = 128;

i2s_chan_handle_t tx = nullptr;
es8311_handle_t codec = nullptr;
uint32_t rate_hz = 0;
int volume = 0;
volatile uint32_t underruns = 0;

// The send queue overflows when every DMA buffer has played and none was
// refilled: the stream ran dry, and auto_clear sends silence.
bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  underruns = underruns + 1;
  return false;
}

}  // namespace

bool audio_out_begin(uint32_t rate) {
  audio_out_end();
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = kDmaBuffers;
  chan.dma_frame_num = kDmaFrames;
  chan.auto_clear = true;
  if (i2s_new_channel(&chan, &tx, nullptr) != ESP_OK) return false;
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = gpio_num_t(kMclk),
          .bclk = gpio_num_t(kBclk),
          .ws = gpio_num_t(kLrck),
          .dout = gpio_num_t(kDout),
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {},
      },
  };
  std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  i2s_event_callbacks_t callbacks = {};
  callbacks.on_send_q_ovf = on_send_q_ovf;
  if (i2s_channel_init_std_mode(tx, &std) != ESP_OK || i2s_channel_register_event_callback(tx, &callbacks, nullptr) != ESP_OK) {
    audio_out_end();
    return false;
  }
  // Silence in every buffer first, so the stream starts full rather than
  // counting underruns before the first write.
  static const int16_t zeros[kDmaFrames * 2] = {};
  for (size_t loaded = sizeof zeros; loaded == sizeof zeros;) i2s_channel_preload_data(tx, zeros, sizeof zeros, &loaded);
  if (i2s_channel_enable(tx) != ESP_OK) {
    audio_out_end();
    return false;
  }
  // The codec after the clocks, as Espressif's i2s_es8311 example does it.
  if (!codec) codec = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
  const es8311_clock_config_t clk = {
      .mclk_inverted = false,
      .sclk_inverted = false,
      .mclk_from_mclk_pin = true,
      .mclk_frequency = int(rate * kMclkMultiple),
      .sample_frequency = int(rate),
  };
  if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK ||
      !audio_out_volume(volume ? volume : kStartVolume)) {
    audio_out_end();
    return false;
  }
  rate_hz = rate;
  underruns = 0;
  return true;
}

void audio_out_end() {
  if (!tx) return;
  audio_out_amp(false);
  i2s_channel_disable(tx);
  i2s_del_channel(tx);
  tx = nullptr;
  rate_hz = 0;
}

bool audio_out_write(const int16_t *stereo, size_t frames) {
  if (!tx) return false;
  size_t written = 0;
  return i2s_channel_write(tx, stereo, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY) == ESP_OK;
}

void audio_out_amp(bool on) { board_pin_write(kAmpPin, on); }

bool audio_out_volume(int percent) {
  if (!codec) return false;
  int set = 0;
  if (es8311_voice_volume_set(codec, percent, &set) != ESP_OK) return false;
  volume = set;
  return true;
}

int audio_out_volume() { return volume; }
uint32_t audio_out_underruns() { return underruns; }
uint32_t audio_out_rate() { return rate_hz; }

}  // namespace synth
