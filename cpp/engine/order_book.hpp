#pragma once

#include <iterator>
#include <list>
#include <map>
#include <unordered_map>

#include "common/types.hpp"

namespace hft {

// Price-time-priority limit order book for one symbol.
//
// Design notes (the part you talk about in interviews):
// - Two ordered maps of price levels: bids descending, asks ascending, so
//   best price is always begin() — O(log P) to find the touch, O(1) amortized
//   to append at an existing level.
// - Each level holds a FIFO std::list of resting orders: time priority falls
//   out of append order for free.
// - An unordered_map<EngineOrderId, Locator> gives O(1) cancels: we store the
//   list iterator and erase it directly. (std::list iterators stay valid
//   while other elements are erased — unlike deque/vector.)
// - Matching allocates fills into a caller-owned vector; in a real engine
//   this would be a preallocated arena, but the shape of the code (no
//   allocation inside the match loop itself) is the same.
class OrderBook {
 public:
  struct RestingOrder {
    EngineOrderId order_id;
    ClientOrderId cl_ord_id;
    Qty qty_remaining;
    Price price;
    Side side;
  };

  using OrderList = std::list<RestingOrder>;

  struct TopOfBook {
    bool has_bid = false;
    bool has_ask = false;
    Price best_bid = 0;
    Price best_ask = 0;
  };

  // Submit an order; `order_id` must be unique. Market orders (is_market)
  // sweep the opposite side and any remainder is cancelled (IOC).
  SubmitResult submit(const OrderRequest& req, EngineOrderId order_id);

  // Cancel a resting order. Returns false if unknown or already gone.
  bool cancel(EngineOrderId order_id);

  TopOfBook top() const;

  std::size_t resting_orders() const noexcept { return locate_.size(); }

 private:
  struct Locator {
    bool is_bid;
    Price price;
    OrderList::iterator it;  // iterator into the level's list
  };

  template <typename BookSide>
  void match(const OrderRequest& req, BookSide& opposite, Fill* fills,
             std::uint32_t& count, Qty& remaining);

  template <typename BookSide>
  void rest_on_side(BookSide& side, const OrderRequest& req, EngineOrderId order_id,
                    Qty remaining);

  std::map<Price, OrderList, std::greater<Price>> bids_;
  std::map<Price, OrderList> asks_;
  std::unordered_map<EngineOrderId, Locator> locate_;
};

}  // namespace hft
