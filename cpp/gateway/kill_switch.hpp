#pragma once

#include <cstdint>
#include <deque>

namespace hft {

// Circuit breaker guarding order flow — in trading this is literally called
// the kill switch, and regulators/exchanges require one. Classic state
// machine (think nginx/Resilience4j breaker, but the trading origin is
// older):
//
//   CLOSED   -> normal trading. Risk rejections are counted in a window;
//              `risk_rejects_to_trip` inside `window_s` trips the breaker.
//   OPEN     -> all orders rejected (ERR KILL_SWITCH). Lasts `cooldown_s`.
//   HALF_OPEN-> after the cooldown, a probation period: the first risk
//              rejection re-trips immediately (with doubled cooldown);
//              `successes_to_close` clean orders close the breaker.
//
// An admin can force OPEN/CLOSED at any time (the classic human "kill the
// strategy" button). Time is injected as a parameter so the state machine is
// unit-testable without sleeping.
class KillSwitch {
 public:
  enum class State : std::uint8_t { Closed, Open, HalfOpen };

  struct Config {
    int risk_rejects_to_trip = 8;   // risk rejects inside the window that trip
    std::int64_t window_ns = 5'000'000'000;   // 5s rolling window
    std::int64_t cooldown_ns = 10'000'000'000; // 10s open duration
    int successes_to_close = 50;    // clean orders in half-open to close
  };

  KillSwitch() : KillSwitch(Config{}) {}
  explicit KillSwitch(Config cfg) : cfg_(cfg) {}

  // May a new order through? Also advances the state machine if the
  // cooldown elapsed.
  bool allows_order(std::int64_t now_ns) {
    if (state_ == State::Open && now_ns - opened_at_ns_ >= cooldown_ns_) {
      state_ = State::HalfOpen;
      half_open_successes_ = 0;
    }
    return state_ != State::Open;
  }

  // Call when an order passes all checks.
  void record_success(std::int64_t /*now_ns*/) {
    if (state_ == State::HalfOpen && ++half_open_successes_ >= cfg_.successes_to_close) {
      state_ = State::Closed;
      window_rejects_.clear();
    }
  }

  // Call when the risk module rejects an order (fat finger, limits).
  void record_risk_reject(std::int64_t now_ns) {
    if (state_ == State::HalfOpen) {
      trip(now_ns);  // re-offending during probation re-trips instantly
      return;
    }
    if (state_ != State::Closed) return;

    // Drop samples older than the window, then count.
    while (!window_rejects_.empty() &&
           now_ns - window_rejects_.front() > cfg_.window_ns) {
      window_rejects_.pop_front();
    }
    window_rejects_.push_back(now_ns);
    if (static_cast<int>(window_rejects_.size()) >= cfg_.risk_rejects_to_trip) {
      trip(now_ns);
    }
  }

  void trip(std::int64_t now_ns) {  // force OPEN (admin kill or auto-trip)
    const bool re_trip = state_ != State::Closed;
    state_ = State::Open;
    opened_at_ns_ = now_ns;
    // Exponential backoff only when the breaker re-trips after probation —
    // a first offense serves the base cooldown.
    cooldown_ns_ = re_trip ? cooldown_ns_ * 2 : cfg_.cooldown_ns;
    window_rejects_.clear();
  }

  void reset() {  // force CLOSED (admin resume)
    state_ = State::Closed;
    window_rejects_.clear();
    cooldown_ns_ = cfg_.cooldown_ns;
  }

  State state() const noexcept { return state_; }
  static const char* to_str(State s) {
    switch (s) {
      case State::Closed: return "CLOSED";
      case State::Open: return "OPEN";
      case State::HalfOpen: return "HALF_OPEN";
    }
    return "?";
  }

 private:
  Config cfg_;
  State state_ = State::Closed;
  std::int64_t opened_at_ns_ = 0;
  std::int64_t cooldown_ns_ = cfg_.cooldown_ns;
  int half_open_successes_ = 0;
  std::deque<std::int64_t> window_rejects_;
};

}  // namespace hft
