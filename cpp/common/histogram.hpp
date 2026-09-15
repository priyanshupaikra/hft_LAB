#pragma once

#include <atomic>
#include <bit>
#include <cstdint>

namespace hft {

// Log2-bucketed latency histogram (a coarse cousin of HdrHistogram).
//
// Why not store every sample and sort? Two properties that matter on a hot
// path: record() is O(1) with no allocation, and memory is fixed regardless
// of traffic (a server that logs "everything" eventually falls over from its
// own telemetry). Trade-off: percentiles are interpolated within a bucket, so
// they approximate to within ~2x at worst inside one bucket — fine for
// p50/p99 dashboarding. The engine benchmark uses exact vector+sort instead.
class LatencyHistogram {
 public:
  // Thread-safe: one atomic increment per record. Contention on 64 cache-line
  // buckets is negligible at demo scale; sharding per-thread would be the
  // next step.
  void record(std::int64_t value_ns) noexcept {
    const auto v = static_cast<std::uint64_t>(value_ns < 0 ? 0 : value_ns);
    const unsigned bucket = v == 0 ? 0 : static_cast<unsigned>(64 - std::countl_zero(v));
    buckets_[bucket < kBuckets ? bucket : kBuckets - 1].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    std::int64_t m = max_.load(std::memory_order_relaxed);
    while (value_ns > m && !max_.compare_exchange_weak(m, value_ns, std::memory_order_relaxed)) {}
  }

  std::int64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
  std::int64_t max() const noexcept { return max_.load(std::memory_order_relaxed); }

  // Approximate percentile in ns; q in [0,100]. Returns -1 if empty.
  std::int64_t percentile(double q) const noexcept {
    const std::int64_t total = count();
    if (total == 0) return -1;
    const double target = (q / 100.0) * static_cast<double>(total);
    double cumulative = 0;
    for (unsigned b = 0; b < kBuckets; ++b) {
      const double c = static_cast<double>(buckets_[b].load(std::memory_order_relaxed));
      if (cumulative + c >= target && c > 0) {
        // Linear interpolation inside [2^(b-1), 2^b).
        const double lo = b == 0 ? 0.0 : std::ldexp(1.0, static_cast<int>(b) - 1);
        const double hi = std::ldexp(1.0, static_cast<int>(b));
        const double frac = (target - cumulative) / c;  // position within bucket
        return static_cast<std::int64_t>(lo + frac * (hi - lo));
      }
      cumulative += c;
    }
    return max();
  }

 private:
  static constexpr unsigned kBuckets = 64;
  std::atomic<std::int64_t> buckets_[kBuckets]{};
  std::atomic<std::int64_t> count_{0};
  std::atomic<std::int64_t> max_{0};
};

}  // namespace hft
