#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace hft {

// Identity + per-trader limits. A real shop pulls this from a config service
// / secret store; the demo embeds a fixed table so the gateway runs alone.
// AuthN = the token (who you are); AuthZ = the limits (what you may do).
struct TraderProfile {
  std::string name;
  std::string token;
  bool admin = false;              // may trip/resume the kill switch
  Qty max_order_qty = 0;           // per-order quantity cap (fat finger)
  std::int64_t max_notional = 0;   // qty*price cap, in cents (fat finger)
  double rate_per_sec = 0.0;       // order-rate throttle
  double burst = 0.0;
};

inline std::vector<TraderProfile> default_traders(double rate_per_sec, double burst) {
  return {
      {"admin", "dev-admin-token", /*admin=*/true, /*max_order_qty=*/1'000'000,
       /*max_notional=*/100'000'000'000LL, rate_per_sec * 10, burst * 10},
      {"alpha", "dev-alpha-token", false, /*max_order_qty=*/5'000,
       /*max_notional=*/10'000'000, rate_per_sec, burst},
      {"beta", "dev-beta-token", false, /*max_order_qty=*/1'000,
       /*max_notional=*/2'000'000, rate_per_sec / 4, burst / 4},
  };
}

}  // namespace hft
