#include "gateway/trading_core.hpp"

#include <cstdio>
#include <cstring>

#include "common/time.hpp"

namespace hft {

TradingCore::TradingCore(std::vector<TraderProfile> traders)
    : traders_(std::move(traders)) {  // own the profiles: by_token_ points into this
  for (const TraderProfile& t : traders_) by_token_[t.token] = &t;
  slots_.reserve(kMaxSessions);
  for (int i = 0; i < kMaxSessions; ++i) slots_.push_back(std::make_unique<Slot>());
  eng_.resize(kMaxSessions);
  engine_.reserve_hint(65'536);  // pre-size engine hash maps: no rehash on hot path
  running_.store(true, std::memory_order_release);
  engine_thread_ = std::thread(&TradingCore::engine_loop, this);
}

TradingCore::~TradingCore() { shutdown(); }

void TradingCore::shutdown() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
    return;
  }
  { std::lock_guard<std::mutex> lk(idle_mu_); idle_cv_.notify_all(); }
  for (auto& slot : slots_) slot->wait_cv.notify_all();
  if (engine_thread_.joinable()) engine_thread_.join();
}

const TraderProfile* TradingCore::authenticate(const std::string& token) const {
  auto it = by_token_.find(token);
  return it == by_token_.end() ? nullptr : it->second;
}

SessionId TradingCore::next_session_id() {
  return next_sid_.fetch_add(1, std::memory_order_relaxed);
}

int TradingCore::acquire_slot(const TraderProfile* profile, SessionId sid) {
  for (int i = 0; i < kMaxSessions; ++i) {
    bool expected = false;
    if (slots_[i]->active.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
      // Written after taking ownership; the engine reads them only after
      // popping our first command, which happens-after these stores via the
      // ring's release/acquire on tail/head.
      slots_[i]->profile = profile;
      slots_[i]->sid = sid;
      return i;
    }
  }
  return -1;
}

void TradingCore::submit_command(int slot, const Command& cmd) {
  Slot& s = *slots_[slot];
  const bool was_empty = s.to_engine.empty();
  while (!s.to_engine.push(cmd)) {
    if (!running_.load(std::memory_order_acquire)) return;
    std::this_thread::yield();  // ring full: engine is far behind
  }
  // Wake the engine only if it was idle-parked (empty -> non-empty). Under
  // load this branch is never taken — the engine is already spinning
  // through slots and never sleeps.
  if (was_empty) {
    std::lock_guard<std::mutex> lk(idle_mu_);
    idle_cv_.notify_one();
  }
}

bool TradingCore::wait_response(int slot, Response& out) {
  Slot& s = *slots_[slot];
  for (;;) {
    if (s.to_client.pop(out)) return true;
    std::unique_lock<std::mutex> lk(s.wait_mu);
    if (s.to_client.pop(out)) return true;  // re-check under the parking lock
    if (!running_.load(std::memory_order_acquire)) return false;
    s.wait_cv.wait_for(lk, std::chrono::milliseconds(2));
  }
}

void TradingCore::release_slot(int slot, SessionId sid) {
  Command cmd{};
  cmd.type = Command::Type::Disconnect;
  cmd.slot = static_cast<std::uint8_t>(slot);
  cmd.sid = sid;
  while (!slots_[slot]->to_engine.push(cmd)) {
    if (!running_.load(std::memory_order_acquire)) return;
    std::this_thread::yield();
  }
  { std::lock_guard<std::mutex> lk(idle_mu_); idle_cv_.notify_one(); }

  // Drain until the engine's final BYE. When it arrives both rings are
  // empty with consistent counters — the next lease can start from them
  // as-is, which is why slot handoff needs no counter resets.
  Response resp;
  while (running_.load(std::memory_order_acquire) && wait_response(slot, resp)) {
    if (resp.final && resp.len >= 3 && std::memcmp(resp.text, "BYE", 3) == 0) break;
  }
  slots_[slot]->active.store(false, std::memory_order_release);
}

// ---- engine thread -----------------------------------------------------------

void TradingCore::engine_loop() {
  while (running_.load(std::memory_order_acquire)) {
    bool worked = false;
    for (int i = 0; i < kMaxSessions; ++i) {
      if (slots_[i]->active.load(std::memory_order_acquire)) worked |= drain_slot(i);
    }
    if (!worked) {
      std::unique_lock<std::mutex> lk(idle_mu_);
      idle_cv_.wait_for(lk, std::chrono::milliseconds(1));
    }
  }
}

bool TradingCore::drain_slot(int slot) {
  bool any = false;
  Command cmd;
  while (slots_[slot]->to_engine.pop(cmd)) {
    any = true;
    process(cmd);
  }
  return any;
}

void TradingCore::send_response(int slot, const Response& resp) {
  Slot& s = *slots_[slot];
  const bool was_empty = s.to_client.empty();
  while (!s.to_client.push(resp)) {
    if (!running_.load(std::memory_order_acquire)) return;
    std::this_thread::yield();  // client not draining: shouldn't happen
  }
  if (was_empty) {  // wake the parked connection thread
    std::lock_guard<std::mutex> lk(s.wait_mu);
    s.wait_cv.notify_one();
  }
}

namespace {
Response make_response(const char* text, bool final = true) {
  Response r{};
  r.final = final;
  r.len = static_cast<std::uint16_t>(std::strlen(text));
  if (r.len > sizeof(Response::text)) r.len = sizeof(Response::text);
  std::memcpy(r.text, text, r.len);
  return r;
}
}  // namespace

void TradingCore::process(const Command& cmd) {
  Slot& slot = *slots_[cmd.slot];
  EngineSession& s = eng_[cmd.slot];
  const std::int64_t now = now_ns();

  // Lease binding: the first command of a new lease initializes the
  // engine-side session; commands from a stale producer (an older lease
  // whose slot was recycled) fail the per-command sid checks below.
  if (s.sid != cmd.sid) {
    if (slot.active.load(std::memory_order_acquire) && slot.sid == cmd.sid &&
        slot.profile != nullptr) {
      s.reset(slot.profile, cmd.sid, now);
    }
  }

  switch (cmd.type) {
    case Command::Type::New: {
      const std::int64_t t0 = now_ns();
      const char* err = nullptr;
      SubmitResult result{};
      std::uint32_t fills_seen = 0;

      if (s.sid != cmd.sid || s.trader == nullptr) {
        ++counters_.rejected_auth;
        err = "NO_SESSION";
      } else if (!kill_.allows_order(now)) {
        ++counters_.rejected_kill;
        err = "KILL_SWITCH";
      } else if (!s.bucket.try_consume(now)) {
        ++counters_.rejected_rate;
        err = "RATE_LIMITED";
      } else if (!s.dedupe.seen_and_insert(cmd.order.cl_ord_id)) {
        ++counters_.rejected_dup;
        err = "DUPLICATE";
      } else if (cmd.order.qty <= 0 || cmd.order.qty > s.trader->max_order_qty ||
                 (!cmd.order.is_market && cmd.order.price <= 0)) {
        kill_.record_risk_reject(now);
        ++counters_.rejected_risk;
        err = (cmd.order.qty <= 0 || cmd.order.qty > s.trader->max_order_qty)
                  ? "RISK_QTY" : "RISK_PRICE";
      } else if (!cmd.order.is_market &&
                 cmd.order.qty * cmd.order.price > s.trader->max_notional) {
        kill_.record_risk_reject(now);
        ++counters_.rejected_risk;
        err = "RISK_NOTIONAL";
      } else {
        result = engine_.submit(cmd.order);
        ++counters_.accepted;
        kill_.record_success(now);
        fills_seen = result.fill_count;

        // Route maker fills to the session that owns the resting order.
        for (std::uint32_t i = 0; i < result.fill_count; ++i) {
          const Fill& f = result.fills[i];
          ++counters_.fills;
          auto owner = resting_owner_.find(f.maker_cl_ord_id);
          if (owner != resting_owner_.end()) {
            const std::uint8_t maker_slot = owner->second;
            EngineSession& maker = eng_[maker_slot];
            char line[96];
            const int n = std::snprintf(line, sizeof(line),
                                        "FILL cl=%llu px=%lld qty=%lld remaining=%lld",
                                        (unsigned long long)f.maker_cl_ord_id,
                                        (long long)f.price, (long long)f.qty,
                                        (long long)f.maker_remaining);
            maker.events.push(line, static_cast<std::uint16_t>(n > 0 ? n : 0));
            if (f.maker_remaining == 0) {
              resting_owner_.erase(owner);
              maker.live_orders.erase(f.maker_cl_ord_id);
            }
          }
        }
        if (result.status == SubmitStatus::Resting ||
            result.status == SubmitStatus::PartiallyFilled) {
          s.live_orders[cmd.order.cl_ord_id] = result.order_id;
          resting_owner_[cmd.order.cl_ord_id] = cmd.slot;
        }
      }

      const std::int64_t t1 = now_ns();
      core_lat_.record(t1 - cmd.received_ns);  // queue + processing
      proc_lat_.record(t1 - t0);               // processing only

      char text[256];
      if (err != nullptr) {
        std::snprintf(text, sizeof(text), "ERR NEW cl=%llu code=%s",
                      (unsigned long long)cmd.order.cl_ord_id, err);
      } else {
        const char* status_str = "RESTING";
        switch (result.status) {
          case SubmitStatus::Filled: status_str = "FILLED"; break;
          case SubmitStatus::PartiallyFilled: status_str = "PARTIAL"; break;
          case SubmitStatus::IocCancelled: status_str = "IOC_CANCELLED"; break;
          case SubmitStatus::Resting: break;
        }
        int off = std::snprintf(text, sizeof(text),
                                "OK NEW cl=%llu oid=%llu status=%s fills=%u",
                                (unsigned long long)cmd.order.cl_ord_id,
                                (unsigned long long)result.order_id, status_str,
                                fills_seen);
        int shown = 0;
        for (std::uint32_t i = 0; i < fills_seen && off > 0 && off < 240; ++i) {
          off += std::snprintf(text + off, sizeof(text) - off, " [%lldx%lld]",
                               (long long)result.fills[i].price,
                               (long long)result.fills[i].qty);
          if (++shown >= 4) break;
        }
      }
      send_response(cmd.slot, make_response(text));
      if (err == nullptr) {
        // Market data: publish top-of-book if this order moved it.
        const SymbolId sym = cmd.order.symbol;
        const OrderBook::TopOfBook top = engine_.top(sym);
        auto& last = last_top_[sym];
        if (top.has_bid != last.has_bid || top.has_ask != last.has_ask ||
            top.best_bid != last.best_bid || top.best_ask != last.best_ask) {
          last = top;
          const std::uint64_t seq = ++md_seq_[sym];
          MdUpdate u{sym, seq, top.best_bid, top.best_ask, top.has_bid, top.has_ask};
          if (md_out_.push(u)) ++counters_.md_updates; else ++counters_.md_dropped;
        }
      }
      break;
    }

    case Command::Type::Cancel: {
      char text[96];
      if (s.sid != cmd.sid || s.trader == nullptr) {
        std::snprintf(text, sizeof(text), "ERR CXL NO_SESSION");
      } else {
        auto live = s.live_orders.find(cmd.cl);
        if (live == s.live_orders.end()) {
          ++counters_.cancels_miss;
          std::snprintf(text, sizeof(text), "ERR CXL UNKNOWN cl=%llu",
                        (unsigned long long)cmd.cl);
        } else {
          SymbolId sym = 0;
          if (engine_.cancel(live->second, &sym)) {
            ++counters_.cancels_ok;
            s.live_orders.erase(live);
            resting_owner_.erase(cmd.cl);
            std::snprintf(text, sizeof(text), "OK CXL cl=%llu",
                          (unsigned long long)cmd.cl);
            const OrderBook::TopOfBook top = engine_.top(sym);
            auto& last = last_top_[sym];
            if (top.has_bid != last.has_bid || top.has_ask != last.has_ask ||
                top.best_bid != last.best_bid || top.best_ask != last.best_ask) {
              last = top;
              const std::uint64_t seq = ++md_seq_[sym];
              MdUpdate u{sym, seq, top.best_bid, top.best_ask, top.has_bid, top.has_ask};
              if (md_out_.push(u)) ++counters_.md_updates; else ++counters_.md_dropped;
            }
          } else {
            ++counters_.cancels_miss;  // filled between submit and cancel
            std::snprintf(text, sizeof(text), "ERR CXL UNKNOWN cl=%llu",
                          (unsigned long long)cmd.cl);
          }
        }
      }
      send_response(cmd.slot, make_response(text));
      break;
    }

    case Command::Type::Events: {
      char buf[sizeof(Response::text)];
      int off = 0;
      EventRecord rec;
      bool any = false;
      while (s.events.pop(rec)) {
        any = true;
        if (off + rec.len + 1 > (int)sizeof(buf)) {  // flush full response
          Response r{};
          r.final = false;
          r.len = static_cast<std::uint16_t>(off);
          std::memcpy(r.text, buf, off);
          send_response(cmd.slot, r);
          off = 0;
        }
        std::memcpy(buf + off, rec.text, rec.len);
        off += rec.len;
        buf[off++] = '\n';
      }
      if (!any) {
        send_response(cmd.slot, make_response("EVENTS_DONE"));
      } else {
        const int n = std::snprintf(buf + off, sizeof(buf) - off, "EVENTS_DONE");
        off += n > 0 ? n : 0;
        Response r{};
        r.final = true;
        r.len = static_cast<std::uint16_t>(off);
        std::memcpy(r.text, buf, off);
        send_response(cmd.slot, r);
      }
      break;
    }

    case Command::Type::Stats: {
      const MatchingEngine::Stats& es = engine_.stats();
      char text[sizeof(Response::text)];
      std::snprintf(text, sizeof(text),
                    "OK STATS accepted=%llu kill_rej=%llu rate_rej=%llu dup_rej=%llu "
                    "risk_rej=%llu cancels=%llu fills=%llu resting=%llu "
                    "kill=%s core_lat_p50_ns=%lld core_lat_p99_ns=%lld "
                    "proc_p50_ns=%lld proc_p99_ns=%lld md_updates=%llu md_dropped=%llu",
                    (unsigned long long)counters_.accepted,
                    (unsigned long long)counters_.rejected_kill,
                    (unsigned long long)counters_.rejected_rate,
                    (unsigned long long)counters_.rejected_dup,
                    (unsigned long long)counters_.rejected_risk,
                    (unsigned long long)counters_.cancels_ok,
                    (unsigned long long)counters_.fills,
                    (unsigned long long)es.resting, KillSwitch::to_str(kill_.state()),
                    core_lat_.percentile(50), core_lat_.percentile(99),
                    proc_lat_.percentile(50), proc_lat_.percentile(99),
                    (unsigned long long)counters_.md_updates,
                    (unsigned long long)counters_.md_dropped);
      send_response(cmd.slot, make_response(text));
      break;
    }

    case Command::Type::Snap: {
      const OrderBook::TopOfBook top = engine_.top(cmd.symbol);
      const auto seq_it = md_seq_.find(cmd.symbol);
      const std::uint64_t seq = seq_it == md_seq_.end() ? 0 : seq_it->second;
      char text[128];
      std::snprintf(text, sizeof(text), "OK SNAP sym=%u seq=%llu bid=%lld ask=%lld",
                    (unsigned)cmd.symbol, (unsigned long long)seq,
                    (long long)top.best_bid, (long long)top.best_ask);
      send_response(cmd.slot, make_response(text));
      break;
    }

    case Command::Type::Kill:
    case Command::Type::Resume: {
      const bool trip = cmd.type == Command::Type::Kill;
      if (s.trader == nullptr || !s.trader->admin) {
        send_response(cmd.slot,
                      make_response(trip ? "ERR KILL NOT_ADMIN" : "ERR RESUME NOT_ADMIN"));
      } else if (trip) {
        kill_.trip(now);
        send_response(cmd.slot, make_response("OK KILL tripped=1"));
      } else {
        kill_.reset();
        send_response(cmd.slot, make_response("OK RESUME kill=CLOSED"));
      }
      break;
    }

    case Command::Type::Disconnect: {
      if (s.sid == cmd.sid) {  // stale disconnects from old leases: ignore
        for (const auto& [cl, oid] : s.live_orders) {
          engine_.cancel(oid);
          resting_owner_.erase(cl);
        }
        s.reset(slot.profile, 0, now);
      }
      // BYE ends the release handshake; see release_slot().
      send_response(cmd.slot, make_response("BYE"));
      break;
    }
  }
}

}  // namespace hft
