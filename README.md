# hft-lab

A miniature exchange + trading-firm stack built to learn, demonstrate, and
talk about system design the way HFT companies (Jane Street, Citadel, Tower
Research, HRT, Jump, ...) actually use it. **C++20 for the venue, Python for
the tooling** — same split those firms use between trading infrastructure
and research/ops tooling.

What's inside (all working, all measured):

- **Limit order book + matching engine** — price-time priority, IOC market
  orders, O(1) cancels, fixed-capacity fill lists (allocation-free match
  path). **8.7-11.6M orders/sec single-threaded.**
- **Lock-free gateway hot path** — per-session SPSC ring buffers feed one
  single-threaded engine (Vyukov queue: acquire/release head/tail, padded
  cache lines). Engine processing is **175ns p50**; no locks on the order
  path. `hft_bench_gw` proves it — including counting heap allocations.
- **Full safety pipeline** — auth, per-session token-bucket rate limiting,
  ClOrdID idempotency (allocation-free generation-tagged dedupe window),
  fat-finger risk checks, and a kill switch (circuit breaker) that
  auto-trips on risk-reject bursts.
- **Market-data feed** — top-of-book deltas with per-symbol sequence
  numbers fanned out to TCP subscribers; slow consumers get dropped (never
  block the engine) and recover via `SNAP` — the classic snapshot+delta
  resync. `py/md_client.py` demonstrates gap detection live.
- **Market-making bot** — `py/mm/mm.py` quotes two sides around a drifting
  fundamental, skews on inventory, polls `EVENTS` for fills, and tracks
  PnL. Noise-trader mode (`injector --noise`) supplies the flow.
- **Python tooling** — paced load injector (dups + fat fingers on purpose),
  polling monitor dashboard, MD client, MM bot.

## Quickstart

```bash
make                    # builds build/hft_{gateway,bench,bench_gw,tests}
make test               # 19/19 unit tests
make bench              # matching-engine micro-benchmark
./build/hft_bench_gw    # lock-free hot path round-trip + alloc counting
./scripts/demo.sh       # live demo: rate limiter + kill switch trips
./scripts/mm_demo.sh    # live demo: market maker earning the spread
```

Manual poke at the protocol:

```bash
./build/hft_gateway --port 5555 --rate 400 --burst 800 &
nc 127.0.0.1 5555
AUTH dev-alpha-token
NEW 1 B 10 9950        # buy 10 @ $99.50 (prices are cents; 0 => market)
NEW 2 S 10 10050       # rests on the ask
NEW 3 B 5 10050        # crosses -> FILLED
NEW 3 B 5 10060        # duplicate ClOrdID -> ERR DUPLICATE
NEW 4 B 100 9999999    # fat finger -> ERR RISK_NOTIONAL
EVENTS                 # maker-side fill notifications (polling)
SNAP 1                 # top-of-book + market-data sequence number
STATS                  # counters + latency percentiles
QUIT
```

Tokens: `dev-admin-token` (can `KILL`/`RESUME`), `dev-alpha-token`,
`dev-beta-token` (tighter limits). Market data streams on `--md-port`
(default 7600): `python3 py/md_client.py`.

## Measured on this machine (Apple clang 21, arm64, -O2)

```
hft_bench (engine alone, single thread, 300k orders/scenario):
  rest-heavy  p50=83ns  -> 8.66M ops/s   (book grows to 300k resting)
  crossing    p50=83ns  -> 11.58M ops/s  (232k fills; +24% after removing
                                          per-order fill-vector allocs)
  churn       p50=83ns  -> 11.61M ops/s  (submit+cancel)

hft_bench_gw (lock-free hot path, 4 producer threads round-trip):
  crossing   0.42M ops/s aggregate, rtt p50=7.4µs p99=21.9µs
  engine-side processing: p50=175ns p99=1.0µs   (proc_* in STATS)
  queue+processing (core_*): p50=4.2µs p99=14.4µs
  allocations: crossing 2.58/order, resting 5.04/order  <- hash-map nodes;
  fill vectors: 0. The counter exists so the claim is checkable.

demo.sh (end-to-end over TCP):
  client RTT p50=86µs p99=135µs under a 2000/s flood
  (mutex-era baseline for the same demo: ~131µs p50 / ~201µs p99)
```

The three-layer ladder — 175ns engine, 4µs core (queue hop), 86µs RTT
(syscalls) — is the honest anatomy of where latency lives, and the argument
for kernel bypass as the next frontier. Note the idle-parked engine adds a
condvar wake (~µs) for sporadic single commands: spin-vs-park is a real
trade-off, visible in STATS as core_lat vs proc_lat.

## Layout

```
cpp/common     types (fixed-point), latency histogram, token bucket,
               alloc counter
cpp/transport  spsc_queue: Vyukov lock-free ring (memory-ordering docs)
cpp/engine     order_book (price-time priority, fixed fill lists),
               matching_engine (per-symbol books, stats)
cpp/gateway    session profiles, kill switch, dedupe window, event ring,
               trading_core (slot-based async engine thread),
               server (thin TCP shell), md_publisher (fan-out + slow-
               consumer drop)
cpp/apps       hft_gateway, hft_bench, hft_bench_gw
cpp/tests      19 unit tests (book, engine, bucket, kill switch, SPSC,
               dedupe window, async core incl. slot reuse)
py/injector    paced order-flow generator + noise-trader mode
py/monitor     polling STATS dashboard; one-shot KILL/RESUME
py/md_client   market-data stream consumer with SNAP gap recovery
py/mm          toy market maker (quotes, inventory skew, PnL)
scripts/       demo.sh (protections trip), mm_demo.sh (MM earns spread)
docs/          ARCHITECTURE.md, INTERVIEW.md, ROADMAP.md
learning.md    component deep dives + DS-to-interview-problem mapping
```

Full concept mapping: **docs/ARCHITECTURE.md**. Interview talk track per
concept: **docs/INTERVIEW.md**. What to build next: **docs/ROADMAP.md**.
Deep-dive explanation of every component, the order flow, and the data
structures (with the classic problems they map to): **learning.md**.
