# Architecture

## The system today (Phase 3 lock-free hot path + market data)

```
                    Python (research/ops plane)
  ┌────────────────┐  ┌────────────────┐  ┌─────────────────────────┐
  │ py/injector    │  │ py/monitor     │  │ py/mm (market maker)    │
  │ paced flow,    │  │ polls STATS    │  │ quotes, skews on        │
  │ dups, fat fin- │  │ (polling       │  │ inventory, polls EVENTS │
  │ gers, noise    │  │ dashboard)     │  │ for maker fills         │
  └───────┬────────┘  └───────┬────────┘  └───────────┬─────────────┘
          │ TCP               │ TCP                   │ TCP
          ▼                   ▼                       ▼
┌──────────────────────────────────────────────────────────────────────┐
│ hft_gateway (C++20)                                                  │
│                                                                      │
│  server.cpp: accept loop, thread per connection, TCP_NODELAY.        │
│  Connection threads ONLY parse lines and shuttle Commands/Responses  │
│  through per-session SPSC rings:                                     │
│                                                                      │
│    conn thread ──Command──▶ [SPSC per slot] ──▶ ENGINE THREAD        │
│    conn thread ◀─Response── [SPSC per slot] ◀── (single, lock-free)  │
│                                                                      │
│  Engine thread (owns ALL trading state; no locks on its path):       │
│    kill switch → token bucket → dedupe window → risk → engine        │
│    MatchingEngine ── per-symbol price-time-priority OrderBooks       │
│    top-of-book deltas ──▶ [SPSC md ring]                             │
│                                                                      │
│  MD publisher thread: drains md ring, fans frames out to TCP         │
│  subscribers with seq numbers; slow consumers dropped, resync via    │
│  SNAP. Telemetry: two histograms (proc_lat, core_lat) in STATS.      │
│  Slot handoff: atomic ownership flag + Disconnect/BYE handshake.     │
└──────────────────────────────────────────────────────────────────────┘
          │
          ▼ MD stream (seq-numbered top-of-book deltas)
  ┌──────────────────┐
  │ py/md_client     │  gap detection -> SNAP resync
  └──────────────────┘
```

Check ordering is deliberate — cheapest reject first. You never spend
engine cycles on an order you're going to refuse anyway.

## The 18 system-design concepts → where they live here → where they live in a real firm

| # | Concept | In hft-lab today | In a real HFT firm | Status |
|---|---------|------------------|--------------------|--------|
| 1 | Rate limiting | Per-session token bucket in the engine thread; demo trips it at 2x limit | Exchange order-rate throttles; firms self-throttle outbound flow | ✅ built |
| 2 | Load balancer | — | Order flow spread across gateways | Phase 4 (consistent-hash router by symbol) |
| 3 | API gateway | `server.cpp` thin shell fronting the core | FIX/session gateways in front of matching engines | ✅ built |
| 4 | AuthN/AuthZ | Token → profile (who) + limits (what) in `session.hpp` | FIX logon, session keys, permissioning | ✅ built |
| 5 | Caching | SNAP returns current top; MD ring is a small delta cache | Instrument reference data, book snapshots | ◐ partial (full snapshot cache in P5) |
| 6 | Database | — | Tick stores / audit ledgers (kdb+, Chronicle) | Phase 3 (async trade store → SQLite) |
| 7 | Replication | — | Redundant A/B feeds, hot-standby engines | Phase 4 |
| 8 | Sharding | Per-symbol books inside one engine (hash routing) | Partitioning by instrument across cores/machines | ◐ partial (N engines in P4) |
| 9 | Message queue | SPSC rings per session + MD ring (`transport/spsc_queue.hpp`) | Aeron / Chronicle / shared-memory buses | ✅ built |
| 10 | Async workers | MD publisher thread drains its own ring | Post-trade processing, research jobs | ◐ partial (persistence worker in P3) |
| 11 | Search | — | Querying tick archives | Phase 5 |
| 12 | CDN | MD publisher fans one update to N subscribers | Market-data fan-out (multicast) to many strategies | ✅ built (TCP fan-out; multicast noted as the real thing) |
| 13 | Monitoring/logging | Two histograms (proc/core) + STATS + injector RTT | Latency histograms per stage, TCA | ✅ built |
| 14 | Health checks | STATS/PING; disconnect cancels that session's orders | Gateway liveness, failover probes | ✅ basic (auto-failover P4) |
| 15 | Circuit breaker | Kill switch: auto-trip on risk bursts, cooldown, half-open, admin override | The risk kill switch — regulator/exchange mandated | ✅ built |
| 16 | Idempotency | ClOrdID dedupe via generation-tagged open-addressing window | FIX ClOrdID/PossDup | ✅ built |
| 17 | Webhooks vs polling | MD stream + taker ACKs are push; EVENTS/SNAP/STATS are pull | Market data push (multicast); snapshots pull | ✅ both shapes |
| 18 | Distributed tracing | proc vs core vs RTT measurement points per order | Order-lifecycle latency attribution | ◐ partial (trace ID in P5) |

## Key design decisions worth defending

- **Fixed-point prices** (`int64` cents) — floats on an order path are a
  bug factory: 0.1 has no exact binary representation, and equal prices
  must compare equal.
- **`std::map` price levels + `std::list` FIFO + locator map** — best price
  is `begin()`, time priority is append order, and cancel is O(1) because
  list iterators survive neighboring erasure. A vector-of-levels would be
  faster to scan but O(n) to insert a level in the middle.
- **One engine thread behind SPSC rings** — all trading state is
  single-threaded by construction, so the check pipeline and the books need
  zero synchronization. Producers (connection threads) never touch engine
  state; they only push trivially-copyable Commands. Measured: 175ns p50
  engine processing, no locks. The cost: a queue hop (~4µs core latency at
  low load incl. condvar wake) — the spin-vs-park trade-off is visible in
  STATS as `core_lat` vs `proc_lat`.
- **Bounded-memory everything** — histogram (log2 buckets), dedupe window
  (generation-tagged table), event ring, fill lists (fixed array). A server
  that buffers "all" samples eventually dies of its own telemetry.
- **Slot handoff with one atomic flag** — sessions are preallocated slots;
  release does a Disconnect→BYE handshake so both rings are provably empty
  before the flag clears, and the next lease needs no counter resets.
- **Market orders are IOC** — a resting market order is an unbounded
  liability; real venues treat them the same way.
- **Kill switch before rate limiter** — risk gates everything; a hard stop
  must not be queueable behind per-session niceties.
- **Slow consumers are dropped, never obeyed** — the MD ring overflows on
  a stalled publisher (counted, `md_dropped`), and stalled subscribers get
  disconnected; both resync via SNAP. Backpressure by subtraction.

## What breaks at scale (and how the roadmap fixes it)

1. One engine thread is one core's worth of throughput (~0.4-1M
   round-trip ops/s in-process). Fix: shard by symbol across engine
   processes behind a consistent-hash router (Phase 4).
2. Everything lives and dies with one process. Fix: replication to a
   standby replaying the ordered command log + health-check failover
   (Phase 4).
3. Still allocates ~2.5-5 hash-map nodes per resting order. Fix: order
   pool / open-addressed locator (the alloc counter in `hft_bench_gw`
   turns this from a vibe into a number).
4. Text protocol and syscalls dominate RTT (86µs vs 175ns of engine).
   Fix: binary framing (exercise), then kernel bypass (out of scope,
   documented).
