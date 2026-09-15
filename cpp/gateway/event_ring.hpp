#pragma once

#include <cstdint>
#include <cstring>

namespace hft {

// Fixed-capacity ring of pre-formatted event records (maker-fill
// notifications). Single-owner (the engine thread writes, and also drains
// on EVENTS commands), so no atomics needed. When full, the oldest event is
// dropped — a slow client losing old notifications is the right failure
// mode (real feeds call this the slow-consumer problem and behave the
// same way: drop, don't block the producer).
struct EventRecord {
  char text[96];
  std::uint16_t len;
};

class FixedEventRing {
 public:
  void push(const char* s, std::uint16_t len) noexcept {
    if (len > sizeof(EventRecord::text)) len = sizeof(EventRecord::text);
    EventRecord& r = buf_[head_];
    std::memcpy(r.text, s, len);
    r.len = len;
    head_ = (head_ + 1) % kCap;
    if (count_ < kCap) ++count_;
  }

  bool pop(EventRecord& out) noexcept {
    if (count_ == 0) return false;
    const std::size_t tail = (head_ + kCap - count_) % kCap;
    out = buf_[tail];
    --count_;
    return true;
  }

  void clear() noexcept { head_ = 0; count_ = 0; }

 private:
  static constexpr std::size_t kCap = 1024;
  EventRecord buf_[kCap]{};
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

}  // namespace hft
