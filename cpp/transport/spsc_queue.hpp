#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hft {

// Bounded single-producer/single-consumer ring buffer (Vyukov-style).
//
// The memory-ordering contract is the whole point, so it's worth spelling
// out. Two counters, each written by exactly one side:
//   tail_ — producer-owned index of the next slot to write
//   head_ — consumer-owned index of the next slot to read
// Data path never touches a lock:
//   push: load tail relaxed (only we write it); if space might be short,
//         re-load head ACQUIRE (to see consumer's progress + the data it
//         finished reading); write slot; store tail RELEASE (publish the
//         slot's contents before making the index visible).
//   pop:  mirror image — acquire on tail to see the producer's data,
//         release on head to signal the slot is free.
// Each side also caches the other's counter (cached_head_/cached_tail_) and
// only re-reads it when the queue looks full/empty — that keeps the shared
// cache-line traffic to roughly one ping per queue-depth, not per op.
//
// The two counters are padded onto separate cache lines so the producer's
// writes to tail_ don't invalidate the consumer's cached copy of that line
// (false sharing) — this padding alone is often a 2-5x difference.
//
// T must be trivially copyable: slots are a plain array, no constructors.
template <typename T, std::size_t Capacity>
class SpscQueue {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "SPSC slots must be trivially copyable");

 public:
  bool push(const T& v) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - cached_head_ >= Capacity) {  // might be full: refresh consumer view
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t - cached_head_ >= Capacity) return false;
    }
    slots_[t & kMask] = v;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& out) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {  // might be empty: refresh producer view
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h == cached_tail_) return false;
    }
    out = slots_[h & kMask];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // Approximate (racy) size — fine for telemetry, never for logic.
  std::size_t size() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }
  bool empty() const noexcept { return size() == 0; }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  alignas(64) std::atomic<std::size_t> tail_{0};  // producer line
  std::size_t cached_head_ = 0;                   // producer's view of head
  alignas(64) std::atomic<std::size_t> head_{0};  // consumer line
  std::size_t cached_tail_ = 0;                   // consumer's view of tail
  alignas(64) T slots_[Capacity]{};               // data lines
};

}  // namespace hft
