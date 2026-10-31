// Which channel the setup hotspot should use. The ESP32-S3 has one radio, so
// when it joins a network the hotspot moves to that network's channel, which
// drops the phone mid-setup. Starting the hotspot on the channel of the
// network Chad will most likely pick avoids the move.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace web_core {

struct ScanEntry {
  const char *ssid;
  int8_t rssi;
  uint8_t channel;
};

constexpr uint8_t kDefaultHotspotChannel = 6;

// The channel of `preferred` (a saved network) if seen, else of the strongest
// named network; kDefaultHotspotChannel when nothing usable was found.
uint8_t pick_hotspot_channel(const ScanEntry *entries, size_t n, const char *preferred);

}  // namespace web_core
