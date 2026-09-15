#pragma once

#include <cstdint>

namespace hft {

// Token-bucket rate limiter: refill `rate` tokens/sec up to `burst`.
// Cheaper than a sliding-window log (O(1) memory) and lets legitimate
// clients burst, which is exactly what we want for order flow — exchanges
// do the same thing with order-rate throttles.
//
// Not thread-safe by design: the gateway serializes access behind its core
// lock (control-plane object, hot path never touches it concurrently).
class TokenBucket {
 public:
  TokenBucket(double rate_per_sec, double burst, std::int64_t now_ns)
      : rate_per_sec_(rate_per_sec),
        burst_(burst),
        tokens_(burst),
        last_refill_ns_(now_ns) {}

  // Returns true and consumes one token, or returns false (empty) — a
  // rejected request consumes nothing.
  bool try_consume(std::int64_t now_ns, double tokens = 1.0) noexcept {
    refill(now_ns);
    if (tokens_ < tokens) return false;
    tokens_ -= tokens;
    return true;
  }

  double available(std::int64_t now_ns) noexcept {
    refill(now_ns);
    return tokens_;
  }

  double rate() const noexcept { return rate_per_sec_; }
  double burst() const noexcept { return burst_; }

 private:
  void refill(std::int64_t now_ns) noexcept {
    if (now_ns <= last_refill_ns_) return;
    const double elapsed_s =
        static_cast<double>(now_ns - last_refill_ns_) * 1e-9;
    tokens_ = tokens_ + elapsed_s * rate_per_sec_;
    if (tokens_ > burst_) tokens_ = burst_;
    last_refill_ns_ = now_ns;
  }

  double rate_per_sec_;
  double burst_;
  double tokens_;
  std::int64_t last_refill_ns_;
};

}  // namespace hft
