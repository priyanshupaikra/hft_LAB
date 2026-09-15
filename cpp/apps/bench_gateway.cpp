// hft_bench_gw — benchmark the gateway's lock-free hot path in-process.
//
// K producer threads lease K session slots and hammer NEW commands through
// the SPSC rings (full round-trip: push -> engine thread -> response pop),
// exactly what a connection thread does minus the TCP syscalls. With work
// always pending the engine thread never parks, so this measures the true
// lock-free path. Two workloads:
//   crossing : alternating buy/sell at one price -> every order trades
//   resting  : one-sided orders away from the touch -> everything rests
//
// Also counts heap allocations during the run (counting_new.cpp): the
// crossing workload should be allocation-free end-to-end; resting still
// allocates hash-map nodes per order — that's the documented next step
// (order pool), visible here as a number instead of a vibe.
//
//   ./hft_bench_gw [--threads 4] [--n 250000] [--mode crossing|resting|both]
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "common/alloc_count.hpp"
#include "common/time.hpp"
#include "gateway/trading_core.hpp"

using namespace hft;

namespace {

struct Totals {
  std::vector<std::int64_t> rtts;
  std::uint64_t ops = 0;
};

void run_producers(TradingCore& core, int threads, long long n, bool resting_mode,
                   Totals& out) {
  const TraderProfile* alpha = core.authenticate("dev-alpha-token");
  std::vector<std::thread> producers;
  std::vector<std::vector<std::int64_t>> rtts(threads);

  for (int t = 0; t < threads; ++t) {
    producers.emplace_back([&, t] {
      const SessionId sid = core.next_session_id();
      const int slot = core.acquire_slot(alpha, sid);
      if (slot < 0) return;
      auto roundtrip = [&](Command cmd) {
        cmd.slot = static_cast<std::uint8_t>(slot);
        cmd.sid = sid;
        cmd.received_ns = now_ns();
        core.submit_command(slot, cmd);
        Response r;
        while (core.wait_response(slot, r)) {
          if (r.final) break;
        }
        return now_ns() - cmd.received_ns;
      };
      auto make_new = [&](long long i) {
        Command c{};
        c.type = Command::Type::New;
        c.order.cl_ord_id = static_cast<ClientOrderId>(t) * 1'000'000'000ULL + i;
        c.order.symbol = 1;
        c.order.side = resting_mode ? Side::Buy : (i % 2 == 0 ? Side::Buy : Side::Sell);
        c.order.qty = 10;
        c.order.price = resting_mode ? 9'000 - static_cast<Price>(i % 500)
                                     : 10'000;
        c.order.is_market = false;
        return c;
      };
      for (long long i = n + 1; i <= n + 2'000; ++i) roundtrip(make_new(i));  // warmup
      rtts[t].reserve(static_cast<std::size_t>(n));
      for (long long i = 1; i <= n; ++i) rtts[t].push_back(roundtrip(make_new(i)));
      core.release_slot(slot, sid);
    });
  }
  for (auto& p : producers) p.join();
  for (auto& v : rtts) {
    out.ops += v.size();
    out.rtts.insert(out.rtts.end(), v.begin(), v.end());
  }
}

std::int64_t pct(std::vector<std::int64_t>& v, double q) {
  if (v.empty()) return 0;
  const std::size_t i = std::min(v.size() - 1,
      static_cast<std::size_t>(q / 100.0 * static_cast<double>(v.size())));
  return v[i];
}

void report(const char* name, Totals& t, long long allocs, std::int64_t wall_ns) {
  std::sort(t.rtts.begin(), t.rtts.end());
  std::printf("%-9s n=%-9llu rtt p50=%-6lld p90=%-6lld p99=%-6lld p99.9=%-7lld "
              "max=%-9lld | %.2fM ops/s | %lld allocs (%.2f/order)\n",
              name, (unsigned long long)t.ops, pct(t.rtts, 50), pct(t.rtts, 90),
              pct(t.rtts, 99), pct(t.rtts, 99.9), t.rtts.empty() ? 0 : t.rtts.back(),
              static_cast<double>(t.ops) * 1e3 / static_cast<double>(wall_ns),
              allocs, static_cast<double>(allocs) / static_cast<double>(t.ops));
}

void run_mode(TradingCore& core, int threads, long long n, const char* name,
              bool resting_mode) {
  Totals t;
  const long long allocs_before = alloc_count().load(std::memory_order_relaxed);
  const std::int64_t t0 = now_ns();
  run_producers(core, threads, n, resting_mode, t);
  const std::int64_t wall_ns = now_ns() - t0;
  const long long allocs = alloc_count().load(std::memory_order_relaxed) - allocs_before;
  report(name, t, allocs, wall_ns);
}

}  // namespace

int main(int argc, char** argv) {
  int threads = 4;
  long long n = 250'000;
  const char* mode = "both";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--threads") == 0) threads = std::atoi(argv[i + 1]);
    else if (std::strcmp(argv[i], "--n") == 0) n = std::atoll(argv[i + 1]);
    else if (std::strcmp(argv[i], "--mode") == 0) mode = argv[i + 1];
  }

  // Effectively unlimited rate/burst: we are measuring the machinery here.
  TradingCore core(default_traders(1e9, 1e9));
  std::printf("hft_bench_gw: %d producer threads x %lld orders (round-trip through "
              "SPSC + engine thread)\n\n", threads, n);

  if (std::strcmp(mode, "both") == 0 || std::strcmp(mode, "crossing") == 0) {
    run_mode(core, threads, n, "crossing", /*resting_mode=*/false);
  }
  if (std::strcmp(mode, "both") == 0 || std::strcmp(mode, "resting") == 0) {
    run_mode(core, threads, n, "resting", /*resting_mode=*/true);
  }

  // Engine's own view: queue+processing vs processing-only percentiles.
  const TraderProfile* admin = core.authenticate("dev-admin-token");
  const SessionId sid = core.next_session_id();
  const int slot = core.acquire_slot(admin, sid);
  if (slot >= 0) {
    Command stats{};
    stats.type = Command::Type::Stats;
    stats.slot = static_cast<std::uint8_t>(slot);
    stats.sid = sid;
    stats.received_ns = now_ns();
    core.submit_command(slot, stats);
    Response r;
    while (core.wait_response(slot, r)) {
      std::printf("%.*s\n", (int)r.len, r.text);
      if (r.final) break;
    }
    core.release_slot(slot, sid);
  }
  return 0;
}
