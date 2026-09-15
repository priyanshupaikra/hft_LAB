#pragma once

#include <chrono>
#include <cstdint>

namespace hft {

// Monotonic clock in nanoseconds. steady_clock is the right clock for latency
// measurement: it never jumps when NTP adjusts the wall clock.
inline std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline double now_seconds(std::int64_t t_ns) { return static_cast<double>(t_ns) * 1e-9; }

}  // namespace hft
