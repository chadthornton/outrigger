// Outrigger Synth's sound out of Big-S3 (Waveshare ESP32-S3-Touch-LCD-3.5):
// the ES8311 codec on the touch chip's I2C bus, I2S0 into it, and the NS4150B
// speaker amplifier on the expander's EXIO7. The speaker is mono; the codec
// plays the left channel of the stereo stream.
//
// Wire must already run on the touch bus (board_display_begin starts it), and
// the codec is set up only from audio_out_begin and audio_out_volume: call
// them before touch polling starts, or from the task that polls touch.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace synth {

// Sets up the codec and I2S at `rate` (44100 or 48000, MCLK = 256 x rate) and
// starts sending silence. The amplifier stays off. Calling it again restarts
// both at the new rate.
bool audio_out_begin(uint32_t rate);
void audio_out_end();

// Interleaved stereo int16. Blocks until the DMA buffers have room, so the
// caller's pace is the sample clock's.
bool audio_out_write(const int16_t *stereo, size_t frames);

// The speaker amplifier. Turn it on after the stream runs, off before it stops,
// so neither edge clicks.
void audio_out_amp(bool on);

// The codec's DAC volume, 0..100 (Espressif's scale: 75 is 0 dB, each step
// about 1.3 dB, 100 is +32 dB).
bool audio_out_volume(int percent);
int audio_out_volume();

// Times the DMA ran out of new samples and sent silence instead, since begin.
uint32_t audio_out_underruns();
uint32_t audio_out_rate();

}  // namespace synth
