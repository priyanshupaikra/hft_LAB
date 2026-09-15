#pragma once

#include <cstdint>

#include "gateway/trading_core.hpp"

namespace hft {

// Minimal line-oriented TCP front-end for the trading core. One reader
// thread per connection; all it does is parse a line, hand a Command to the
// core's SPSC ring, and write the responses back. Every decision lives in
// the engine thread — this shell holds no trading state.
//
// Wire protocol (one command per line, responses newline-terminated):
//   AUTH <token>
//   NEW <clordid> <B|S> <qty> <price>        price 0 => market (IOC)
//   NEW <clordid> <B|S> <qty> <price> <sym>  optional symbol id
//   CXL <clordid>
//   EVENTS        -> drains this session's maker-fill notifications
//   STATS         -> single key=value line of counters + latency
//   SNAP <sym>    -> current top-of-book + market-data sequence number
//   KILL / RESUME -> admin only: force the kill switch open/closed
//   PING -> PONG
class GatewayServer {
 public:
  GatewayServer(TradingCore& core, std::uint16_t port) : core_(core), port_(port) {}

  // Blocking accept loop; returns non-zero on socket setup failure.
  int run();

 private:
  void handle_connection(int fd);
  std::string dispatch(char* line, bool& authed, int& slot, SessionId& sid);

  TradingCore& core_;
  std::uint16_t port_;
};

}  // namespace hft
