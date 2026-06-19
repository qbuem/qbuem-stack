/**
 * @file tests/tick_loop_test.cpp
 * @brief TickLoop core tests — drift-free deadlines, deterministic catch-up,
 *        max_catchup bound + backlog, and the latency histogram.
 *
 * The advance(now, fn) overload takes a synthetic clock, so the scheduling
 * logic is tested deterministically with no threads or real sleeps.
 */

#include <qbuem/core/tick_loop.hpp>

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;
using Clock = TickLoop::Clock;

namespace {
TickInfo last; // captured by callbacks below
}

TEST(TickLoop, FirstCallEstablishesDeadlineFiresNothing) {
  TickLoop loop({.interval = 100ms});
  const auto t0 = Clock::now();
  std::vector<uint64_t> fired;
  uint32_t n = loop.advance(t0, [&](TickInfo i) { fired.push_back(i.index); });
  EXPECT_EQ(n, 0u);
  EXPECT_TRUE(fired.empty());
}

TEST(TickLoop, NoDriftOnExactSchedule) {
  TickLoop loop({.interval = 100ms});
  const auto t0 = Clock::now();
  std::vector<uint64_t> fired;
  auto cb = [&](TickInfo i) { fired.push_back(i.index); last = i; };

  loop.advance(t0, cb);                 // establish deadline @ t0+100
  EXPECT_EQ(loop.advance(t0 + 100ms, cb), 1u); // tick 0
  EXPECT_EQ(loop.advance(t0 + 200ms, cb), 1u); // tick 1
  EXPECT_EQ(loop.advance(t0 + 250ms, cb), 0u); // not due (deadline t0+300)
  EXPECT_EQ(loop.advance(t0 + 305ms, cb), 1u); // tick 2, ~5ms late

  ASSERT_EQ(fired.size(), 3u);
  EXPECT_EQ(fired[0], 0u);
  EXPECT_EQ(fired[1], 1u);
  EXPECT_EQ(fired[2], 2u);
  // Deadlines never drift: the tick-2 fire was scheduled at t0+300 and observed
  // at t0+305, so lateness ≈ 5 ms regardless of the earlier 250 ms idle gap.
  EXPECT_NEAR(static_cast<double>(last.lateness_ns), 5e6, 2e6);
  EXPECT_TRUE(last.is_catchup == false);
}

TEST(TickLoop, CatchUpRunsEveryMissedTickInOrder) {
  TickLoop loop({.interval = 100ms, .max_catchup = 8});
  const auto t0 = Clock::now();
  std::vector<uint64_t> fired;
  std::vector<bool> catchup;
  auto cb = [&](TickInfo i) { fired.push_back(i.index); catchup.push_back(i.is_catchup); };

  loop.advance(t0, cb);                  // deadline @ t0+100
  // Wake up very late: ticks due at +100, +200, +300 all fire in one call.
  uint32_t n = loop.advance(t0 + 350ms, cb);
  EXPECT_EQ(n, 3u);
  ASSERT_EQ(fired.size(), 3u);
  EXPECT_EQ(fired[0], 0u);
  EXPECT_EQ(fired[1], 1u);
  EXPECT_EQ(fired[2], 2u);
  // First is on-schedule (well, late but the first of the batch); the extra two
  // are catch-up ticks.
  EXPECT_FALSE(catchup[0]);
  EXPECT_TRUE(catchup[1]);
  EXPECT_TRUE(catchup[2]);
  EXPECT_EQ(loop.tick_count(), 3u);
}

TEST(TickLoop, MaxCatchupBoundsWorkAndReportsBacklog) {
  TickLoop loop({.interval = 100ms, .max_catchup = 2}); // budget = 3 per call
  const auto t0 = Clock::now();
  std::vector<uint64_t> fired;
  auto cb = [&](TickInfo i) { fired.push_back(i.index); };

  loop.advance(t0, cb); // deadline @ t0+100
  // 9 ticks are due (deadlines +100..+900); only 3 fire this wakeup.
  uint32_t n = loop.advance(t0 + 1000ms, cb);
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(loop.stats().backlog_events, 1u);
  EXPECT_GT(loop.behind(t0 + 1000ms), 0u);

  // The remaining ticks drain over subsequent wakeups — none skipped.
  uint32_t more = loop.advance(t0 + 1000ms, cb);
  EXPECT_EQ(more, 3u);
  EXPECT_EQ(fired.size(), 6u);
  for (uint64_t i = 0; i < fired.size(); ++i) EXPECT_EQ(fired[i], i); // contiguous
}

TEST(TickLoop, NextSleepMsIsDriftCompensated) {
  TickLoop loop({.interval = 100ms});
  const auto t0 = Clock::now();
  loop.advance(t0, [](TickInfo) {});      // deadline @ t0+100
  // At t0+40, 60 ms remain to the deadline.
  EXPECT_EQ(loop.next_sleep_ms(t0 + 40ms), 60);
  // Past the deadline ⇒ 0 (wake now to catch up).
  EXPECT_EQ(loop.next_sleep_ms(t0 + 130ms), 0);
  // Fire the due tick; deadline advances to t0+200 (drift-free: the 30 ms of
  // lateness does NOT push the next deadline out to t0+230).
  loop.advance(t0 + 130ms, [](TickInfo) {});
  EXPECT_EQ(loop.next_sleep_ms(t0 + 130ms), 70); // 200 - 130
}

TEST(TickLoop, StatsRecordLatenessAndTicks) {
  TickLoop loop({.interval = 10ms, .max_catchup = 64});
  const auto t0 = Clock::now();
  auto cb = [](TickInfo) {};
  loop.advance(t0, cb);
  for (int k = 1; k <= 50; ++k) loop.advance(t0 + std::chrono::milliseconds(10 * k), cb);

  auto s = loop.stats();
  EXPECT_EQ(s.ticks, 50u);
  EXPECT_GT(loop.jitter_histogram().count(), 0u);
  EXPECT_GT(loop.work_histogram().count(), 0u);
}

// ─── TickHistogram ────────────────────────────────────────────────────────────

TEST(TickHistogram, PercentileBuckets) {
  TickHistogram h;
  // 99 samples at 1µs, 1 sample at 100ms.
  for (int i = 0; i < 99; ++i) h.record(1'000);
  h.record(100'000'000);
  EXPECT_EQ(h.count(), 100u);
  // p50 falls in the 1µs region; p99/p999 reach the large outlier bucket.
  EXPECT_LE(h.percentile(0.50), 1'000u);
  EXPECT_GE(h.percentile(0.999), 100'000'000u);
}

TEST(TickHistogram, EmptyReturnsZero) {
  TickHistogram h;
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.percentile(0.99), 0u);
}
