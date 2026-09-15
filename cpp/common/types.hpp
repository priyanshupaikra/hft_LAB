#pragma once

#include <cstdint>
#include <vector>

namespace hft {

// Prices are fixed-point int64 scaled by 100 (i.e. cents). No floats anywhere
// on the order path: fixed-point avoids rounding drift and compares exactly.
using Price = std::int64_t;
using Qty = std::int64_t;
using ClientOrderId = std::uint64_t;  // assigned by the client (ClOrdID)
using EngineOrderId = std::uint64_t;  // assigned by the engine
using SymbolId = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
constexpr char to_char(Side s) { return s == Side::Buy ? 'B' : 'S'; }

struct OrderRequest {
  ClientOrderId cl_ord_id;
  SymbolId symbol;
  Side side;
  Qty qty;              // must be > 0
  Price price;          // scaled x100; ignored for market orders
  bool is_market;       // market orders behave as IOC (immediate-or-cancel)
};

struct Fill {
  Price price;
  Qty qty;
  Side taker_side;
  ClientOrderId taker_cl_ord_id;
  ClientOrderId maker_cl_ord_id;  // resting order that was hit
  EngineOrderId maker_order_id;
  Qty maker_remaining;            // 0 means the maker is fully done
};

enum class SubmitStatus : std::uint8_t {
  Resting,        // limit order parked on the book
  Filled,         // fully matched immediately
  PartiallyFilled,// partially matched, remainder resting
  IocCancelled,   // market order remainder discarded
};

// Fixed-capacity fill list. A vector here would allocate on every crossing
// order — unpredictable latency and ~100ns a pop. The cap also bounds the
// worst-case sweep of one market order; when the list is full the sweep
// stops and the remainder rests (limit) or is cancelled (market/IOC).
// Real venues cap sweeps for exactly this reason.
inline constexpr std::uint32_t kMaxFillsPerOrder = 32;

struct SubmitResult {
  SubmitStatus status;
  EngineOrderId order_id;
  Qty unfilled;
  std::uint32_t fill_count = 0;
  Fill fills[kMaxFillsPerOrder];
};

}  // namespace hft
