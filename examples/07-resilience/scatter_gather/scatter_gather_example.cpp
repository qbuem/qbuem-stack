/**
 * @file scatter_gather_example.cpp
 * @brief ScatterGatherAction / DebounceAction / ThrottleAction usage example.
 *
 * Coverage:
 * - ScatterGatherAction Config setup (max_parallel, channel_cap)
 * - try_push success/failure (backpressure)
 * - input() channel access + size_approx
 * - scatter / process / gather lambda semantics
 * - DebounceAction Config + try_push
 * - ThrottleAction Config + try_push
 */

#include <qbuem/pipeline/event_actions.hpp>
#include <qbuem/compat/print.hpp>

#include <cassert>
#include <sstream>
#include <string>
#include <vector>

using namespace qbuem;

// ─── 1. ScatterGatherAction config + try_push ─────────────────────────────────

static void demo_scatter_gather() {
    std::println("[ScatterGatherAction] config + try_push demo");

    // Scatter a sentence into words → process each word length → gather max length
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

    // Limit batch execution to max_parallel=3
    ScatterGatherAction<std::string, std::string, size_t, size_t> action(
        scatter_fn, process_fn, gather_fn,
        {.max_parallel = 3, .channel_cap = 16}
    );

    assert(action.input() != nullptr);

    // Successful try_push
    assert(action.try_push("hello world foo bar"));
    assert(action.try_push("the quick brown fox"));
    assert(action.input()->size_approx() == 2u);
    std::println("[ScatterGatherAction] input queue size={}",
                action.input()->size_approx());

    // Returns false when channel is full
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
    std::println("[ScatterGatherAction] backpressure: OK");

    std::println("[ScatterGatherAction] OK");
}

// ─── 2. DebounceAction ────────────────────────────────────────────────────────

static void demo_debounce() {
    using namespace std::chrono_literals;
    std::println("[DebounceAction] config + try_push demo");

    DebounceAction<int> action({.gap = 50ms, .channel_cap = 32});
    assert(action.input() != nullptr);

    for (int i = 0; i < 5; ++i)
        action.try_push(i);

    assert(action.input()->size_approx() == 5u);
    std::println("[DebounceAction] input={} events queued",
                action.input()->size_approx());
    std::println("[DebounceAction] OK");
}

// ─── 3. ThrottleAction ───────────────────────────────────────────────────────

static void demo_throttle() {
    std::println("[ThrottleAction] config + try_push demo");

    // 1000 items/second, burst of 10
    ThrottleAction<int> action({.rate_per_sec = 1000u, .burst = 10u, .channel_cap = 64u});
    assert(action.input() != nullptr);

    for (int i = 0; i < 8; ++i)
        action.try_push(i);

    assert(action.input()->size_approx() == 8u);
    std::println("[ThrottleAction] input={} items queued",
                action.input()->size_approx());
    std::println("[ThrottleAction] OK");
}

int main() {
    demo_scatter_gather();
    demo_debounce();
    demo_throttle();
    std::println("scatter_gather_example: ALL OK");
    return 0;
}
