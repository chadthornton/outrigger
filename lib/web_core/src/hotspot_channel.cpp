#include "hotspot_channel.h"

#include <string.h>

namespace web_core {

uint8_t pick_hotspot_channel(const ScanEntry *entries, size_t n, const char *preferred) {
  const ScanEntry *best = nullptr, *best_preferred = nullptr;
  for (size_t i = 0; i < n; ++i) {
    const ScanEntry &e = entries[i];
    if (!e.ssid || !e.ssid[0] || e.channel < 1 || e.channel > 13) continue;
    if (!best || e.rssi > best->rssi) best = &e;
    if (preferred && preferred[0] && strcmp(e.ssid, preferred) == 0 &&
        (!best_preferred || e.rssi > best_preferred->rssi))
      best_preferred = &e;
  }
  if (best_preferred) return best_preferred->channel;
  return best ? best->channel : kDefaultHotspotChannel;
}

}  // namespace web_core
