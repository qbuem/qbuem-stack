/**
 * @file pipeline_topology_test.cpp
 * @brief Cross-topology pipeline coverage: StaticPipeline (linear), DynamicPipeline
 *        (hot-swap / remove stage), PipelineGraph (split = fan-out, merge = fan-in),
 *        and composition (two pipelines merged into one consumer).
 *
 * Every case drives data through and ASSERTS the result — this is the
 * "build a pipeline and run it" (ffmpeg-style) contract under test.
 */
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/pipeline/pipeline_graph.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

namespace {

// Cooperative yield (does not block the reactor thread).
struct Yield {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const noexcept {
        if (auto* r = Reactor::current()) r->post([h]() mutable { h.resume(); });
        else h.resume();
    }
    void await_resume() const noexcept {}
};

struct RunGuard {
    Dispatcher   dispatcher;
    std::jthread thread;
    bool         stopped_ = false;
    explicit RunGuard(size_t n = 2) : dispatcher(n) {
        thread = std::jthread([this] { dispatcher.run(); });
    }
    // Stop the reactor(s) and join the loop thread. Idempotent. Call this BEFORE
    // a pipeline / graph the worker coroutines reference is destroyed: RunGuard
    // is declared first so its destructor runs LAST — after those locals — and a
    // still-running worker would otherwise race the destructor freeing channels
    // (TSan data race / heap-use-after-free).
    void shutdown() {
        if (stopped_) return;
        stopped_ = true;
        dispatcher.stop();
        if (thread.joinable()) thread.join();
    }
    ~RunGuard() { shutdown(); }

    // Free-function coroutine (frame owns f) — never spawn a temporary lambda.
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
        ASSERT_TRUE(done->load()) << "run_and_wait timed out";
    }
};

template <typename Ch>
Task<void> wait_count(std::shared_ptr<Ch> out, std::vector<int>& sink, int n,
                      std::chrono::milliseconds max = 2s) {
    auto dl = std::chrono::steady_clock::now() + max;
    while (static_cast<int>(sink.size()) < n &&
           std::chrono::steady_clock::now() < dl) {
        auto item = out->try_recv();
        if (item) sink.push_back(item->value);
        else co_await Yield{};
    }
    co_return;
}

} // namespace

// ─── 1. StaticPipeline — linear chain (×2 then +1) ──────────────────────────
TEST(PipelineTopology, StaticLinearChain) {
    RunGuard guard;
    auto pipeline = PipelineBuilder<int, int>{}
        .add<int>([](int v, ActionEnv) -> Task<Result<int>> { co_return v * 2; })
        .add<int>([](int v, ActionEnv) -> Task<Result<int>> { co_return v + 1; })
        .build();
    pipeline.start(guard.dispatcher);
    auto out = pipeline.output();

    std::vector<int> got;
    guard.run_and_wait([&]() -> Task<void> {
        co_await pipeline.push(10);  // → 21
        co_await pipeline.push(20);  // → 41
        co_await wait_count(out, got, 2);
    });
    pipeline.stop();
    guard.run_and_wait([]() -> Task<void> { co_return; });
    guard.shutdown(); // stop reactor before pipeline destructs (TSan: worker lifetime race)

    ASSERT_EQ(got.size(), 2u);
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got[0], 21);
    EXPECT_EQ(got[1], 41);
}

// ─── 2. DynamicPipeline — run, hot-swap a stage, remove a stage ──────────────
TEST(PipelineTopology, DynamicHotSwapAndRemove) {
    RunGuard guard;
    DynamicPipeline<int> dp;
    dp.add_stage("scale", [](int v, ActionEnv) -> Task<Result<int>> { co_return v * 2; });
    dp.add_stage("bias",  [](int v, ActionEnv) -> Task<Result<int>> { co_return v + 1; });
    dp.start(guard.dispatcher);
    auto out = dp.output();

    // Initial: ×2 then +1  → 5 → 11
    std::vector<int> got;
    guard.run_and_wait([&]() -> Task<void> {
        co_await dp.push(5);
        co_await wait_count(out, got, 1);
    });
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], 11);

    // Hot-swap "scale" to ×10 (live) → 5 → ×10=50 → +1=51.
    // Re-fetch output() after reconfig: rewire may rebuild channels.
    bool swapped = dp.hot_swap("scale",
        [](int v, ActionEnv) -> Task<Result<int>> { co_return v * 10; });
    EXPECT_TRUE(swapped);
    guard.run_and_wait([]() -> Task<void> { co_return; });  // let reconfig settle
    out = dp.output();
    got.clear();
    guard.run_and_wait([&]() -> Task<void> {
        co_await dp.push(5);
        co_await wait_count(out, got, 1);
    });
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], 51);

    // Remove "bias" → only ×10 remains → 5 → 50
    bool removed = dp.remove_stage("bias");
    EXPECT_TRUE(removed);
    guard.run_and_wait([]() -> Task<void> { co_return; });  // let reconfig settle
    out = dp.output();
    got.clear();
    guard.run_and_wait([&]() -> Task<void> {
        co_await dp.push(5);
        co_await wait_count(out, got, 1);
    });
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], 50);

    dp.stop();
    guard.run_and_wait([]() -> Task<void> { co_return; });
    guard.shutdown(); // stop reactor before dp destructs (TSan: worker lifetime race)
}

// ─── 3. PipelineGraph — split (fan-out) + merge (fan-in) ─────────────────────
TEST(PipelineTopology, GraphSplitAndMerge) {
    RunGuard guard;
    PipelineGraph<int> graph;
    graph
        .node("src",   [](int v, ActionEnv) -> Task<Result<int>> { co_return v; }, 1, 64)
        .node("plus",  [](int v, ActionEnv) -> Task<Result<int>> { co_return v + 1000; }, 1, 64)
        .node("minus", [](int v, ActionEnv) -> Task<Result<int>> { co_return v - 1000; }, 1, 64)
        // split: src fans out to BOTH branches
        .edge("src", "plus")
        .edge("src", "minus")
        .source("src")
        // merge: both branches fan in to the graph output
        .sink("plus")
        .sink("minus");

    graph.start(guard.dispatcher);
    auto out = graph.output();

    std::vector<int> got;
    guard.run_and_wait([&]() -> Task<void> {
        co_await graph.push(5);              // → 1005 (plus) AND -995 (minus)
        co_await wait_count(out, got, 2);    // both branches produce one item
    });
    graph.stop();
    guard.run_and_wait([]() -> Task<void> { co_return; });
    guard.shutdown(); // stop reactor before graph destructs (TSan: fanout_worker lifetime race)

    ASSERT_EQ(got.size(), 2u);
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got[0], -995);   // minus branch
    EXPECT_EQ(got[1], 1005);   // plus branch
}

// ─── 3c. PipelineGraph — destroy while workers are in flight (lifetime) ──────
// Mirror of DynamicDestroyWhileRunningIsSafe for the graph: workers (node +
// fan-out) route via per-node snapshots, never the parent graph's nodes_ map or
// registry_, so the graph can be destroyed before the reactor stops. ASan/TSan
// would flag a use-after-free otherwise.
TEST(PipelineTopology, GraphDestroyWhileRunningIsSafe) {
    RunGuard guard;
    {
        PipelineGraph<int> graph;
        graph
            .node("src",   [](int v, ActionEnv) -> Task<Result<int>> { co_return v; }, 1, 64)
            .node("plus",  [](int v, ActionEnv) -> Task<Result<int>> { co_return v + 1; }, 2, 64)
            .node("minus", [](int v, ActionEnv) -> Task<Result<int>> { co_return v - 1; }, 2, 64)
            .edge("src", "plus")
            .edge("src", "minus")
            .source("src")
            .sink("plus")
            .sink("minus");
        graph.start(guard.dispatcher);
        // Fire items but do NOT drain/stop — leave node + fan-out workers mid-flight.
        guard.run_and_wait([&]() -> Task<void> {
            for (int i = 0; i < 16; ++i) co_await graph.push(i);
        });
        // graph goes out of scope here → ~PipelineGraph() closes channels while the
        // reactor thread may still be executing its workers.
    }
    guard.run_and_wait([]() -> Task<void> { co_return; });  // let workers drain
    guard.shutdown();
    SUCCEED();  // reaching here clean under ASan/TSan means no teardown UAF
}

// ─── 4. Composition — two StaticPipelines merged into one consumer ───────────
TEST(PipelineTopology, MergeTwoPipelinesIntoOneConsumer) {
    RunGuard guard;
    auto p1 = PipelineBuilder<int, int>{}
        .add<int>([](int v, ActionEnv) -> Task<Result<int>> { co_return v + 1; })
        .build();
    auto p2 = PipelineBuilder<int, int>{}
        .add<int>([](int v, ActionEnv) -> Task<Result<int>> { co_return v + 100; })
        .build();
    p1.start(guard.dispatcher);
    p2.start(guard.dispatcher);
    auto o1 = p1.output();
    auto o2 = p2.output();

    std::vector<int> got;
    guard.run_and_wait([&]() -> Task<void> {
        co_await p1.push(1);   // → 2
        co_await p2.push(1);   // → 101
        // merge: drain BOTH pipeline outputs into one consumer
        auto dl = std::chrono::steady_clock::now() + 2s;
        while (got.size() < 2 && std::chrono::steady_clock::now() < dl) {
            if (auto a = o1->try_recv()) got.push_back(a->value);
            else if (auto b = o2->try_recv()) got.push_back(b->value);
            else co_await Yield{};
        }
    });
    p1.stop(); p2.stop();
    guard.run_and_wait([]() -> Task<void> { co_return; });
    guard.shutdown(); // stop reactor before p1/p2 destruct (TSan: worker lifetime race)

    ASSERT_EQ(got.size(), 2u);
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got[0], 2);
    EXPECT_EQ(got[1], 101);
}
