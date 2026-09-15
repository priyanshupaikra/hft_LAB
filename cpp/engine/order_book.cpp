#include "engine/order_book.hpp"

namespace hft {

SubmitResult OrderBook::submit(const OrderRequest& req, EngineOrderId order_id) {
  SubmitResult result;
  result.order_id = order_id;

  Qty remaining = req.qty;
  std::uint32_t fills = 0;

  if (req.side == Side::Buy) {
    match(req, asks_, result.fills, fills, remaining);
  } else {
    match(req, bids_, result.fills, fills, remaining);
  }
  result.fill_count = fills;

  // Anything left after the sweep?
  if (remaining == 0) {
    result.status = SubmitStatus::Filled;
  } else if (req.is_market) {
    // IOC semantics: a market order never rests — price protection.
    result.status = SubmitStatus::IocCancelled;
    result.unfilled = remaining;
  } else {
    // Rest the remainder at its limit price, at the back of the level (time
    // priority).
    if (req.side == Side::Buy) {
      rest_on_side(bids_, req, order_id, remaining);
    } else {
      rest_on_side(asks_, req, order_id, remaining);
    }
    result.unfilled = remaining;
    result.status =
        fills == 0 ? SubmitStatus::Resting : SubmitStatus::PartiallyFilled;
  }

  return result;
}

// Walks the opposite side of the book while prices cross. Works for both
// sides because both maps expose begin() = best price first.
template <typename BookSide>
void OrderBook::match(const OrderRequest& req, BookSide& opposite, Fill* fills,
                      std::uint32_t& count, Qty& remaining) {
  auto level_it = opposite.begin();
  while (remaining > 0 && level_it != opposite.end() && count < kMaxFillsPerOrder) {
    const Price level_price = level_it->first;
    if (!req.is_market &&
        (req.side == Side::Buy ? req.price < level_price : req.price > level_price)) {
      break;  // limit no longer crosses
    }

    auto& level_orders = level_it->second;
    auto order_it = level_orders.begin();
    while (order_it != level_orders.end() && remaining > 0) {
      RestingOrder& maker = *order_it;
      const Qty fill_qty = std::min(remaining, maker.qty_remaining);

      fills[count++] = Fill{
          /*price=*/level_price,
          /*qty=*/fill_qty,
          /*taker_side=*/req.side,
          /*taker_cl_ord_id=*/req.cl_ord_id,
          /*maker_cl_ord_id=*/maker.cl_ord_id,
          /*maker_order_id=*/maker.order_id,
          /*maker_remaining=*/maker.qty_remaining - fill_qty,
      };

      remaining -= fill_qty;
      maker.qty_remaining -= fill_qty;

      if (maker.qty_remaining == 0) {
        locate_.erase(maker.order_id);
        order_it = level_orders.erase(order_it);
      } else {
        break;  // this maker absorbed the whole remaining taker qty
      }
    }

    if (level_orders.empty()) {
      level_it = opposite.erase(level_it);
    } else {
      ++level_it;
    }
  }
}

// Appends `remaining` to the back of its price level on one side.
template <typename BookSide>
void OrderBook::rest_on_side(BookSide& side, const OrderRequest& req,
                             EngineOrderId order_id, Qty remaining) {
  auto& level = side[req.price];
  level.push_back(RestingOrder{order_id, req.cl_ord_id, remaining, req.price, req.side});
  locate_[order_id] = Locator{req.side == Side::Buy, req.price, std::prev(level.end())};
}

bool OrderBook::cancel(EngineOrderId order_id) {
  auto loc = locate_.find(order_id);
  if (loc == locate_.end()) return false;

  if (loc->second.is_bid) {
    auto level_it = bids_.find(loc->second.price);
    level_it->second.erase(loc->second.it);
    if (level_it->second.empty()) bids_.erase(level_it);
  } else {
    auto level_it = asks_.find(loc->second.price);
    level_it->second.erase(loc->second.it);
    if (level_it->second.empty()) asks_.erase(level_it);
  }
  locate_.erase(loc);
  return true;
}

OrderBook::TopOfBook OrderBook::top() const {
  TopOfBook t;
  if (!bids_.empty()) {
    t.has_bid = true;
    t.best_bid = bids_.begin()->first;
  }
  if (!asks_.empty()) {
    t.has_ask = true;
    t.best_ask = asks_.begin()->first;
  }
  return t;
}

// Explicit instantiation for both map flavors used by the templates.
template void OrderBook::rest_on_side<std::map<Price, OrderBook::OrderList, std::greater<Price>>>(
    std::map<Price, OrderBook::OrderList, std::greater<Price>>&, const OrderRequest&,
    EngineOrderId, Qty);
template void OrderBook::rest_on_side<std::map<Price, OrderBook::OrderList>>(
    std::map<Price, OrderBook::OrderList>&, const OrderRequest&, EngineOrderId, Qty);

template void OrderBook::match<std::map<Price, OrderBook::OrderList, std::greater<Price>>>(
    const OrderRequest&, std::map<Price, OrderBook::OrderList, std::greater<Price>>&,
    Fill*, std::uint32_t&, Qty&);
template void OrderBook::match<std::map<Price, OrderBook::OrderList>>(
    const OrderRequest&, std::map<Price, OrderBook::OrderList>&, Fill*, std::uint32_t&, Qty&);

}  // namespace hft
