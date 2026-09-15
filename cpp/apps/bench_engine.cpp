// hft_bench — matching-engine micro-benchmark.
//
// Scenarios (single thread, one symbol unless noted):
//   1. rest-heavy : limit orders away from the touch -> pure book inserts
//   2. crossing   : alternating buys/sells around a fair value -> matching
//   3. churn      : submit + immediately cancel -> insert/erase pressure
//
// Reports exact ns/op percentiles (vector + sort, unlike the gateway's
// bucketed histogram) and orders/sec.
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

#include "common/time.hpp"
#include "engine/matching_engine.hpp"

using namespace hft;

namespace {

volatile std::uint64_t g_sink = 0;  // keeps the compiler from folding results away

struct Sample {
  std::int64_t ns;
};

std::vector<std::int64_t> percentiles(std::vector<Sample>& samples,
                                      const std::vector<double>& qs) {
  std::vector<std::int64_t> ns;
  ns.reserve(samples.size());
  for (const Sample& s : samples) ns.push_back(s.ns);
  std::sort(ns.begin(), ns.end());
  std::vector<std::int64_t> out;
  for (double q : qs) {
    const std::size_t idx = std::min(ns.size() - 1,
        static_cast<std::size_t>(q / 100.0 * static_cast<double>(ns.size())));
    out.push_back(ns[idx]);
  }
  return out;
}

void report(const char* name, std::vector<Sample>& samples) {
  const auto ps = percentiles(samples, {50.0, 90.0, 99.0, 99.9});
  const std::int64_t total_ns = [&] {
    std::int64_t t = 0;
    for (const Sample& s : samples) t += s.ns;
    return t;
  }();
  std::printf("%-12s n=%-8zu p50=%-7lld p90=%-7lld p99=%-7lld p99.9=%-8lld -> %.2fM ops/s (avg %.0fns)\n",
              name, samples.size(), ps[0], ps[1], ps[2], ps[3],
              static_cast<double>(samples.size()) * 1e3 / static_cast<double>(total_ns),
              static_cast<double>(total_ns) / static_cast<double>(samples.size()));
}

constexpr int kN = 300'000;

// Bids park below fair value, asks above; jitter within each band so the
// book builds depth across ~2000 levels without ever crossing.
std::vector<OrderRequest> gen_rest_heavy() {
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> level(0, 1000);
  std::vector<OrderRequest> out;
  out.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    const bool buy = i % 2 == 0;
    const Price px = buy ? 10'000'00 - 50'00 - level(rng) * 5
                         : 10'000'00 + 50'00 + level(rng) * 5;
    out.push_back(OrderRequest{static_cast<ClientOrderId>(i + 1), 1,
                               buy ? Side::Buy : Side::Sell,
                               1 + static_cast<Qty>(rng() % 100), px, false});
  }
  return out;
}

// Prices jitter around fair value so roughly half the orders cross.
std::vector<OrderRequest> gen_crossing() {
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> jitter(-500, 500);
  std::vector<OrderRequest> out;
  out.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    const bool buy = i % 2 == 0;
    const Price px = 10'000'00 + jitter(rng);
    out.push_back(OrderRequest{static_cast<ClientOrderId>(i + 1), 1,
                               buy ? Side::Buy : Side::Sell,
                               1 + static_cast<Qty>(rng() % 100), px, false});
  }
  return out;
}

}  // namespace

int main() {
  std::printf("hft_bench: %d orders per scenario (single thread, steady_clock, ns)\n\n", kN);

  {  // 1. rest-heavy
    MatchingEngine engine;
    auto orders = gen_rest_heavy();
    std::vector<Sample> samples;
    samples.reserve(orders.size());
    for (const OrderRequest& o : orders) {
      const std::int64_t t0 = now_ns();
      const SubmitResult r = engine.submit(o);
      const std::int64_t t1 = now_ns();
      g_sink = g_sink * 31 + r.order_id * 131u + r.fill_count;
      samples.push_back({t1 - t0});
    }
    report("rest-heavy", samples);
    std::printf("             resting=%llu\n", (unsigned long long)engine.stats().resting);
  }

  {  // 2. crossing
    MatchingEngine engine;
    auto orders = gen_crossing();
    std::vector<Sample> samples;
    samples.reserve(orders.size());
    for (const OrderRequest& o : orders) {
      const std::int64_t t0 = now_ns();
      const SubmitResult r = engine.submit(o);
      const std::int64_t t1 = now_ns();
      g_sink = g_sink * 31 + r.order_id * 131u + r.fill_count;
      samples.push_back({t1 - t0});
    }
    report("crossing", samples);
    std::printf("             fills=%llu qty_traded=%llu resting=%llu\n",
                (unsigned long long)engine.stats().fills,
                (unsigned long long)engine.stats().qty_traded,
                (unsigned long long)engine.stats().resting);
  }

  {  // 3. churn: submit then cancel
    MatchingEngine engine;
    std::vector<Sample> samples;
    samples.reserve(kN);
    for (int i = 0; i < kN; ++i) {
      const OrderRequest o{static_cast<ClientOrderId>(i + 1), 1, Side::Buy, 10,
                           10'000'00 - 1'000 - i, false};
      const std::int64_t t0 = now_ns();
      const SubmitResult r = engine.submit(o);
      engine.cancel(r.order_id);
      const std::int64_t t1 = now_ns();
      g_sink = g_sink * 31 + r.order_id * 131u + r.fill_count;
      samples.push_back({t1 - t0});
    }
    report("churn", samples);
    std::printf("             resting=%llu (expect 0)\n",
                (unsigned long long)engine.stats().resting);
  }

  return 0;
}
