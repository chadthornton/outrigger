// A join from the setup page (web.cpp): the network the phone picked, tried
// once with the hotspot still up. The page polls /setup/state for how it went,
// so a join that works keeps the hotspot a minute for the phone to show it,
// and one that doesn't is stopped after 15 s: a station still retrying hops
// channels and drags the hotspot (and the phone) along.
#pragma once

#include <stdint.h>

namespace web_core {

class SetupJoin {
 public:
  static constexpr uint32_t kGiveUpMs = 15000;
  static constexpr uint32_t kShowOnlineMs = 60000;

  enum class State : uint8_t { Idle, Joining, Online, WrongPassword, NotFound, Failed };

  // join() ran for the page's network at `now` (millis() then, which may be
  // later than the loop's own `now`).
  void start(uint32_t now);
  // The station dropped while joining (wifi_err_reason_t): the last one says
  // why a join that gives up failed.
  void disconnected(uint8_t reason);
  // Called every loop. True once, when a join gives up: the caller stops the station.
  bool tick(uint32_t now, bool connected);

  State state() const { return state_; }
  // A join is under way: the fallback's own retries wait (WifiFallback::Seen::setup_join).
  bool joining() const { return state_ == State::Joining; }
  // Joined from the page under a minute ago: the hotspot stays so the phone sees it.
  bool showing_online(uint32_t now) const;

 private:
  State state_ = State::Idle;
  uint32_t started_ = 0, online_at_ = 0;
  uint8_t reason_ = 0;
};

// "joining", "online", "wrong_password", "not_found", "failed", "idle": the page's word for it.
const char *setup_join_name(SetupJoin::State s);

}  // namespace web_core
