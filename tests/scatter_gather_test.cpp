/**
 * @file scatter_gather_test.cpp
 * @brief ScatterGatherAction 단위 테스트.
 *
 * 커버리지:
 * - ScatterGatherAction 생성 (scatter/process/gather λ)
 * - try_push: 성공 / 용량 초과 false
 * - input() 채널 접근
 * - max_parallel 설정 반영 (Config::max_parallel)
 * - DebounceAction Config: gap / channel_cap
 * - ThrottleAction Config: rate_per_sec / burst / channel_cap
 */

#include <qbuem/pipeline/event_actions.hpp>
#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <sstream>

using namespace qbuem;

// ─── 헬퍼 ────────────────────────────────────────────────────────────────────

static std::vector<int> split_ints(int n, int count) {
    std::vector<int> v;
    for (int i = 0; i < count; ++i) v.push_back(n + i);
    return v;
}

// ─── ScatterGatherAction 생성 검증 ────────────────────────────────────────────

TEST(ScatterGatherAction, ConstructionDoesNotThrow) {
    EXPECT_NO_THROW({
        ScatterGatherAction<int, int, int, int> action(
            [](int n) -> std::vector<int> { return {n}; },
            [](int x, ActionEnv) -> Task<Result<int>> { co_return Result<int>(x); },
            [](int /*orig*/, std::vector<int> v) -> int {
                return v.empty() ? 0 : v[0];
            },
            {.max_parallel = 4, .channel_cap = 32}
        );
    });
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

// ─── Config 필드 검증 ─────────────────────────────────────────────────────────

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

// ─── ScatterGather 함수 의미론 검증 ──────────────────────────────────────────

TEST(ScatterGatherAction, ScatterFnCanReturnEmptyVector) {
    // scatter → empty → gather 호출, 빈 SubOut 전달
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
    // Reactor 없이 worker_loop를 직접 실행하지 않으므로 채널 레벨만 검증
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
