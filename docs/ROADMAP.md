# Roadmap

Done so far: Phase 1-2 (engine, gateway protections, tooling) and the core
of Phase 3 + half of Phase 5:

- ✅ **Lock-free SPSC ring** (`transport/spsc_queue.hpp`) with full
  memory-ordering documentation, unit + two-thread stress tests.
- ✅ **Single-threaded engine refactor**: per-session rings in, per-session
  response rings out, one engine thread owns all trading state. Engine
  processing 175ns p50 (was ~2.2µs under the mutex design).
- ✅ **Allocation discipline**: fixed fill lists (crossing workload got
  24% faster), generation-tagged dedupe window, fixed event ring, response
  buffers in ring slots — plus an allocation *counter* so claims stay
  honest (remaining: hash-map nodes per rest, ~2.5-5/order — the pool
  exercise below).
- ✅ **Market-data publisher** with sequence numbers, slow-consumer drop,
  SNAP resync, and a Python client that demonstrates gap recovery.
- ✅ **Market-making bot + noise-trader mode** (the strategy plane seed).

Each phase below lands 2-4 more of the 18 concepts as real code, keeps all
tests green, and ends with a measured before/after.

## Phase 3 — Transport: lock-free queue, event bus, persistence (concepts 9, 10, 6)

The SPSC rings and single-threaded engine are done (see header). What
remains:

1. **Async persistence worker**: fills and audit records flow from the
   engine on a second SPSC ring to a worker thread that batch-writes
   SQLite (WAL mode). Benchmark the write batch size vs latency trade-off.
2. **Order pool / open-addressed locator**: kill the remaining hash-map
   node allocations per resting order (~2.5-5/order visible in
   `hft_bench_gw`) — preallocated slots + free list, engine-thread-owned.
3. **Re-benchmark + publish**: target 0 allocations/order on the crossing
   workload and a README table row to prove it.

Exit criteria: order path allocation-free after parse; crash-replay test
restores book state from the audit log.

## Phase 4 — Scale & resilience: sharding, routing, replication (concepts 2, 7, 8, 14)

1. **Symbol sharding**: run K engine processes; a router process does
   consistent hashing on symbol → engine. Consistent hash ring with
   virtual nodes so adding a shard remaps ~1/N symbols.
2. **Replication**: each engine streams its ordered input log to a standby
   (deterministic replay = state recovery). Kill the primary mid-load in
   the demo; standby promotes; the monitor shows the gap.
3. **Health checks + failover**: router probes engines; mark-dead,
   re-route, alarm. This is also where load balancing stops being a
   metaphor and becomes a component.

Exit criteria: demo script kills -9 a shard mid-run and loses zero
accepted-but-unacknowledged orders (idempotency + replay cover the gap).

## Phase 5 — Observability & research plane (concepts 5, 11, 12, 13, 17, 18)

Partially done: the MD push stream with sequence numbers, slow-consumer
drop, and SNAP resync are live (concept 12, most of 17). Remaining:

1. **Trace IDs** end-to-end: gateway stamps one per order; it rides the
   rings, the engine, the persistence worker; a span log reconstructs
   per-stage latency (the proc/core/RTT ladder becomes per-order).
2. **Snapshot cache**: versioned book snapshots keyed by sequence number
   for gap-free re-subscribe (the general form of today's SNAP).
3. **Tick query + replay**: a Python `research/replay.py` that reads the
   SQLite audit store and replays a session against the MM bot; a tiny
   query CLI for "fills in window" (search over the store). The MM's
   random-walk fundamental gets replaced by replayed data.

## Parallel prep track (alongside, not after)

Weeks where you're not coding phases:

- **C++ for HFT internals**: memory model (seq_cst vs acq_rel vs relaxed),
  false sharing, allocators/arenas, move semantics, templates,
  `std::chrono`, benchmarking discipline (warmup, outliers, p99 vs mean).
  Map each topic to the file where the repo uses it — see INTERVIEW.md.
- **DSA**: LeetCode hard-ish timed practice — Citadel-style loops.
- **Probability**: green book + Jane Street puzzle archive; do them aloud.
- **OCaml basics** if Jane Street is on the target list (MOOC first
  chapters are enough to be dangerous in a screen).

## Career notes (from the research that shaped this repo)

- **Jane Street**: custom practical problems, puzzles, OCaml taught live,
  ~zero LeetCode. They optimize for reasoning + clean code under discussion.
- **Citadel/Citadel Securities**: LeetCode DSA + deep low-latency C++
  (lock-free, memory management, cache behavior) + behavioral "tell me
  about a latency/crash you fixed."
- **Tower/Jump/HRT**: C++-centric systems depth — memory model, stack vs
  heap latency, template metaprogramming, atomics.
- **Quant Researcher / Trader variants**: same systems repo is a strong
  signal for QR-infrastructure and trading-tech roles; pair it with the
  probability/statistics track and, for QR, a Python research notebook
  that consumes the Phase 5 replay data (signal on simulated fills).
