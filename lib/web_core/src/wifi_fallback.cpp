#include "wifi_fallback.h"

namespace web_core {

void WifiFallback::boot_join(uint32_t now) {
  boot_at_ = now;
  booted_ = true;
}

WifiFallback::Do WifiFallback::tick(uint32_t now, const Seen &s) {
  if (s.online) {
    ever_online_ = true;
    retrying_ = false;
    return Do::Nothing;
  }
  if (ever_online_ || !s.saved) return Do::Nothing;  // a drop: the library's own reconnect
  if (s.setup_join) {  // the setup page's join owns the station; ours waits a full period after it
    retrying_ = false;
    next_retry_at_ = now + kRetryEveryMs;
    return Do::Nothing;
  }
  if (s.hotspot_on) {
    if (retrying_) {
      if (now - retry_started_ < kRetryGiveUpMs) return Do::Nothing;
      retrying_ = false;
      return Do::StopJoin;
    }
    if (int32_t(now - next_retry_at_) < 0 || s.phones > 0) return Do::Nothing;
    retrying_ = true;
    retry_started_ = now;
    next_retry_at_ = now + kRetryEveryMs;
    return Do::Join;
  }
  if (!booted_) return Do::Nothing;
  const uint32_t since = now - boot_at_;
  if (since >= kHotspotAfterMs) {
    next_retry_at_ = now + kRetryEveryMs;
    return Do::OpenHotspot;
  }
  if (s.stalled && nudges_ < 2 && since >= (nudges_ == 0 ? kFirstNudgeMs : kSecondNudgeMs)) {
    ++nudges_;
    return Do::Nudge;
  }
  return Do::Nothing;
}

}  // namespace web_core
