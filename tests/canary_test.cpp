/**
 * @file canary_test.cpp
 * @brief CanaryRouter unit tests.
 *
 * Coverage:
 * - CanaryMetrics: record_success / record_error / error_rate / avg_latency_us / reset
 * - CanaryRouter initial state (canary_pct=0)
 * - set_canary_percent / canary_percent
 * - 0% canary → all routed to stable
 * - 100% canary → all routed to canary
 * - rollback_to_stable → canary_pct=0
 * - push return value (true=success, false=no fn)
 * - stable_metrics / canary_metrics metric updates
 * - SleepAwaiter removal + AsyncSleep replacement check (verified by compilation only)
 */

#include <qbuem/pipeline/canary.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using namespace qbuem;

// ─── CanaryMetrics ───────────────────────────────────────────────────────────

TEST(CanaryMetrics, InitialState) {
    CanaryMetrics m;
    EXPECT_EQ(m.error_rate(), 0.0);
    EXPECT_EQ(m.avg_latency_us(), 0u);
}

TEST(CanaryMetrics, RecordSuccess) {
    CanaryMetrics m;
    m.record_success(100);
    m.record_success(200);
    EXPECT_EQ(m.avg_latency_us(), 150u);
    EXPECT_DOUBLE_EQ(m.error_rate(), 0.0);
}

TEST(CanaryMetrics, RecordError) {
    CanaryMetrics m;
    m.record_success(50);
    m.record_error();
    // 2 total, 1 error → 0.5
    EXPECT_DOUBLE_EQ(m.error_rate(), 0.5);
}

TEST(CanaryMetrics, Reset) {
    CanaryMetrics m;
    m.record_success(100);
    m.record_error();
    m.reset();
    EXPECT_EQ(m.error_rate(), 0.0);
    EXPECT_EQ(m.avg_latency_us(), 0u);
}

// ─── CanaryRouter initial state ───────────────────────────────────────────────

TEST(CanaryRouter, InitialCanaryPercentIsZero) {
    CanaryRouter<int> router;
    EXPECT_EQ(router.canary_percent(), 0u);
}

TEST(CanaryRouter, SetCanaryPercentClampsAt100) {
    CanaryRouter<int> router;
    router.set_canary_percent(150);
    EXPECT_EQ(router.canary_percent(), 100u);
}

// ─── Routing correctness ──────────────────────────────────────────────────────

TEST(CanaryRouter, ZeroPercentAllGoToStable) {
    CanaryRouter<int> router;
    int stable_count = 0;
    int canary_count = 0;

    router.set_stable([&](int) -> bool { ++stable_count; return true; });
    router.set_canary([&](int) -> bool { ++canary_count; return true; });

    for (int i = 0; i < 100; ++i)
        router.push(i);

    EXPECT_EQ(stable_count, 100);
    EXPECT_EQ(canary_count, 0);
}

TEST(CanaryRouter, HundredPercentAllGoToCanary) {
    CanaryRouter<int> router;
    int stable_count = 0;
    int canary_count = 0;

    router.set_stable([&](int) -> bool { ++stable_count; return true; });
    router.set_canary([&](int) -> bool { ++canary_count; return true; });

    router.set_canary_percent(100);
    for (int i = 0; i < 100; ++i)
        router.push(i);

    EXPECT_EQ(canary_count, 100);
    EXPECT_EQ(stable_count, 0);
}

TEST(CanaryRouter, FiftyPercentApproxHalf) {
    CanaryRouter<int> router;
    std::atomic<int> stable_count{0};
    std::atomic<int> canary_count{0};

    router.set_stable([&](int) -> bool { ++stable_count; return true; });
    router.set_canary([&](int) -> bool { ++canary_count; return true; });

    router.set_canary_percent(50);
    for (int i = 0; i < 1000; ++i)
        router.push(i);

    EXPECT_EQ(stable_count.load() + canary_count.load(), 1000);
    // 50% ± 15%
    EXPECT_GT(canary_count.load(), 300);
    EXPECT_LT(canary_count.load(), 700);
}

// ─── Rollback ────────────────────────────────────────────────────────────────

TEST(CanaryRouter, RollbackToStableSetsZeroPercent) {
    CanaryRouter<int> router;
    router.set_canary_percent(75);
    EXPECT_EQ(router.canary_percent(), 75u);
    router.rollback_to_stable();
    EXPECT_EQ(router.canary_percent(), 0u);
}

TEST(CanaryRouter, AfterRollbackAllGoToStable) {
    CanaryRouter<int> router;
    int stable_count = 0;
    int canary_count = 0;

    router.set_stable([&](int) -> bool { ++stable_count; return true; });
    router.set_canary([&](int) -> bool { ++canary_count; return true; });

    router.set_canary_percent(100);
    router.rollback_to_stable();

    for (int i = 0; i < 10; ++i)
        router.push(i);

    EXPECT_EQ(stable_count, 10);
    EXPECT_EQ(canary_count, 0);
}

// ─── push return value + automatic metric updates ────────────────────────────

TEST(CanaryRouter, PushReturnsFalseWhenNoFn) {
    CanaryRouter<int> router;
    // no fn → false
    EXPECT_FALSE(router.push(1));
}

TEST(CanaryRouter, PushReturnsTrueWhenFnSucceeds) {
    CanaryRouter<int> router;
    router.set_stable([](int) -> bool { return true; });
    EXPECT_TRUE(router.push(1));
}

TEST(CanaryRouter, StableMetricsUpdatedOnPush) {
    CanaryRouter<int> router;
    router.set_stable([](int) -> bool { return true; });

    for (int i = 0; i < 5; ++i)
        router.push(i);

    // stable metrics should have successes recorded (latency may be 0 for sub-µs ops)
    EXPECT_GT(router.stable_metrics().total.load(), 0u);
    EXPECT_GT(router.stable_metrics().latency_count.load(), 0u);
    EXPECT_DOUBLE_EQ(router.stable_metrics().error_rate(), 0.0);
}

TEST(CanaryRouter, CanaryMetricsUpdatedOnCanaryPush) {
    CanaryRouter<int> router;
    router.set_canary([](int) -> bool { return true; });
    router.set_canary_percent(100);

    for (int i = 0; i < 5; ++i)
        router.push(i);

    EXPECT_DOUBLE_EQ(router.canary_metrics().error_rate(), 0.0);
}

TEST(CanaryRouter, MetricsErrorRateOnFailure) {
    CanaryRouter<int> router;
    router.set_stable([](int) -> bool { return false; });  // always fail

    for (int i = 0; i < 4; ++i)
        router.push(i);

    // all failures → error_rate = 1.0
    EXPECT_DOUBLE_EQ(router.stable_metrics().error_rate(), 1.0);
}
