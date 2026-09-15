#include "tests/test_framework.hpp"

#include <thread>

#include "common/time.hpp"

#include "common/token_bucket.hpp"
#include "engine/matching_engine.hpp"
#include "gateway/dedupe_window.hpp"
#include "gateway/kill_switch.hpp"
#include "gateway/trading_core.hpp"
#include "transport/spsc_queue.hpp"

using namespace hft;

namespace {

OrderRequest limit(ClientOrderId cl, Side s, Qty q, Price p, SymbolId sym = 1) {
  return OrderRequest{cl, sym, s, q, p, /*is_market=*/false};
}

OrderRequest market(ClientOrderId cl, Side s, Qty q, SymbolId sym = 1) {
  return OrderRequest{cl, sym, s, q, /*price=*/0, /*is_market=*/true};
}

// ---- OrderBook / MatchingEngine ------------------------------------------------

void book_exact_fill() {
  MatchingEngine e;
  e.submit(limit(1, Side::Buy, 100, 10'000));
  SubmitResult r = e.submit(limit(2, Side::Sell, 100, 10'000));  // crosses
  CHECK_EQ(r.status, SubmitStatus::Filled);
  CHECK_EQ(r.fill_count, 1u);
  CHECK_EQ(r.fills[0].price, 10'000);   // trades at the MAKER's price
  CHECK_EQ(r.fills[0].qty, 100);
  CHECK_EQ(r.fills[0].maker_cl_ord_id, 1u);
  CHECK_EQ(e.stats().resting, 0u);
}

void book_partial_then_rest() {
  MatchingEngine e;
  e.submit(limit(1, Side::Buy, 100, 10'000));
  SubmitResult r = e.submit(limit(2, Side::Sell, 250, 10'000));
  CHECK_EQ(r.status, SubmitStatus::PartiallyFilled);
  CHECK_EQ(r.fill_count, 1u);
  CHECK_EQ(r.unfilled, 150);
  CHECK_EQ(e.stats().resting, 1u);  // the 150 remainder rests
  auto t = e.top(1);
  CHECK(t.has_ask && t.best_ask == 10'000);
}

void book_price_time_priority() {
  MatchingEngine e;
  e.submit(limit(1, Side::Buy, 10, 10'000));  // first at the level
  e.submit(limit(2, Side::Buy, 10, 10'000));  // second, same price
  SubmitResult r = e.submit(limit(3, Side::Sell, 15, 10'000));
  CHECK_EQ(r.fill_count, 2u);
  CHECK_EQ(r.fills[0].maker_cl_ord_id, 1u);  // older order fills first
  CHECK_EQ(r.fills[1].maker_cl_ord_id, 2u);
  CHECK_EQ(r.fills[1].maker_remaining, 5);   // second maker partially done
  CHECK_EQ(e.stats().resting, 1u);           // the 5 remainder rests
}

void book_best_price_first() {
  MatchingEngine e;
  e.submit(limit(1, Side::Buy, 10, 9'900));   // worse bid
  e.submit(limit(2, Side::Buy, 10, 10'000));  // better bid
  SubmitResult r = e.submit(limit(3, Side::Sell, 10, 9'000));  // crosses both
  CHECK_EQ(r.fill_count, 1u);
  CHECK_EQ(r.fills[0].price, 10'000);         // highest bid wins
  CHECK_EQ(r.fills[0].maker_cl_ord_id, 2u);
}

void market_order_sweeps_levels() {
  MatchingEngine e;
  e.submit(limit(1, Side::Sell, 5, 10'000));
  e.submit(limit(2, Side::Sell, 5, 10'100));
  SubmitResult r = e.submit(market(3, Side::Buy, 8));  // takes 5 + 3
  CHECK_EQ(r.status, SubmitStatus::Filled);
  CHECK_EQ(r.fill_count, 2u);
  CHECK_EQ(r.fills[0].price, 10'000);
  CHECK_EQ(r.fills[1].price, 10'100);
  CHECK_EQ(e.last_trade(1), 10'100);
}

void market_ioc_never_rests() {
  MatchingEngine e;
  SubmitResult r = e.submit(market(1, Side::Buy, 50));  // empty book
  CHECK_EQ(r.status, SubmitStatus::IocCancelled);
  CHECK_EQ(r.unfilled, 50);
  CHECK_EQ(e.stats().resting, 0u);
}

void cancel_frees_book() {
  MatchingEngine e;
  SubmitResult r1 = e.submit(limit(1, Side::Buy, 10, 10'000));
  CHECK(e.cancel(r1.order_id));
  CHECK(!e.cancel(r1.order_id));  // double cancel misses
  e.submit(limit(2, Side::Sell, 10, 9'000));  // no buy left to cross
  SubmitResult r3 = e.submit(market(3, Side::Buy, 10));
  CHECK_EQ(r3.fill_count, 1u);  // fills against the sell instead
  CHECK_EQ(r3.fills[0].price, 9'000);
}

void engine_routes_symbols() {
  MatchingEngine e;
  SubmitResult r1 = e.submit(limit(1, Side::Buy, 10, 10'000, /*sym=*/7));
  SubmitResult r2 = e.submit(limit(2, Side::Buy, 10, 20'000, /*sym=*/9));
  CHECK(r1.order_id != r2.order_id);
  CHECK_EQ(e.stats().resting, 2u);
  CHECK_EQ(e.last_trade(7), 0);  // no trades yet
}

// ---- TokenBucket ---------------------------------------------------------------

void token_bucket_burst_then_throttle() {
  const std::int64_t t0 = 1'000'000'000;
  TokenBucket tb(/*rate=*/100.0, /*burst=*/5.0, t0);
  int allowed = 0;
  for (int i = 0; i < 10; ++i) allowed += tb.try_consume(t0) ? 1 : 0;
  CHECK_EQ(allowed, 5);  // burst consumed, no refill at same instant

  CHECK(!tb.try_consume(t0 + 5'000'000));                  // +5ms -> +0.5 tokens
  CHECK(tb.try_consume(t0 + 15'000'000));                  // +15ms -> 1.5 >= 1
  CHECK_EQ(static_cast<int>(tb.available(t0 + 1'000'000'000)), 5);  // caps at burst
}

void token_bucket_steady_rate() {
  const std::int64_t t0 = 1'000'000'000;
  TokenBucket tb(/*rate=*/1000.0, /*burst=*/1.0, t0);
  tb.try_consume(t0);  // drain to 0
  int allowed = 0;
  for (int i = 1; i <= 100; ++i) {
    allowed += tb.try_consume(t0 + i * 1'000'000) ? 1 : 0;  // 1ms apart, 1/ms refill
  }
  // Each step refills exactly one token (1ms at 1000/s); allow ±1 for
  // double-rounding at the boundary.
  CHECK(allowed >= 99 && allowed <= 100);
}

// ---- KillSwitch ----------------------------------------------------------------

KillSwitch::Config fast_config() {
  KillSwitch::Config c;
  c.risk_rejects_to_trip = 3;
  c.window_ns = 1'000'000'000;
  c.cooldown_ns = 2'000'000'000;
  c.successes_to_close = 2;
  return c;
}

void kill_switch_auto_trip_and_recover() {
  KillSwitch ks(fast_config());
  const std::int64_t t = 1'000'000'000;
  CHECK(ks.allows_order(t));
  ks.record_risk_reject(t);
  ks.record_risk_reject(t + 1);
  CHECK(ks.allows_order(t + 2));  // 2 < 3 rejects: still closed
  ks.record_risk_reject(t + 3);
  CHECK(!ks.allows_order(t + 4));  // tripped: OPEN
  CHECK(!ks.allows_order(t + 5));

  CHECK(ks.allows_order(t + 4'000'000'000));  // cooldown elapsed -> HALF_OPEN
  ks.record_risk_reject(t + 4'000'000'001);   // re-offends during probation
  CHECK(!ks.allows_order(t + 4'000'000'002)); // re-tripped, doubled cooldown

  CHECK(ks.allows_order(t + 12'000'000'000)); // 8s later (2x cooldown): HALF_OPEN
  ks.record_success(t);
  ks.record_success(t);
  CHECK(ks.allows_order(t + 1));              // CLOSED again
}

void kill_switch_admin_override() {
  KillSwitch ks(fast_config());
  ks.trip(100);
  CHECK(!ks.allows_order(200));
  ks.reset();
  CHECK(ks.allows_order(300));
}

RUN_TEST(book_exact_fill);
RUN_TEST(book_partial_then_rest);
RUN_TEST(book_price_time_priority);
RUN_TEST(book_best_price_first);
RUN_TEST(market_order_sweeps_levels);
RUN_TEST(market_ioc_never_rests);
RUN_TEST(cancel_frees_book);
RUN_TEST(engine_routes_symbols);
RUN_TEST(token_bucket_burst_then_throttle);
RUN_TEST(token_bucket_steady_rate);
RUN_TEST(kill_switch_auto_trip_and_recover);
RUN_TEST(kill_switch_admin_override);

// ---- SPSC ring ------------------------------------------------------------------

void spsc_fifo_single_thread() {
  SpscQueue<int, 8> q;
  int v = 0;
  CHECK(q.pop(v) == false);
  for (int i = 0; i < 8; ++i) CHECK(q.push(i));
  CHECK(!q.push(99));  // full
  for (int i = 0; i < 8; ++i) {
    CHECK(q.pop(v));
    CHECK_EQ(v, i);  // FIFO order survives wraparound slots
  }
  CHECK(!q.pop(v));  // empty
  CHECK(q.push(42) && q.pop(v) && v == 42);  // reusable after drain
}

void spsc_two_threads() {
  SpscQueue<std::uint64_t, 256> q;
  constexpr std::uint64_t kN = 200'000;
  std::uint64_t sum = 0;
  std::thread consumer([&] {
    std::uint64_t v;
    for (std::uint64_t got = 0; got < kN;) {
      if (q.pop(v)) { sum += v; ++got; }
    }
  });
  std::thread producer([&] {
    for (std::uint64_t i = 1; i <= kN; ++i) {
      while (!q.push(i)) std::this_thread::yield();  // ring far smaller than kN
    }
  });
  producer.join();
  consumer.join();
  CHECK_EQ(sum, kN * (kN + 1) / 2);  // nothing lost, nothing duplicated
}

// ---- Dedupe window --------------------------------------------------------------

void dedupe_window_basics() {
  DedupeWindow w;
  CHECK(w.seen_and_insert(100));
  CHECK(!w.seen_and_insert(100));   // duplicate
  CHECK(w.seen_and_insert(101));
  CHECK(!w.seen_and_insert(101));
  CHECK(w.seen_and_insert(0) == false);  // 0 is never valid

  // Fill past one rotation: old ids stay dedupe-able (two-generation window).
  for (std::uint64_t i = 1; i <= 5'000; ++i) CHECK(w.seen_and_insert(10'000 + i));
  CHECK(!w.seen_and_insert(100));  // still remembered after rotation

  w.reset();
  CHECK(w.seen_and_insert(100));   // fresh window after reset
}

// ---- TradingCore (async, slot-based) ---------------------------------------------

// Push a command through a leased slot and return the joined response text.
std::string core_roundtrip(TradingCore& core, int slot, SessionId sid, Command cmd) {
  cmd.slot = static_cast<std::uint8_t>(slot);
  cmd.sid = sid;
  cmd.received_ns = now_ns();
  core.submit_command(slot, cmd);
  std::string out;
  Response r;
  while (core.wait_response(slot, r)) {
    out.append(r.text, r.len);
    out += '\n';
    if (r.final) break;
  }
  return out;
}

Command new_cmd(ClientOrderId cl, Side side, Qty qty, Price px) {
  Command c{};
  c.type = Command::Type::New;
  c.order.cl_ord_id = cl;
  c.order.symbol = 1;
  c.order.side = side;
  c.order.qty = qty;
  c.order.price = px;
  c.order.is_market = px == 0;
  return c;
}

void core_pipeline_and_dedupe() {
  TradingCore core(default_traders(1'000.0, 2'000.0));
  const TraderProfile* alpha = core.authenticate("dev-alpha-token");
  CHECK(alpha != nullptr);
  CHECK(core.authenticate("nope") == nullptr);
  const SessionId sid = core.next_session_id();
  const int slot = core.acquire_slot(alpha, sid);
  CHECK(slot >= 0);

  CHECK(core_roundtrip(core, slot, sid, new_cmd(1, Side::Buy, 10, 9'950))
            .rfind("OK NEW", 0) == 0);
  CHECK(core_roundtrip(core, slot, sid, new_cmd(2, Side::Sell, 10, 9'950))
            .find("FILLED") != std::string::npos);
  CHECK(core_roundtrip(core, slot, sid, new_cmd(1, Side::Buy, 10, 9'960))
            .find("DUPLICATE") != std::string::npos);          // idempotency
  CHECK(core_roundtrip(core, slot, sid, new_cmd(3, Side::Buy, 999'999, 9'960))
            .find("RISK_QTY") != std::string::npos);            // fat finger
}

void core_stats_and_kill_switch() {
  TradingCore core(default_traders(1'000.0, 2'000.0));
  const TraderProfile* admin = core.authenticate("dev-admin-token");
  const TraderProfile* alpha = core.authenticate("dev-alpha-token");
  const SessionId a_sid = core.next_session_id();
  const int a_slot = core.acquire_slot(admin, a_sid);
  const SessionId l_sid = core.next_session_id();
  const int l_slot = core.acquire_slot(alpha, l_sid);

  Command stats{};
  stats.type = Command::Type::Stats;
  const std::string s0 = core_roundtrip(core, l_slot, l_sid, stats);
  CHECK(s0.find("OK STATS") == 0 && s0.find("kill=CLOSED") != std::string::npos);

  Command kill{};
  kill.type = Command::Type::Kill;
  CHECK(core_roundtrip(core, l_slot, l_sid, kill).find("NOT_ADMIN") !=
        std::string::npos);  // alpha cannot kill
  CHECK(core_roundtrip(core, a_slot, a_sid, kill).find("OK KILL") == 0);

  CHECK(core_roundtrip(core, l_slot, l_sid, new_cmd(7, Side::Buy, 1, 9'950))
            .find("KILL_SWITCH") != std::string::npos);

  Command resume{};
  resume.type = Command::Type::Resume;
  CHECK(core_roundtrip(core, a_slot, a_sid, resume).find("kill=CLOSED") !=
        std::string::npos);
  CHECK(core_roundtrip(core, l_slot, l_sid, new_cmd(7, Side::Buy, 1, 9'950))
            .rfind("OK NEW", 0) == 0);
}

void core_slot_reuse() {
  TradingCore core(default_traders(1'000.0, 2'000.0));
  const TraderProfile* alpha = core.authenticate("dev-alpha-token");

  for (int lease = 0; lease < 3; ++lease) {  // hand the same slot around
    const SessionId sid = core.next_session_id();
    const int slot = core.acquire_slot(alpha, sid);
    CHECK(slot >= 0);
    // Same ClOrdID as the previous lease: fresh dedupe window must accept it.
    const std::string r = core_roundtrip(core, slot, sid, new_cmd(42, Side::Buy, 5, 9'900));
    CHECK(r.rfind("OK NEW", 0) == 0);
    core.release_slot(slot, sid);
  }
}

void core_maker_events_flow() {
  TradingCore core(default_traders(1'000.0, 2'000.0));
  const TraderProfile* alpha = core.authenticate("dev-alpha-token");
  const SessionId m_sid = core.next_session_id();
  const int m_slot = core.acquire_slot(alpha, m_sid);   // maker session
  const SessionId t_sid = core.next_session_id();
  const int t_slot = core.acquire_slot(alpha, t_sid);   // taker session

  core_roundtrip(core, m_slot, m_sid, new_cmd(1, Side::Sell, 10, 10'050));  // rests
  core_roundtrip(core, t_slot, t_sid, new_cmd(2, Side::Buy, 4, 10'050));    // crosses

  Command events{};
  events.type = Command::Type::Events;
  const std::string ev = core_roundtrip(core, m_slot, m_sid, events);
  CHECK(ev.find("FILL cl=1") != std::string::npos);
  CHECK(ev.find("remaining=6") != std::string::npos);
  CHECK(ev.find("EVENTS_DONE") != std::string::npos);
}

RUN_TEST(spsc_fifo_single_thread);
RUN_TEST(spsc_two_threads);
RUN_TEST(dedupe_window_basics);
RUN_TEST(core_pipeline_and_dedupe);
RUN_TEST(core_stats_and_kill_switch);
RUN_TEST(core_slot_reuse);
RUN_TEST(core_maker_events_flow);

}  // namespace
