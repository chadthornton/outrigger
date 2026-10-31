// What to do when the saved Wi-Fi network doesn't answer after a boot.
//
// The Arduino core retries a failed first join once. Two failures get past it:
// a second AUTH_FAIL (202) leaves the station at status 4 with no further
// retry, and some joins stall at status 6 with no disconnect event at all
// (3 of 101 soak boots). So the station gets up to two nudges, at 20 s and
// 40 s: WiFi.reconnect(), which on a station that isn't associated is only
// esp_wifi_connect(). A nudge never lands while the station is associated
// (status 0, waiting on DHCP): a forced disconnect over the library's own join
// left the board associated but unreachable (branch boot-probe, 2026-10-30).
//
// The hotspot opens 60 s after the boot join, and only if the network never
// worked since boot: its scan blocks the loop ~2 s, which mustn't land mid-job
// after a mere router hiccup. With the hotspot up, the saved network is retried
// every 30 s, but never while a phone is on the hotspot: a station attempt hops
// channels and would drop the phone mid-setup. A retry that hasn't joined in
// 15 s is stopped for the same reason.
#pragma once

#include <stdint.h>

namespace web_core {

class WifiFallback {
 public:
  static constexpr uint32_t kFirstNudgeMs = 20000;
  static constexpr uint32_t kSecondNudgeMs = 40000;
  static constexpr uint32_t kHotspotAfterMs = 60000;
  static constexpr uint32_t kRetryEveryMs = 30000;
  static constexpr uint32_t kRetryGiveUpMs = 15000;

  enum class Do { Nothing, Nudge, OpenHotspot, Join, StopJoin };

  struct Seen {
    bool saved;       // a saved network exists
    bool online;      // the station has an address (WL_CONNECTED)
    bool stalled;     // not associated and not trying: status 6, 4 or 1
    bool hotspot_on;
    int phones;       // stations on the hotspot
    bool setup_join;  // a join from the setup page is running; it has its own timeout
  };

  // The saved network's join at boot started now.
  void boot_join(uint32_t now);
  // Called every loop; at most one action per call.
  Do tick(uint32_t now, const Seen &s);

 private:
  uint32_t boot_at_ = 0, next_retry_at_ = 0, retry_started_ = 0;
  uint8_t nudges_ = 0;
  bool booted_ = false, ever_online_ = false, retrying_ = false;
};

}  // namespace web_core
