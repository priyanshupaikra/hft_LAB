#pragma once

#include <atomic>

namespace hft {

// Global allocation counter for the benchmark binaries: link
// counting_new.cpp into a binary and every heap allocation bumps the
// counter, so a benchmark can prove (or bust) "zero allocations on the hot
// path". The atomic is relaxed — we're counting, not synchronizing.
inline std::atomic<long long>& alloc_count() {
  static std::atomic<long long> count{0};
  return count;
}

}  // namespace hft
