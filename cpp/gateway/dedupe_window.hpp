#pragma once

#include <cstddef>
#include <cstdint>

#include "common/types.hpp"

namespace hft {

// Fixed-capacity idempotency window: "have I seen this ClOrdID recently?"
// with zero allocations and O(1) amortized cost.
//
// Structure: open-addressing hash table (linear probing) where every slot
// is generation-tagged. "Clearing" the table = bumping the generation,
// which makes every slot stale in O(1) — no per-entry deletion, which is
// the part that makes open addressing awkward otherwise.
//
// Window semantics (deliberately approximate, like every real system's):
// a ClOrdID is remembered while its slot survives TWO generation bumps
// (~1.5x table capacity of later inserts). Retries arrive milliseconds
// after the original, so a window of thousands of recent IDs is far beyond
// any sane retry behavior — bounded memory beats perfect recall here.
class DedupeWindow {
 public:
  // Returns true if `cl` was newly remembered; false if it is a duplicate.
  bool seen_and_insert(ClientOrderId cl) noexcept {
    if (cl == 0) return false;  // 0 is the empty-slot marker, never valid
    const std::size_t mask = kCap - 1;
    std::size_t i = hash(cl) & mask;
    for (int probes = 0; probes < 64; ++probes, i = (i + 1) & mask) {
      // A slot remembers its key across two generations (current + previous).
      if (keys_[i] == cl &&
          (gen_[i] == cur_gen_ || gen_[i] == prev_gen_)) {
        return false;  // duplicate within the retry window
      }
      if (gen_[i] != cur_gen_) {  // stale or never-used: claim it
        gen_[i] = cur_gen_;
        keys_[i] = cl;
        if (++inserted_ >= kRotate) rotate();
        return true;
      }
      // live slot holding a different key: keep probing
    }
    rotate();  // pathological clustering: nuke the window, accept the order
    return true;
  }

  void reset() noexcept {
    cur_gen_ += 2;  // stale-ify everything, both generations
    prev_gen_ = cur_gen_ - 1;
    inserted_ = 0;
  }

 private:
  static constexpr std::size_t kCap = 8192;        // power of two
  static constexpr std::size_t kRotate = kCap / 2; // load factor 50%

  void rotate() noexcept {  // current generation becomes "previous"
    prev_gen_ = cur_gen_;
    ++cur_gen_;
    inserted_ = 0;
  }

  static std::size_t hash(ClientOrderId x) noexcept {  // splitmix64 finalizer
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return static_cast<std::size_t>(x);
  }

  ClientOrderId keys_[kCap] = {};
  std::uint32_t gen_[kCap] = {};    // 0 = never used; tag per generation
  std::uint32_t cur_gen_ = 1;
  std::uint32_t prev_gen_ = 0;
  std::size_t inserted_ = 0;
};

}  // namespace hft
