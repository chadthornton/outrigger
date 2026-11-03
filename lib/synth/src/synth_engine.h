// Outrigger Synth inside the firmware (APPS=synth): Felucca rendered on its own
// task on core 1, out of Big-S3's speaker. The router hands it controller
// messages from the USB task; the screen reads its render numbers.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace synth {

// Starts the codec, Felucca and the audio task, then the amplifier. Call it
// from the task that polls touch, before the USB host starts: the codec and
// the amplifier share the touch chip's I2C bus. False if any part failed.
bool synth_begin();

// A channel voice message for Felucca. Safe from the USB task: it only queues,
// and never waits. False if the queue was full and the message was dropped.
bool synth_midi(uint8_t status, uint8_t data1, uint8_t data2);

struct SynthStats {
  uint32_t budget_us;      // one block's time at 44.1 kHz
  uint32_t render_avg_us;  // since the last synth_stats() call
  uint32_t render_max_us;
  uint32_t over_budget;    // blocks that took longer than budget_us, since start
  uint32_t underruns;      // times the speaker ran dry, since start
  uint32_t dropped;        // messages lost to a full queue, since start
};
// Takes and resets the window's average and maximum.
SynthStats synth_stats();

// One line for the Setup screen, such as "Synth 812/1451 us (56%), max 990, 0 underruns".
void synth_status_line(char *out, size_t size);

}  // namespace synth
