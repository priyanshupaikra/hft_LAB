#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "common/histogram.hpp"
#include "common/token_bucket.hpp"
#include "common/types.hpp"
#include "engine/matching_engine.hpp"
#include "gateway/dedupe_window.hpp"
#include "gateway/event_ring.hpp"
#include "gateway/kill_switch.hpp"
#include "gateway/session.hpp"
#include "transport/spsc_queue.hpp"

namespace hft {

using SessionId = std::uint64_t;

// What a connection thread sends to the engine thread. Must stay trivially
// copyable: it rides a preallocated SPSC ring slot, never the heap.
struct Command {
  enum class Type : std::uint8_t {
    New, Cancel, Events, Stats, Kill, Resume, Disconnect, Snap
  };
  Type type;
  std::uint8_t slot;        // owning session slot
  SessionId sid;            // lease guard: stale producers are rejected
  std::int64_t received_ns;
  OrderRequest order;       // New
  ClientOrderId cl;         // Cancel
  SymbolId symbol;          // Snap
};
static_assert(std::is_trivially_copyable_v<Command>, "Command must ride the SPSC ring");

// What the engine thread sends back. `final` marks the last response for a
// command (EVENTS can span several).
struct Response {
  bool final;
  std::uint16_t len;
  char text[400];
};
static_assert(std::is_trivially_copyable_v<Response>, "Response must ride the SPSC ring");

inline constexpr int kMaxSessions = 64;

// Top-of-book delta handed to the MD publisher thread.
struct MdUpdate {
  std::uint32_t symbol;
  std::uint64_t seq;
  Price bid;
  Price ask;
  bool has_bid;
  bool has_ask;
};
static_assert(std::is_trivially_copyable_v<MdUpdate>);

// The gateway's brain, re-architected for a lock-free hot path (Phase 3).
//
//   connection threads                     engine thread
//   ─────────────────                      ────────────────────────────
//   parse line ──▶ Command ──SPSC──▶  check pipeline (kill → rate →
//   then block on              per-slot  dedupe → risk) → matching engine
//   response queue ◀──SPSC────         → Response(s)
//
// - Every session slot owns two SPSC rings (in/out). Engine thread polls
//   active slots round-robin; connection threads only touch their own
//   rings → the data path takes no locks, ever.
// - Locks appear exactly twice, both off the data path: (a) condition
//   variables for *sleeping* when idle (signaled only on empty→non-empty
//   transitions, so steady state never pays for them), (b) the immutable
//   auth table, written once at construction.
// - Session slot handoff is a single atomic ownership flag; see
//   release_slot() for why that is sufficient.
// - All mutable engine state (books, kill switch, counters, histograms,
//   rate buckets, dedupe windows) is engine-thread-owned — that's what
//   makes "single-threaded by construction" true instead of aspirational.
class TradingCore {
 public:
  struct Counters {
    std::uint64_t accepted = 0;
    std::uint64_t rejected_auth = 0;
    std::uint64_t rejected_kill = 0;
    std::uint64_t rejected_rate = 0;
    std::uint64_t rejected_dup = 0;
    std::uint64_t rejected_risk = 0;
    std::uint64_t cancels_ok = 0;
    std::uint64_t cancels_miss = 0;
    std::uint64_t fills = 0;
    std::uint64_t md_updates = 0;
    std::uint64_t md_dropped = 0;
  };

  explicit TradingCore(std::vector<TraderProfile> traders);
  ~TradingCore();
  TradingCore(const TradingCore&) = delete;
  TradingCore& operator=(const TradingCore&) = delete;

  // ---- connection-thread API (control plane + hot-path produce/consume) ----

  // Immutable after construction: safe to call from any thread, no lock.
  const TraderProfile* authenticate(const std::string& token) const;
  SessionId next_session_id();  // atomic

  // Lease a slot (ownership: the CAS on Slot::active). Returns -1 if all
  // slots are taken.
  int acquire_slot(const TraderProfile* profile, SessionId sid);

  // Push a command; blocks (yield-loop) only if the 1024-deep ring is full,
  // i.e. the engine is many milliseconds behind.
  void submit_command(int slot, const Command& cmd);

  // Pop the next response, parking on a condvar while none is ready.
  // Returns false only at shutdown.
  bool wait_response(int slot, Response& out);

  // Give the slot back: sends Disconnect, waits for the engine's BYE, then
  // clears the ownership flag. The BYE handshake is what makes the single
  // flag sufficient: when it completes, both rings are provably empty and
  // their counters consistent, so the next lease needs no resets.
  void release_slot(int slot, SessionId sid);

  // ---- engine-thread observable state for the MD publisher (Arc 2) ----
  // The publisher drains this ring from its own thread.
  SpscQueue<struct MdUpdate, 4096>& md_queue() { return md_out_; }

  void shutdown();

 private:
  struct Slot {
    std::atomic<bool> active{false};  // ownership token, nothing else
    const TraderProfile* profile = nullptr;  // written by owner pre-first-push
    SessionId sid = 0;
    SpscQueue<Command, 1024> to_engine;
    SpscQueue<Response, 256> to_client;
    std::mutex wait_mu;                     // client-side parking only
    std::condition_variable wait_cv;
  };

  // Engine-thread-owned session state. No synchronization needed: only the
  // engine thread ever reads or writes these.
  struct EngineSession {
    const TraderProfile* trader = nullptr;
    SessionId sid = 0;
    TokenBucket bucket{1.0, 1.0, 0};
    DedupeWindow dedupe;
    FixedEventRing events;
    std::unordered_map<ClientOrderId, EngineOrderId> live_orders;
    void reset(const TraderProfile* t, SessionId s, std::int64_t now) {
      trader = t;
      sid = s;
      bucket = TokenBucket(t ? t->rate_per_sec : 1.0, t ? t->burst : 1.0, now);
      dedupe.reset();
      events.clear();
      live_orders.clear();
    }
  };

  void engine_loop();
  bool drain_slot(int slot);
  void process(const Command& cmd);
  void send_response(int slot, const Response& resp);

  std::vector<TraderProfile> traders_;  // must outlive by_token_ pointers
  std::unordered_map<std::string, const TraderProfile*> by_token_;
  std::vector<std::unique_ptr<Slot>> slots_;  // stable addresses (atomics inside)
  std::vector<EngineSession> eng_;

  MatchingEngine engine_;
  KillSwitch kill_;
  Counters counters_;
  LatencyHistogram core_lat_;  // received_ns -> response built (queue+proc)
  LatencyHistogram proc_lat_;  // pipeline only

  std::unordered_map<ClientOrderId, std::uint8_t> resting_owner_;  // cl -> slot
  std::unordered_map<SymbolId, std::uint64_t> md_seq_;   // engine-owned seq
  std::unordered_map<SymbolId, OrderBook::TopOfBook> last_top_;
  SpscQueue<MdUpdate, 4096> md_out_;

  std::atomic<bool> running_{false};
  std::thread engine_thread_;
  std::mutex idle_mu_;  // engine idle parking only
  std::condition_variable idle_cv_;
  std::atomic<SessionId> next_sid_{1};
};

}  // namespace hft
