#include "setup_join.h"

namespace web_core {

void SetupJoin::start(uint32_t now) {
  state_ = State::Joining;
  started_ = now;
  reason_ = 0;
}

void SetupJoin::disconnected(uint8_t reason) {
  if (state_ == State::Joining) reason_ = reason;
}

bool SetupJoin::tick(uint32_t now, bool connected) {
  if (state_ != State::Joining) return false;
  if (connected) {
    state_ = State::Online;
    online_at_ = now;
    return false;
  }
  if (now - started_ <= kGiveUpMs) return false;
  // 15 4WAY_HANDSHAKE_TIMEOUT, 202 AUTH_FAIL, 204 HANDSHAKE_TIMEOUT: the
  // password; 201 NO_AP_FOUND: the network isn't in range.
  state_ = reason_ == 15 || reason_ == 202 || reason_ == 204 ? State::WrongPassword
           : reason_ == 201                                   ? State::NotFound
                                                              : State::Failed;
  return true;
}

bool SetupJoin::showing_online(uint32_t now) const {
  return state_ == State::Online && now - online_at_ <= kShowOnlineMs;
}

const char *setup_join_name(SetupJoin::State s) {
  switch (s) {
    case SetupJoin::State::Joining: return "joining";
    case SetupJoin::State::Online: return "online";
    case SetupJoin::State::WrongPassword: return "wrong_password";
    case SetupJoin::State::NotFound: return "not_found";
    case SetupJoin::State::Failed: return "failed";
    default: return "idle";
  }
}

}  // namespace web_core
