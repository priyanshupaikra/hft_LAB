#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "gateway/trading_core.hpp"

namespace hft {

// Market-data publisher: drains the engine's MdUpdate ring and fans the
// frames out to every connected subscriber over TCP.
//
// Why a separate thread and queue: the engine must never block on a
// subscriber. Updates flow engine -> SPSC ring -> this thread; if the ring
// fills (publisher behind), the ENGINE DROPS (counted in md_dropped) —
// the classic slow-consumer policy of every real feed: drop, never block
// the producer. Subscribers detect gaps via sequence numbers and resync
// with a SNAP through the gateway — the snapshot+delta recovery pattern.
//
// (Production feeds use UDP multicast / kernel bypass so one send reaches
// N consumers once; TCP fan-out per subscriber is the demo-grade stand-in
// and honestly acknowledged as such.)
class MdPublisher {
 public:
  MdPublisher(SpscQueue<MdUpdate, 4096>& in, std::uint16_t port)
      : in_(in), port_(port) {}

  // Returns false + sets err on socket failure.
  bool start(const char** err);
  void stop();
  std::size_t subscriber_count() const { return subscriber_count_.load(std::memory_order_relaxed); }

 private:
  void run();
  void broadcast(const char* frame, std::size_t len);
  void accept_new();

  SpscQueue<MdUpdate, 4096>& in_;
  std::uint16_t port_;
  int listen_fd_ = -1;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<std::size_t> subscriber_count_{0};

  std::mutex subs_mu_;       // subscriber list is control-plane state
  std::vector<int> subs_;
};

}  // namespace hft
