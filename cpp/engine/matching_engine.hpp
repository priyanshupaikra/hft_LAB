#pragma once

#include <unordered_map>

#include "engine/order_book.hpp"

namespace hft {

// Routes orders to per-symbol books and owns engine order IDs.
//
// Sharding note: one MatchingEngine per symbol shard is exactly how real
// venues and HFT firms partition state — two symbols never interact, so they
// can live on different cores/machines with zero coordination (ROADMAP
// Phase 4 wires multiple engine processes behind a consistent-hash router).
// Order-level state that must be global (ID counter) stays local to this
// object; the gateway is the only writer.
class MatchingEngine {
 public:
  struct Stats {
    std::uint64_t accepted = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t fills = 0;
    std::uint64_t qty_traded = 0;
    std::uint64_t resting = 0;  // live resting orders right now
  };

  SubmitResult submit(const OrderRequest& req);
  // On success, sets *sym_out to the cancelled order's symbol (the caller
  // needs it to refresh market data for that book).
  bool cancel(EngineOrderId order_id, SymbolId* sym_out = nullptr);

  // Pre-size the big hash map (live orders) so the hot path never rehashes.
  void reserve_hint(std::size_t n) { order_symbol_.reserve(n); }

  Price last_trade(SymbolId symbol) const;
  OrderBook::TopOfBook top(SymbolId symbol) const;
  const Stats& stats() const noexcept { return stats_; }

 private:
  OrderBook& book_for(SymbolId symbol);

  std::unordered_map<SymbolId, OrderBook> books_;
  std::unordered_map<EngineOrderId, SymbolId> order_symbol_;  // for cancels
  std::unordered_map<SymbolId, Price> last_trade_;
  EngineOrderId next_order_id_ = 1;
  Stats stats_;
};

}  // namespace hft
