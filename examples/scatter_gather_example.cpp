/**
 * @file scatter_gather_example.cpp
 * @brief ScatterGatherAction / DebounceAction / ThrottleAction 사용 예시.
 *
 * 커버리지:
 * - ScatterGatherAction Config 설정 (max_parallel, channel_cap)
 * - try_push 성공/실패 (백프레셔)
 * - input() 채널 접근 + size_approx
 * - scatter / process / gather 람다 의미론
 * - DebounceAction Config + try_push
 * - ThrottleAction Config + try_push
 */

#include <qbuem/pipeline/event_actions.hpp>

#include <cassert>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace qbuem;

// ─── 1. ScatterGatherAction 설정 + try_push ──────────────────────────────────

static void demo_scatter_gather() {
    std::puts("[ScatterGatherAction] config + try_push demo");

    // 문자열을 단어들로 scatter → 각 단어 길이 process → max 길이 gather
    auto scatter_fn = [](std::string sentence) -> std::vector<std::string> {
        std::vector<std::string> words;
        std::istringstream iss(sentence);
        std::string w;
        while (iss >> w) words.push_back(w);
        return words;
    };

    auto process_fn = [](std::string word, ActionEnv) -> Task<Result<size_t>> {
        co_return Result<size_t>(word.size());
    };

    auto gather_fn = [](std::string /*orig*/,
                        std::vector<size_t> lengths) -> size_t {
        size_t mx = 0;
        for (auto l : lengths) if (l > mx) mx = l;
        return mx;
    };

    // max_parallel=3 으로 배치 실행 제한 설정
    ScatterGatherAction<std::string, std::string, size_t, size_t> action(
        scatter_fn, process_fn, gather_fn,
        {.max_parallel = 3, .channel_cap = 16}
    );

    assert(action.input() != nullptr);

    // try_push 성공
    assert(action.try_push("hello world foo bar"));
    assert(action.try_push("the quick brown fox"));
    assert(action.input()->size_approx() == 2u);
    std::printf("[ScatterGatherAction] input queue size=%zu\n",
                action.input()->size_approx());

    // 채널 가득 찰 때 false
    ScatterGatherAction<int, int, int, int> small(
        [](int n) -> std::vector<int> { return {n}; },
        [](int x, ActionEnv) -> Task<Result<int>> { co_return Result<int>(x); },
        [](int, std::vector<int> v) -> int { return v.empty() ? 0 : v[0]; },
        {.max_parallel = 2, .channel_cap = 2}
    );
    assert(small.try_push(1));
    assert(small.try_push(2));
    bool full_blocked = !small.try_push(3);
    assert(full_blocked);
    std::puts("[ScatterGatherAction] backpressure: OK");

    std::puts("[ScatterGatherAction] OK");
}

// ─── 2. DebounceAction ────────────────────────────────────────────────────────

static void demo_debounce() {
    using namespace std::chrono_literals;
    std::puts("[DebounceAction] config + try_push demo");

    DebounceAction<int> action({.gap = 50ms, .channel_cap = 32});
    assert(action.input() != nullptr);

    for (int i = 0; i < 5; ++i)
        action.try_push(i);

    assert(action.input()->size_approx() == 5u);
    std::printf("[DebounceAction] input=%zu events queued\n",
                action.input()->size_approx());
    std::puts("[DebounceAction] OK");
}

// ─── 3. ThrottleAction ──────────────────────────────────────────────────────

static void demo_throttle() {
    std::puts("[ThrottleAction] config + try_push demo");

    // 초당 1000개, 버스트 10개
    ThrottleAction<int> action({.rate_per_sec = 1000u, .burst = 10u, .channel_cap = 64u});
    assert(action.input() != nullptr);

    for (int i = 0; i < 8; ++i)
        action.try_push(i);

    assert(action.input()->size_approx() == 8u);
    std::printf("[ThrottleAction] input=%zu items queued\n",
                action.input()->size_approx());
    std::puts("[ThrottleAction] OK");
}

int main() {
    demo_scatter_gather();
    demo_debounce();
    demo_throttle();
    std::puts("scatter_gather_example: ALL OK");
    return 0;
}
