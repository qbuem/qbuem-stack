/**
 * @file scatter_gather_test.cpp
 * @brief ScatterGatherAction unit tests.
 *
 * Coverage:
 * - ScatterGatherAction construction (scatter/process/gather λ)
 * - try_push: success / false when over capacity
 * - input() channel access
 * - max_parallel config applied (Config::max_parallel)
 * - DebounceAction Config: gap / channel_cap
 * - ThrottleAction Config: rate_per_sec / burst / channel_cap
 */

#include <qbuem/pipeline/event_actions.hpp>
#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <sstream>

using namespace qbuem;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<int> split_ints(int n, int count) {
    std::vector<int> v;
    for (int i = 0; i < count; ++i) v.push_back(n + i);
    return v;
}

// ─── ScatterGatherAction construction validation ──────────────────────────────

TEST(ScatterGatherAction, ConstructionDoesNotThrow) {
    // EXPECT_NO_THROW doesn't work with commas in template args; just construct directly
    ScatterGatherAction<int, int, int, int> action(
        [](int n) -> std::vector<int> { return {n}; },
        [](int x, ActionEnv) -> Task<Result<int>> { co_return Result<int>(x); },
        [](int /*orig*/, std::vector<int> v) -> int {
            return v.empty() ? 0 : v[0];
        },
        {.max_parallel = 4, .channel_cap = 32}
    );
    SUCCEED(); // reached without exception
}

TEST(ScatterGatherAction, InputChannelIsNotNull) {
    ScatterGatherAction<int, int, int, int> action(
        [](int n) -> std::vector<int> { return {n}; },
        [](int x, ActionEnv) -> Task<Result<int>> { co_return Result<int>(x); },
        [](int, std::vector<int> v) -> int { return v.empty() ? 0 : v[0]; }
    );
    EXPECT_NE(action.input(), nullptr);
}

// ─── try_push ────────────────────────────────────────────────────────────────

TEST(ScatterGatherAction, TryPushSuccessWhenCapacityAvailable) {
    ScatterGatherAction<int, int, int, int> action(
        [](int n) -> std::vector<int> { return {n}; },
        [](int x, ActionEnv) -> Task<Result<int>> { co_return Result<int>(x); },
        [](int, std::vector<int> v) -> int { return v.empty() ? 0 : v[0]; },
        {.max_parallel = 4, .channel_cap = 8}
    );
    EXPECT_TRUE(action.try_push(1));
    EXPECT_TRUE(action.try_push(2));
}

TEST(ScatterGatherAction, TryPushReturnsFalseWhenFull) {
    ScatterGatherAction<int, int, int, int> action(
        [](int n) -> std::vector<int> { return {n}; },
        [](int x, ActionEnv) -> Task<Result<int>> { co_return Result<int>(x); },
        [](int, std::vector<int> v) -> int { return v.empty() ? 0 : v[0]; },
        {.max_parallel = 2, .channel_cap = 2}
    );
    EXPECT_TRUE(action.try_push(1));
    EXPECT_TRUE(action.try_push(2));
    EXPECT_FALSE(action.try_push(3));  // full
}

TEST(ScatterGatherAction, InputChannelSizeApproxMatchesPushCount) {
    ScatterGatherAction<int, int, int, int> action(
        [](int n) -> std::vector<int> { return {n}; },
        [](int x, ActionEnv) -> Task<Result<int>> { co_return Result<int>(x); },
        [](int, std::vector<int> v) -> int { return v.empty() ? 0 : v[0]; },
        {.max_parallel = 4, .channel_cap = 16}
    );
    for (int i = 0; i < 5; ++i)
        action.try_push(i);
    EXPECT_EQ(action.input()->size_approx(), 5u);
}

// ─── Config field validation ──────────────────────────────────────────────────

TEST(ScatterGatherAction, DefaultMaxParallelIs8) {
    ScatterGatherAction<int, int, int, int>::Config cfg;
    EXPECT_EQ(cfg.max_parallel, 8u);
}

TEST(ScatterGatherAction, DefaultChannelCapIs256) {
    ScatterGatherAction<int, int, int, int>::Config cfg;
    EXPECT_EQ(cfg.channel_cap, 256u);
}

TEST(ScatterGatherAction, CustomConfig) {
    ScatterGatherAction<int, int, int, int>::Config cfg{
        .max_parallel = 3,
        .channel_cap  = 64,
        .registry     = nullptr
    };
    EXPECT_EQ(cfg.max_parallel, 3u);
    EXPECT_EQ(cfg.channel_cap, 64u);
}

// ─── ScatterGather function semantics validation ──────────────────────────────

TEST(ScatterGatherAction, ScatterFnCanReturnEmptyVector) {
    // scatter → empty → gather called with empty SubOut
    std::vector<int> gathered;
    ScatterGatherAction<int, int, int, int> action(
        [](int) -> std::vector<int> { return {}; },  // empty scatter
        [](int x, ActionEnv) -> Task<Result<int>> { co_return Result<int>(x); },
        [&](int orig, std::vector<int> v) -> int {
            gathered = v;
            return orig;
        },
        {.max_parallel = 4, .channel_cap = 4}
    );
    // Not running worker_loop directly without a Reactor, so verify at channel level only
    EXPECT_TRUE(action.try_push(42));
}

// ─── DebounceAction Config ─────────────────────────────────────────────────

TEST(DebounceAction, DefaultConfig) {
    DebounceAction<int>::Config cfg;
    EXPECT_EQ(cfg.gap.count(), 100);
    EXPECT_EQ(cfg.channel_cap, 256u);
}

TEST(DebounceAction, ConstructionWithCustomConfig) {
    using namespace std::chrono_literals;
    DebounceAction<int> action({.gap = 50ms, .channel_cap = 32});
    EXPECT_NE(action.input(), nullptr);
}

TEST(DebounceAction, TryPushAndInput) {
    using namespace std::chrono_literals;
    DebounceAction<int> action({.gap = 10ms, .channel_cap = 8});
    EXPECT_TRUE(action.try_push(1));
    EXPECT_TRUE(action.try_push(2));
    EXPECT_EQ(action.input()->size_approx(), 2u);
}

// ─── ThrottleAction Config ────────────────────────────────────────────────

TEST(ThrottleAction, DefaultConfig) {
    ThrottleAction<int>::Config cfg;
    EXPECT_GT(cfg.rate_per_sec, 0u);
    EXPECT_GE(cfg.burst, 1u);
}

TEST(ThrottleAction, ConstructionWithCustomConfig) {
    ThrottleAction<int> action({.rate_per_sec = 100u, .burst = 10u, .channel_cap = 32u});
    EXPECT_NE(action.input(), nullptr);
}

TEST(ThrottleAction, TryPushAndInput) {
    ThrottleAction<int> action({.rate_per_sec = 1000u, .burst = 5u, .channel_cap = 8u});
    EXPECT_TRUE(action.try_push(1));
    EXPECT_EQ(action.input()->size_approx(), 1u);
}
