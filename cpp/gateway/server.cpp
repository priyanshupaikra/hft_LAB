#include "gateway/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "common/time.hpp"

namespace hft {
namespace {

bool send_all(int fd, const char* data, std::size_t len) {
  std::size_t sent = 0;
  while (sent < len) {
    const ssize_t n = ::send(fd, data + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool send_str(int fd, const std::string& s) {
  return send_all(fd, s.data(), s.size()) && send_all(fd, "\n", 1);
}

bool parse_ll(const char* s, long long& out) {
  if (s == nullptr) return false;
  char* end = nullptr;
  out = std::strtoll(s, &end, 10);
  return end != nullptr && *end == '\0' && end != s;
}

// Builds a queue-routed Command from an input line (everything except
// AUTH/PING/QUIT). Returns false + an error reply when malformed.
bool build_command(char* line, Command& cmd, std::string& err) {
  char* save = nullptr;
  const char* verb = ::strtok_r(line, " \t", &save);
  if (verb == nullptr) {
    err = "ERR EMPTY";
    return false;
  }
  cmd = Command{};

  if (::strcmp(verb, "NEW") == 0) {
    const char* a_cl = ::strtok_r(nullptr, " \t", &save);
    const char* a_side = ::strtok_r(nullptr, " \t", &save);
    const char* a_qty = ::strtok_r(nullptr, " \t", &save);
    const char* a_px = ::strtok_r(nullptr, " \t", &save);
    const char* a_sym = ::strtok_r(nullptr, " \t", &save);
    long long cl, qty, px;
    if (!parse_ll(a_cl, cl) || a_side == nullptr || !parse_ll(a_qty, qty) ||
        !parse_ll(a_px, px) || cl <= 0 || (a_side[0] != 'B' && a_side[0] != 'S')) {
      err = "ERR NEW MALFORMED";
      return false;
    }
    long long sym = 1;
    if (a_sym != nullptr && !parse_ll(a_sym, sym)) {
      err = "ERR NEW MALFORMED";
      return false;
    }
    cmd.type = Command::Type::New;
    cmd.order.cl_ord_id = static_cast<ClientOrderId>(cl);
    cmd.order.symbol = static_cast<SymbolId>(sym);
    cmd.order.side = a_side[0] == 'B' ? Side::Buy : Side::Sell;
    cmd.order.qty = qty;
    cmd.order.price = px;
    cmd.order.is_market = px == 0;  // price 0 => market (IOC)
    return true;
  }
  if (::strcmp(verb, "CXL") == 0) {
    long long cl;
    if (!parse_ll(::strtok_r(nullptr, " \t", &save), cl) || cl <= 0) {
      err = "ERR CXL MALFORMED";
      return false;
    }
    cmd.type = Command::Type::Cancel;
    cmd.cl = static_cast<ClientOrderId>(cl);
    return true;
  }
  if (::strcmp(verb, "EVENTS") == 0) { cmd.type = Command::Type::Events; return true; }
  if (::strcmp(verb, "STATS") == 0)  { cmd.type = Command::Type::Stats; return true; }
  if (::strcmp(verb, "KILL") == 0)   { cmd.type = Command::Type::Kill; return true; }
  if (::strcmp(verb, "RESUME") == 0) { cmd.type = Command::Type::Resume; return true; }
  if (::strcmp(verb, "SNAP") == 0) {
    long long sym = 1;
    const char* a_sym = ::strtok_r(nullptr, " \t", &save);
    if (a_sym != nullptr && !parse_ll(a_sym, sym)) {
      err = "ERR SNAP MALFORMED";
      return false;
    }
    cmd.type = Command::Type::Snap;
    cmd.symbol = static_cast<SymbolId>(sym);
    return true;
  }

  err = "ERR UNKNOWN_CMD";
  return false;
}

}  // namespace

int GatewayServer::run() {
  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    std::perror("socket");
    return 1;
  }
  int one = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port_);
  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("bind");
    return 1;
  }
  if (::listen(listen_fd, 128) < 0) {
    std::perror("listen");
    return 1;
  }
  std::fprintf(stderr, "hft_gateway listening on port %u\n", static_cast<unsigned>(port_));

  while (true) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    const int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (fd < 0) continue;
    int nd = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));  // latency > throughput here
    std::thread(&GatewayServer::handle_connection, this, fd).detach();
  }
}

void GatewayServer::handle_connection(int fd) {
  std::string leftover;
  char buf[4096];
  int slot = -1;
  SessionId sid = 0;
  bool authed = false;
  bool alive = true;

  // The entire per-order job of this thread: parse -> SPSC -> engine, then
  // write responses back as they arrive.
  auto forward = [&](Command cmd) {
    cmd.slot = static_cast<std::uint8_t>(slot);
    cmd.sid = sid;
    cmd.received_ns = now_ns();
    core_.submit_command(slot, cmd);
    Response r;
    while (core_.wait_response(slot, r)) {
      if (!send_all(fd, r.text, r.len) || !send_all(fd, "\n", 1)) {
        alive = false;
        return;
      }
      if (r.final) return;
    }
  };

  // AUTH against the immutable table + slot lease — control plane, no
  // engine involvement.
  auto do_auth = [&](char* line) {
    if (authed) return send_str(fd, "ERR AUTH ALREADY");
    char* save = nullptr;
    ::strtok_r(line, " \t", &save);  // AUTH
    const char* token = ::strtok_r(nullptr, " \t", &save);
    const TraderProfile* profile = token == nullptr ? nullptr : core_.authenticate(token);
    if (profile == nullptr) return send_str(fd, "ERR AUTH BAD_TOKEN");
    sid = core_.next_session_id();
    slot = core_.acquire_slot(profile, sid);
    if (slot < 0) return send_str(fd, "ERR AUTH BUSY");
    authed = true;
    char out[160];
    std::snprintf(out, sizeof(out), "OK AUTH trader=%s rate=%.0f burst=%.0f admin=%d",
                  profile->name.c_str(), profile->rate_per_sec, profile->burst,
                  profile->admin ? 1 : 0);
    return send_str(fd, out);
  };

  while (alive) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    leftover.append(buf, static_cast<std::size_t>(n));

    std::size_t start = 0;
    while (alive) {
      const std::size_t nl = leftover.find('\n', start);
      if (nl == std::string::npos) break;
      if (nl > start) {  // skip empty lines
        std::string line(leftover, start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "PING") {
          alive = send_str(fd, "PONG");
        } else if (line.rfind("AUTH", 0) == 0) {
          alive = do_auth(line.data());
        } else if (line == "QUIT") {
          alive = false;
          break;
        } else if (!authed) {
          alive = send_str(fd, "ERR AUTH REQUIRED");
        } else {
          Command cmd{};
          std::string err;
          if (build_command(line.data(), cmd, err)) {
            forward(cmd);
          } else {
            alive = send_str(fd, err);
          }
        }
      }
      start = nl + 1;
    }
    if (!alive) break;
    leftover.erase(0, start);
  }

  ::close(fd);
  if (slot >= 0) core_.release_slot(slot, sid);
}

}  // namespace hft
