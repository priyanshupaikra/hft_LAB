#include "engine/matching_engine.hpp"

namespace hft {

OrderBook& MatchingEngine::book_for(SymbolId symbol) {
  auto it = books_.find(symbol);
  if (it == books_.end()) it = books_.emplace(symbol, OrderBook{}).first;
  return it->second;
}

SubmitResult MatchingEngine::submit(const OrderRequest& req) {
  const EngineOrderId id = next_order_id_++;
  SubmitResult result = book_for(req.symbol).submit(req, id);

  ++stats_.accepted;
  stats_.fills += result.fill_count;
  for (std::uint32_t i = 0; i < result.fill_count; ++i) {
    const Fill& f = result.fills[i];
    stats_.qty_traded += f.qty;
    last_trade_[req.symbol] = f.price;
    if (f.maker_remaining == 0) {
      // A resting order died by matching (not by cancel) — retire it.
      order_symbol_.erase(f.maker_order_id);
      --stats_.resting;
    }
  }
  // Track live resting count and where each order lives for cancels.
  if (result.status == SubmitStatus::Resting ||
      result.status == SubmitStatus::PartiallyFilled) {
    order_symbol_[id] = req.symbol;
    ++stats_.resting;
  }
  return result;
}

bool MatchingEngine::cancel(EngineOrderId order_id, SymbolId* sym_out) {
  auto it = order_symbol_.find(order_id);
  if (it == order_symbol_.end()) return false;
  const SymbolId symbol = it->second;
  if (!books_.at(symbol).cancel(order_id)) return false;
  order_symbol_.erase(it);
  --stats_.resting;
  ++stats_.cancelled;
  if (sym_out != nullptr) *sym_out = symbol;
  return true;
}

Price MatchingEngine::last_trade(SymbolId symbol) const {
  auto it = last_trade_.find(symbol);
  return it == last_trade_.end() ? 0 : it->second;
}

OrderBook::TopOfBook MatchingEngine::top(SymbolId symbol) const {
  auto it = books_.find(symbol);
  return it == books_.end() ? OrderBook::TopOfBook{} : it->second.top();
}

}  // namespace hft
