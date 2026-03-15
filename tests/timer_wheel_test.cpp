/**
 * @file tests/timer_wheel_test.cpp
 * @brief Unit tests for TimerWheel (schedule, cancel, tick, next_expiry_ms).
 */

#include <gtest/gtest.h>
#include <qbuem/core/timer_wheel.hpp>

#include <atomic>
#include <functional>
#include <vector>

using namespace qbuem;

// ─── schedule / tick ──────────────────────────────────────────────────────────

TEST(TimerWheelTest, ScheduledCallbackFiresAfterDelay) {
    TimerWheel wheel;
    int count = 0;
    wheel.schedule(100, [&] { ++count; });

    // Not yet — 50ms elapsed
    size_t fired = wheel.tick(50);
    EXPECT_EQ(fired, 0u);
    EXPECT_EQ(count, 0);

    // Now — total 100ms
    fired = wheel.tick(50);
    EXPECT_EQ(fired, 1u);
    EXPECT_EQ(count, 1);
}

TEST(TimerWheelTest, MultipleTimersFire) {
    TimerWheel wheel;
    std::vector<int> fired_ids;

    wheel.schedule(50,  [&] { fired_ids.push_back(1); });
    wheel.schedule(100, [&] { fired_ids.push_back(2); });
    wheel.schedule(200, [&] { fired_ids.push_back(3); });

    wheel.tick(50);   // T1 fires
    wheel.tick(50);   // T2 fires (total 100ms)
    wheel.tick(100);  // T3 fires (total 200ms)

    ASSERT_EQ(fired_ids.size(), 3u);
    EXPECT_EQ(fired_ids[0], 1);
    EXPECT_EQ(fired_ids[1], 2);
    EXPECT_EQ(fired_ids[2], 3);
}

TEST(TimerWheelTest, TimersFireInOrder) {
    TimerWheel wheel;
    std::vector<std::string> order;

    wheel.schedule(50,  [&] { order.push_back("early"); });
    wheel.schedule(100, [&] { order.push_back("late"); });

    wheel.tick(100);  // Both should fire, early first

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "early");
    EXPECT_EQ(order[1], "late");
}

// ─── cancel ───────────────────────────────────────────────────────────────────

TEST(TimerWheelTest, CancelPreventsCallback) {
    TimerWheel wheel;
    int count = 0;
    auto id = wheel.schedule(100, [&] { ++count; });

    bool ok = wheel.cancel(id);
    EXPECT_TRUE(ok);

    wheel.tick(200);  // Past the deadline
    EXPECT_EQ(count, 0);
}

TEST(TimerWheelTest, CancelInvalidIdReturnsFalse) {
    TimerWheel wheel;
    bool ok = wheel.cancel(TimerWheel::kInvalid);
    EXPECT_FALSE(ok);
}

TEST(TimerWheelTest, CancelAlreadyFiredIdReturnsFalse) {
    TimerWheel wheel;
    auto id = wheel.schedule(10, [] {});
    wheel.tick(20);  // fires

    bool ok = wheel.cancel(id);
    EXPECT_FALSE(ok);
}

TEST(TimerWheelTest, OnlyTargetTimerCancelled) {
    TimerWheel wheel;
    int a = 0, b = 0;
    auto id_a = wheel.schedule(50, [&] { ++a; });
    wheel.schedule(50, [&] { ++b; });

    wheel.cancel(id_a);
    wheel.tick(100);

    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
}

// ─── next_expiry_ms ───────────────────────────────────────────────────────────

TEST(TimerWheelTest, NextExpiryMsReturnsApproximateDelay) {
    TimerWheel wheel;
    wheel.schedule(300, [] {});
    uint64_t next = wheel.next_expiry_ms();
    // Should be roughly 300 (the wheel has 1ms resolution at level 0)
    EXPECT_GE(next, 298u);
    EXPECT_LE(next, 302u);
}

TEST(TimerWheelTest, NextExpiryMsMaxWhenNoTimers) {
    TimerWheel wheel;
    uint64_t next = wheel.next_expiry_ms();
    // No timers → returns a very large value (or UINT64_MAX)
    EXPECT_GT(next, 0u);
}

// ─── immediate timer (delay=0) ────────────────────────────────────────────────

TEST(TimerWheelTest, ImmediateTimerFiresOnZeroTick) {
    TimerWheel wheel;
    int count = 0;
    wheel.schedule(0, [&] { ++count; });
    wheel.tick(0);
    EXPECT_EQ(count, 1);
}

// ─── count ────────────────────────────────────────────────────────────────────

TEST(TimerWheelTest, CountReflectsActiveTimers) {
    TimerWheel wheel;
    EXPECT_EQ(wheel.count(), 0u);

    auto id1 = wheel.schedule(100, [] {});
    auto id2 = wheel.schedule(200, [] {});
    EXPECT_EQ(wheel.count(), 2u);

    wheel.cancel(id1);
    EXPECT_EQ(wheel.count(), 1u);

    wheel.tick(200);  // id2 fires
    EXPECT_EQ(wheel.count(), 0u);
}

// ─── stress: 100 timers ───────────────────────────────────────────────────────

TEST(TimerWheelTest, StressScheduleAndFireAllTimers) {
    TimerWheel wheel;
    std::atomic<size_t> fired{0};

    for (size_t i = 1; i <= 100; ++i)
        wheel.schedule(i, [&] { fired.fetch_add(1, std::memory_order_relaxed); });

    for (uint64_t ms = 1; ms <= 100; ++ms)
        wheel.tick(1);

    EXPECT_EQ(fired.load(), 100u);
}
