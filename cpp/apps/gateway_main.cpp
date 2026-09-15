// hft_gateway — runs the demo trading venue.
//
//   ./hft_gateway [--port 5555] [--rate 1000] [--burst 2000] [--md-port 7600]
//
// --rate/--burst override every non-admin trader's order-rate throttle so
// demos can trip the limiter on purpose. --md-port starts the market-data
// publisher (top-of-book stream with sequence numbers); 0 disables it.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gateway/md_publisher.hpp"
#include "gateway/server.hpp"

int main(int argc, char** argv) {
  std::uint16_t port = 5555;
  std::uint16_t md_port = 7600;
  double rate = 1000.0;
  double burst = 2000.0;

  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--port") == 0) port = static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
    else if (std::strcmp(argv[i], "--md-port") == 0) md_port = static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
    else if (std::strcmp(argv[i], "--rate") == 0) rate = std::atof(argv[i + 1]);
    else if (std::strcmp(argv[i], "--burst") == 0) burst = std::atof(argv[i + 1]);
  }

  hft::TradingCore core(hft::default_traders(rate, burst));
  hft::GatewayServer server(core, port);

  hft::MdPublisher md(core.md_queue(), md_port);
  const char* md_err = nullptr;
  const bool md_on = md_port != 0 && md.start(&md_err);

  std::fprintf(stderr,
      "\nhft-lab gateway\n"
      "  port=%u  rate=%.0f/s  burst=%.0f  md_port=%s\n"
      "  tokens: dev-admin-token | dev-alpha-token | dev-beta-token\n"
      "  protocol: AUTH <tok> | NEW <cl> <B|S> <qty> <px> [sym] | CXL <cl>\n"
      "            EVENTS | STATS | SNAP <sym> | KILL | RESUME | PING | QUIT  (price 0 = market)\n\n",
      static_cast<unsigned>(port), rate, burst,
      md_on ? std::to_string(md_port).c_str() : "off");

  return server.run();
}
