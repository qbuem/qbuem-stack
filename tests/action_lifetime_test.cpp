/**
 * @file action_lifetime_test.cpp
 * @brief Teardown-safety tests for the action wrappers.
 *
 * Each action (BatchAction, DebounceAction, ThrottleAction, ScatterGatherAction,
 * WindowedAction) spawns worker coroutines on a Dispatcher. These tests destroy
 * the action BEFORE stopping the reactor — i.e. while its workers may still be
 * running — to prove a worker never dereferences the freed action. Each worker
 * holds a shared_ptr to the action's State, so destroying the handle keeps the
 * State alive until the workers drain. ASan/TSan would flag a use-after-free
 * otherwise; reaching the end of each test clean is the assertion.
 */
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/batch_action.hpp>
#include <qbuem/pipeline/event_actions.hpp>
#include <qbuem/pipeline/windowed_action.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

namespace {

// Runs a Dispatcher on its own thread; stops + joins on shutdown(). Declared
// first in each test so it outlives the action locals — but the whole point of
// these tests is that the action is destroyed (inner scope) while this is still
// running, so shutdown() happens AFTER the action is already gone.
struct RunGuard {
    Dispatcher   dispatcher;
    std::jthread thread;
    bool         stopped_ = false;
    explicit RunGuard(size_t n = 2) : dispatcher(n) {
        thread = std::jthread([this] { dispatcher.run(); });
    }
    void shutdown() {
        if (stopped_) return;
        stopped_ = true;
        dispatcher.stop();
        if (thread.joinable()) thread.join();
    }
    ~RunGuard() { shutdown(); }

    template <typename F>
    static Task<void> run_coro(F f, std::shared_ptr<std::atomic<bool>> done) {
        co_await f();
        done->store(true, std::memory_order_release);
    }
    template <typename F>
    void run_and_wait(F&& f, std::chrono::milliseconds timeout = 5s) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        dispatcher.spawn(run_coro(std::forward<F>(f), done));
        auto dl = std::chrono::steady_clock::now() + timeout;
        while (!done->load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < dl)
            std::this_thread::sleep_for(1ms);
    }
};

} // namespace

// ─── BatchAction ─────────────────────────────────────────────────────────────
TEST(ActionLifetime, BatchDestroyWhileRunning) {
    RunGuard guard;
    {
        BatchAction<int, int> action(
            [](std::vector<int> v, ActionEnv) -> Task<Result<std::vector<int>>> {
                co_return v;
            },
            {.max_batch_size = 8, .max_wait_ms = 5, .workers = 2, .channel_cap = 64});
        action.start(guard.dispatcher);
        for (int i = 0; i < 16; ++i) action.try_push(i);
        // action destroyed here, workers may still be running on the reactor thread.
    }
    guard.run_and_wait([]() -> Task<void> { co_return; });
    guard.shutdown();
    SUCCEED();
}

// ─── DebounceAction ──────────────────────────────────────────────────────────
TEST(ActionLifetime, DebounceDestroyWhileRunning) {
    RunGuard guard;
    {
        DebounceAction<int> action({.gap = 50ms, .channel_cap = 64});
        action.start(guard.dispatcher);
        for (int i = 0; i < 16; ++i) action.try_push(i);
    }
    guard.run_and_wait([]() -> Task<void> { co_return; });
    guard.shutdown();
    SUCCEED();
}

// ─── ThrottleAction ──────────────────────────────────────────────────────────
TEST(ActionLifetime, ThrottleDestroyWhileRunning) {
    RunGuard guard;
    {
        ThrottleAction<int> action({.rate_per_sec = 1000u, .burst = 8u, .channel_cap = 64u});
        action.start(guard.dispatcher);
        for (int i = 0; i < 16; ++i) action.try_push(i);
    }
    guard.run_and_wait([]() -> Task<void> { co_return; });
    guard.shutdown();
    SUCCEED();
}

// ─── ScatterGatherAction ─────────────────────────────────────────────────────
TEST(ActionLifetime, ScatterGatherDestroyWhileRunning) {
    RunGuard guard;
    {
        ScatterGatherAction<int, int, int, int> action(
            [](int n) -> std::vector<int> { return {n, n + 1}; },
            [](int x, ActionEnv) -> Task<Result<int>> { co_return Result<int>(x); },
            [](int, std::vector<int> v) -> int { return v.empty() ? 0 : v[0]; },
            {.max_parallel = 4, .channel_cap = 64});
        action.start(guard.dispatcher);
        for (int i = 0; i < 16; ++i) action.try_push(i);
    }
    guard.run_and_wait([]() -> Task<void> { co_return; });
    guard.shutdown();
    SUCCEED();
}

// ─── WindowedAction ──────────────────────────────────────────────────────────
TEST(ActionLifetime, WindowedDestroyWhileRunning) {
    RunGuard guard;
    {
        using WA = WindowedAction<int, std::string, int64_t, int64_t>;
        WA::Config cfg;
        cfg.type     = WindowType::Tumbling;
        cfg.size     = milliseconds{1000};
        cfg.key_fn   = [](const int&) -> std::string { return "k"; };
        cfg.acc_fn   = [](int64_t& acc, const int& v) { acc += v; };
        cfg.emit_fn  = [](std::string, int64_t acc,
                          std::chrono::system_clock::time_point) -> int64_t { return acc; };
        cfg.init_acc = 0;
        WA action(std::move(cfg));
        action.start(guard.dispatcher);
        for (int i = 0; i < 16; ++i) action.try_push(i);
    }
    guard.run_and_wait([]() -> Task<void> { co_return; });
    guard.shutdown();
    SUCCEED();
}
