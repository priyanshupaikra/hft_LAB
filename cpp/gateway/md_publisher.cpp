#include "gateway/md_publisher.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace hft {
namespace {
void set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
}  // namespace

bool MdPublisher::start(const char** err) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    if (err) *err = "socket";
    return false;
  }
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port_);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    if (err) *err = "bind";
    return false;
  }
  if (::listen(listen_fd_, 64) < 0) {
    if (err) *err = "listen";
    return false;
  }
  set_nonblocking(listen_fd_);
  running_.store(true, std::memory_order_release);
  thread_ = std::thread(&MdPublisher::run, this);
  return true;
}

void MdPublisher::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
    return;
  }
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  std::lock_guard<std::mutex> lk(subs_mu_);
  for (int fd : subs_) ::close(fd);
  subs_.clear();
  subscriber_count_.store(0, std::memory_order_relaxed);
}

void MdPublisher::accept_new() {
  while (true) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) break;
    int nd = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
    set_nonblocking(fd);  // slow consumers get dropped, never block us
    std::lock_guard<std::mutex> lk(subs_mu_);
    subs_.push_back(fd);
    subscriber_count_.store(subs_.size(), std::memory_order_relaxed);
  }
}

void MdPublisher::broadcast(const char* frame, std::size_t len) {
  std::lock_guard<std::mutex> lk(subs_mu_);
  std::size_t out = 0;
  for (std::size_t i = 0; i < subs_.size(); ++i) {
    const ssize_t n = ::send(subs_[i], frame, len, MSG_NOSIGNAL);
    if (n >= 0) {
      subs_[out++] = subs_[i];  // keep healthy subscribers
    } else {
      ::close(subs_[i]);        // dead or slow: drop it, it can SNAP back
    }
  }
  subs_.resize(out);
  subscriber_count_.store(out, std::memory_order_relaxed);
}

void MdPublisher::run() {
  char frame[128];
  while (running_.load(std::memory_order_acquire)) {
    accept_new();
    bool did_work = false;
    MdUpdate u;
    while (in_.pop(u)) {
      did_work = true;
      // bid=0 / ask=0 means "no side" (prices are positive by construction).
      const int n = std::snprintf(frame, sizeof(frame), "MD sym=%u seq=%llu bid=%lld ask=%lld\n",
                                  (unsigned)u.symbol, (unsigned long long)u.seq,
                                  (long long)(u.has_bid ? u.bid : 0),
                                  (long long)(u.has_ask ? u.ask : 0));
      if (n > 0) broadcast(frame, static_cast<std::size_t>(n));
    }
    if (!did_work) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
}

}  // namespace hft
