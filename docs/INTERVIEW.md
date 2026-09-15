# Interview talk track

How to present hft-lab in an HFT software-engineer interview: one 90-second
story per concept, anchored to code that actually exists. Interviewers at
Citadel/Tower/HRT/Jump probe C++ internals and systems reasoning; Jane
Street probes practical programming and clear thinking. This repo gives you
concrete, defensible answers for both styles.

## The opening story (30 seconds)

> "I built a small exchange and gateway stack in C++20: a price-time
> priority matching engine that sustains ~8-12M orders a second
> single-threaded, fronted by a gateway that does auth, token-bucket rate
> limiting, ClOrdID dedupe, and pre-trade risk with a kill switch that
> auto-trips on fat-finger bursts. Python tooling drives load at it and
> monitors it. Everything is measured — there's a benchmark binary and
> latency histograms in the STATS endpoint."

## Per-concept 90-second answers

**Rate limiting** — "Token bucket per session (`cpp/common/token_bucket.hpp`):
O(1) memory, allows bursts, refills at the limit rate. I inject 2000 orders/s
against a 400/s limit and the demo shows exactly the excess rejected. Real
firms: exchanges throttle order rates per session; firms self-throttle so a
runaway strategy gets shelved before the exchange bans them. Alternative
designs I can compare: sliding-window log (exact but O(memory)), fixed
window (boundary double-spend problem), leaky bucket (smooths, no bursts)."

**API gateway / AuthN / AuthZ** — "My gateway is the classic pattern:
protocol termination, authentication (token → trader identity), authorization
(the identity carries limits — max qty, max notional, admin bit), and
request shaping, all in front of the 'backend' (engine). In trading the
analogue is the FIX session gateway doing logon and permissioning. I keep
the check order cheapest-reject-first: kill switch → rate → dedupe → risk."

**Caching** — "Not built yet, but the design is scoped (ROADMAP Phase 5):
book snapshots cached with a sequence number so a consumer either gets a
consistent snapshot or a 'stale, retry' marker. Real use: instrument
reference data is immutable-ish and read on every order — the classic
cache-me-hard data."

**Database / async workers** — "The engine's hot path never touches a
database — that's the whole design. Fills go onto a queue and a worker
persists them later (Phase 3). Real firms: tick stores like kdb+ written
asynchronously; the audit trail is append-only. I can discuss WAL semantics
and why you never fsync on the order path."

**Replication / sharding** — "Symbols never interact, so the book shards by
instrument with zero coordination — I route per-symbol inside the engine
today, and Phase 4 makes it N processes behind a consistent-hash router.
Consistent hashing means adding a shard only remaps ~1/N of keys.
Replication: engines are deterministic given the same ordered inputs, so a
standby replays the input stream — that's also how you recover state after
a crash (event sourcing)."

**Message queue** — "Today it's a mutex — deliberately naive so I can show
the upgrade. Phase 3 is a single-producer/single-consumer ring buffer:
head/tail atomics with acquire/release ordering, cache-line-padded so the
producer's writes don't invalidate the consumer's cache line (false
sharing). That's exactly what Aeron/DPDK-style transport does, minus kernel
bypass."

**Monitoring / tracing** — "A log2-bucketed histogram: O(1) record, fixed
memory, interpolated percentiles — a coarse HdrHistogram. Server-side core
latency is ~2µs p50 while client RTT is ~130µs, which tells you syscalls
and the network dominate — that observation drives the Phase 3 redesign.
Distributed tracing in trading = latency attribution per stage of an
order's life; I carry the measurement points, Phase 5 adds a trace ID."

**Circuit breaker / kill switch** — "The most literal mapping: trading's
circuit breaker predates the pattern's use in web services. Mine is a state
machine — CLOSED (count risk rejects in a window), OPEN (all orders
rejected), HALF_OPEN (probation; one violation re-trips with doubled
cooldown). Admin can force it either way, which is the human 'kill the
strategy' button. The demo trips it with a burst of fat fingers and you
watch accepted orders go to zero."

**Idempotency** — "FIX invented this: ClOrdID is the idempotency key, and
duplicates are flagged (PossDup) rather than double-executed. My gateway
keeps a bounded LRU of seen ClOrdIDs per session and answers `ERR
DUPLICATE`. The subtle part is bounding memory — an unbounded 'seen' set is
a memory leak wearing a mustache."

**Webhooks vs polling** — "Both shapes are in the repo on purpose: taker
fills are pushed inline in the ACK; maker fills are queued and delivered
when the client sends EVENTS (poll). Push = low latency but needs the
consumer to be listening; poll = simpler, survivable, latency-bounded by
the interval. Market data is push (multicast) because milliseconds are
money; snapshots are pull because you only need them on startup/gap."

## C++ questions this codebase prepares you for

- **Why `std::list` for level FIFO?** Iterators survive neighbor erasure →
  O(1) cancel via a locator map. Vector would amortize better on scan but
  pay O(n) mid-insert and invalidate iterators on erase.
- **Why `std::map` and not `unordered_map` for levels?** Levels need order
  (best price = begin()); hashing can't give you "next-worse price" for
  sweeps. Trade: O(log P) vs O(1); P (distinct price levels) is small.
- **Fixed-point vs floating** — see ARCHITECTURE.md; also: which
  comparisons are exact, and why `double` money is a lint error in any
  serious shop.
- **Memory ordering** (Phase 3 lands the code, know the theory now):
  acquire/release on the ring's head/tail; why relaxed is enough for
  counters; what a data race is (UB, not "usually fine").
- **False sharing** — two atomics on one cache line ping-pong between
  cores; pad to 64 bytes. My histogram's buckets are contended only at
  demo scale and I say so honestly.
- **`steady_clock` vs `system_clock`** — latency needs monotonic; wall
  clock jumps on NTP.
- **The bug I shipped and caught**: storing pointers into a by-value
  constructor parameter (dangling TraderProfiles) — the smoke test read
  garbage limits. Great answer for "tell me about a bug you found."

## Prep track beyond this repo

- **Probability brainteasers** — every quant-firm loop asks them in every
  role: expected value, conditional probability, martingale-flavored
  games, mental math. Green book (Zhou, *A Practical Guide to Quantitative
  Finance Interviews*) + Jane Street puzzles.
- **Jane Street divergence** — OCaml and practical exercises, no LeetCode;
  practice thinking aloud and writing clean code in a functional style.
- **Algorithmic depth** — Citadel asks LeetCode-hard DSA in addition to
  systems; keep a parallel DSA track.

## Sources (interview-process research)

- [Interviewing.io — Jane Street interview questions](https://interviewing.io/jane-street-interview-questions)
- [InterviewQuery — Jane Street guide](https://www.interviewquery.com/interview-guides/jane-street)
- [TryExponent — Jane Street SWE interview experience](https://www.tryexponent.com/experiences/jane-street-senior-software-engineer-interview-d9cb42)
- [InterviewCoder — Citadel SWE: 41 questions](https://www.interviewcoder.co/blog/citadel-software-engineer)
- [DataFord — Tower Research SWE guide](https://dataford.io/interview-guides/tower-research-capital/software-engineer)
- [20 real C++ quant interview questions (HRT/Jump/Citadel/Tower)](https://www.quantt.co.uk/resources/cpp-quant-interview-questions)
- [r/cpp — How to prepare for HFT/low-latency interviews](https://www.reddit.com/r/cpp/comments/1rcdrts/how_to_prepare_for_a_hftlow_latency_programming/)
- [QuantStart — How to get a job at an HFT firm](https://www.quantstart.com/articles/How-to-Get-a-Job-at-a-High-Frequency-Trading-Firm/)
