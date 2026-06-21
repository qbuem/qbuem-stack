/**
 * @file tests/tick_loop_test.cpp
 * @brief TickLoop core tests — drift-free deadlines, deterministic catch-up,
 *        max_catchup bound + backlog, and the latency histogram.
 *
 * The advance(now, fn) overload takes a synthetic clock, so the scheduling
 * logic is tested deterministically with no threads or real sleeps.
 */

#include <qbuem/core/tick_loop.hpp>
#include <qbuem/core/tick_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <thread>
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

// ─── run_pinned + concurrency + overrun (real threads / real clock) ───────────

TEST(TickLoop, RunPinnedTicksAndStops) {
  TickLoop loop({.interval = 2ms, .spin_window = 200us});
  std::atomic<uint64_t> count{0};
  const auto t0 = std::chrono::steady_clock::now();
  std::jthread t([&] {
    loop.run_pinned([&](TickInfo) { count.fetch_add(1, std::memory_order_relaxed); });
  });
  std::this_thread::sleep_for(120ms);
  loop.stop();
  t.join();
  // The loop ticks for its WHOLE lifetime (thread spawn → stop() observed →
  // join returns), not just the 120 ms sleep: on a loaded CI runner stop()/join
  // can add 100+ ms, and a drift-free 2 ms loop running that long legitimately
  // produces proportionally more ticks (e.g. 131 over ~262 ms). So anchor the
  // upper bound to the ACTUAL elapsed wall-time rather than the nominal sleep.
  // A fixed-timestep loop cannot exceed elapsed/interval ticks even with
  // catch-up, so elapsed_ms/2 + margin is a hard ceiling; the lower bound stays
  // a loose fixed floor (it only ever runs over the budget, never under it).
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();
  const uint64_t ceiling = static_cast<uint64_t>(elapsed_ms) / 2 + 20;
  EXPECT_GE(count.load(), 10u);
  EXPECT_LE(count.load(), ceiling);
  EXPECT_FALSE(loop.running());
  EXPECT_EQ(loop.stats().ticks, count.load());
}

TEST(TickLoop, ConcurrentStatsReadIsSafe) {
  // A reader hammers stats() while the tick thread runs — the cross-thread-read
  // claim is exactly what TSan/ASan validate here.
  TickLoop loop({.interval = 1ms, .spin_window = 100us});
  std::atomic<bool> go{true};
  std::atomic<uint64_t> sink{0};
  std::jthread reader([&] {
    while (go.load(std::memory_order_relaxed)) {
      auto s = loop.stats();
      sink.fetch_add(s.ticks + s.jitter_p99_ns + s.work_p99_ns,
                     std::memory_order_relaxed); // prevent elision
    }
  });
  std::jthread driver([&] { loop.run_pinned([](TickInfo) {}); });
  std::this_thread::sleep_for(80ms);
  loop.stop();
  driver.join();
  go.store(false, std::memory_order_relaxed);
  reader.join();
  SUCCEED(); // no data race / crash
}

TEST(TickLoop, OverrunAndLoadDetected) {
  TickLoop loop({.interval = 1ms, .max_catchup = 0});
  const auto t0 = Clock::now();
  loop.advance(t0, [](TickInfo) {});
  // A callback that takes longer than one interval → overrun + load > 0.
  loop.advance(t0 + 1ms,
               [](TickInfo) { std::this_thread::sleep_for(3ms); });
  auto s = loop.stats();
  EXPECT_GE(s.overruns, 1u);
  EXPECT_GT(s.load, 0.0);
  EXPECT_GT(s.max_work_ns, 1'000'000u); // > 1 ms work recorded
}

// ─── TickScheduler ────────────────────────────────────────────────────────────

namespace {
// Stateful synthetic-clock driver: keeps ONE monotonic timeline for a scheduler
// so multiple drive segments compose (each drive() resetting t0 would clash with
// the loop's persisted deadline).
struct Driver {
  TickScheduler& s;
  std::chrono::milliseconds interval;
  Clock::time_point t0 = Clock::now();
  int k = 0;
  Driver(TickScheduler& sc, std::chrono::milliseconds iv) : s(sc), interval(iv) {
    s.advance(t0); // establish the first deadline at t0 + interval
  }
  void tick(int base_ticks) {
    for (int i = 0; i < base_ticks; ++i) { ++k; s.advance(t0 + interval * k); }
  }
};
// One-shot helper for single-segment tests.
void drive(TickScheduler& s, int base_ticks, std::chrono::milliseconds interval) {
  Driver(s, interval).tick(base_ticks);
}
} // namespace

TEST(TickScheduler, MultiRateSystemsFireAtTheirOwnRate) {
  TickScheduler s({.interval = 10ms});
  std::atomic<int> n1{0}, n2{0}, n3{0};
  s.add_system({.every = 1, .name = "every1"}, [&](const TickContext&) { n1++; });
  s.add_system({.every = 2, .name = "every2"}, [&](const TickContext&) { n2++; });
  s.add_system({.every = 3, .name = "every3"}, [&](const TickContext&) { n3++; });

  drive(s, 6, 10ms); // 6 sim ticks (time_scale 1.0)
  EXPECT_EQ(s.tick(), 6u);
  EXPECT_EQ(n1.load(), 6);          // every tick
  EXPECT_EQ(n2.load(), 3);          // ticks 0,2,4
  EXPECT_EQ(n3.load(), 2);          // ticks 0,3
}

TEST(TickScheduler, SystemsRunInOrder) {
  TickScheduler s({.interval = 10ms});
  std::vector<int> seq;
  s.add_system({.order = 30, .name = "c"}, [&](const TickContext&) { seq.push_back(30); });
  s.add_system({.order = 10, .name = "a"}, [&](const TickContext&) { seq.push_back(10); });
  s.add_system({.order = 20, .name = "b"}, [&](const TickContext&) { seq.push_back(20); });
  drive(s, 1, 10ms);
  ASSERT_EQ(seq.size(), 3u);
  EXPECT_EQ(seq[0], 10);
  EXPECT_EQ(seq[1], 20);
  EXPECT_EQ(seq[2], 30);
}

TEST(TickScheduler, DeterministicRngIsReplayable) {
  auto run = [](uint64_t seed) {
    TickScheduler s({.interval = 10ms});
    s.set_seed(seed);
    std::vector<uint64_t> out;
    s.add_system({.every = 1}, [&](const TickContext& c) { out.push_back(c.rng->next_u64()); });
    drive(s, 10, 10ms);
    return out;
  };
  auto a = run(123), b = run(123), c = run(456);
  EXPECT_EQ(a.size(), 10u);
  EXPECT_EQ(a, b); // same (seed) ⇒ identical stream — replay-verifiable
  EXPECT_NE(a, c); // different seed ⇒ different
}

TEST(TickScheduler, PauseStepResume) {
  TickScheduler s({.interval = 10ms});
  std::atomic<int> n{0};
  s.add_system({.every = 1}, [&](const TickContext&) { n++; });

  Driver d(s, 10ms);            // one monotonic timeline across all segments
  s.pause();
  d.tick(5);                    // paused ⇒ no sim ticks
  EXPECT_EQ(n.load(), 0);
  EXPECT_EQ(s.tick(), 0u);

  s.step(3);                    // queue 3 single-steps (still paused)
  d.tick(1);
  EXPECT_EQ(n.load(), 3);

  s.resume();
  d.tick(4);
  EXPECT_EQ(n.load(), 7);       // 3 stepped + 4 resumed
}

TEST(TickScheduler, TimeScaleFastAndSlow) {
  { // 2x: each base tick → 2 sim ticks
    TickScheduler s({.interval = 10ms});
    std::atomic<int> n{0};
    s.add_system({.every = 1}, [&](const TickContext&) { n++; });
    s.set_time_scale(2.0);
    drive(s, 3, 10ms);
    EXPECT_EQ(n.load(), 6);
  }
  { // 0.5x: one sim tick per 2 base ticks
    TickScheduler s({.interval = 10ms});
    std::atomic<int> n{0};
    s.add_system({.every = 1}, [&](const TickContext&) { n++; });
    s.set_time_scale(0.5);
    drive(s, 6, 10ms);
    EXPECT_EQ(n.load(), 3);
  }
}

TEST(TickScheduler, PerSystemMetricsAndOverrunWatchdog) {
  TickScheduler s({.interval = 5ms});
  uint32_t slow = s.add_system({.every = 1, .name = "slow"},
                               [&](const TickContext&) { std::this_thread::sleep_for(2ms); });
  std::atomic<int> alarms{0};
  s.set_overrun_handler(1ms, [&](uint32_t id, uint64_t) { if (id == slow) alarms++; });

  drive(s, 3, 5ms);
  auto st = s.system_stats(slow);
  EXPECT_EQ(st.ticks, 3u);
  EXPECT_GT(st.max_work_ns, 1'000'000u);     // > 1 ms work recorded
  EXPECT_EQ(s.system_overruns(slow), 3u);    // every run exceeded the 1 ms budget
  EXPECT_EQ(alarms.load(), 3);
}

TEST(TickScheduler, ConcurrentStatsReadIsSafe) {
  TickScheduler s({.interval = 1ms, .spin_window = 100us});
  uint32_t id = s.add_system({.every = 1}, [](const TickContext&) {});
  std::atomic<bool> go{true};
  std::atomic<uint64_t> sink{0};
  std::jthread reader([&] {
    while (go.load(std::memory_order_relaxed)) {
      auto a = s.stats();
      auto b = s.system_stats(id);
      sink.fetch_add(a.ticks + b.ticks + b.work_p99_ns, std::memory_order_relaxed);
    }
  });
  std::jthread driver([&] { s.run_pinned(); });
  std::this_thread::sleep_for(80ms);
  s.stop();
  driver.join();
  go.store(false, std::memory_order_relaxed);
  reader.join();
  SUCCEED(); // no data race / crash under TSan + ASan
}
