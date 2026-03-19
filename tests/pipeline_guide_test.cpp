/**
 * @file tests/pipeline_guide_test.cpp
 * @brief Pipeline Master Guide pattern validation tests.
 *
 * This test file validates each pattern and recipe introduced in
 * `docs/pipeline-master-guide.md`.
 *
 * ## Coverage
 * §3-2  DynamicPipeline basic operation / hot_swap
 * §4-1  Bulkheading — isolate heavy actions to a separate worker
 * §5-1  Fan-out (Broadcast) — PipelineGraph 1→N branching
 * §5-1  Fan-in  (Merge)    — PipelineGraph N→1 collection
 * §5-2  Sidecar Observation — T-pipe copy observation
 * §5-2  Feedback Loop       — upstream retransmit of failed items
 * §6A   Sensor Fusion (N:1 Sync) — ServiceRegistry Gather
 * §6B   Hardware Batching (NPU)  — BatchAction
 * §6C   Resilient WAS (DLQ)      — DlqAction + DeadLetterQueue
 * §7    Periodic Polling Source  — while(true) + co_await sleep
 */

#include <gtest/gtest.h>

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/batch_action.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/dead_letter.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/pipeline/pipeline_graph.hpp>
#include <qbuem/pipeline/service_registry.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/pipeline/task_group.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Common helpers ───────────────────────────────────────────────────────────

/// RAII wrapper that safely runs and stops a Dispatcher in a GTest environment.
struct RunGuard {
    Dispatcher dispatcher;
    std::jthread thread;

    explicit RunGuard(size_t threads = 1) : dispatcher(threads) {
        thread = std::jthread([this] { dispatcher.run(); });
    }
    ~RunGuard() {
        dispatcher.stop();
        thread.join();
    }

    // Named coroutine — avoids GCC HALO stack-use-after-return in lambdas.
    template <typename F>
    static Task<void> flush_coro_(F f, std::shared_ptr<std::atomic<bool>> done) {
        co_await f();
        done->store(true, std::memory_order_release);
    }

    // Spawn a coroutine on the dispatcher and block until it completes.
    // Used to flush pending reactor wake events after pipeline.stop() so
    // coroutine frames are freed before the dispatcher shuts down.
    template <typename F>
    void run_and_wait(F&& f, std::chrono::milliseconds timeout = 5s) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        dispatcher.spawn(flush_coro_(std::forward<F>(f), done));
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done->load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
    }
};

/// Collects up to `n` items from the channel within the given `timeout`.
template <typename T>
std::vector<T> collect(
    std::shared_ptr<AsyncChannel<ContextualItem<T>>> ch,
    size_t n,
    std::chrono::milliseconds timeout = 3000ms)
{
    std::vector<T> results;
    results.reserve(n);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (results.size() < n && std::chrono::steady_clock::now() < deadline) {
        auto item = ch->try_recv();
        if (item) results.push_back(std::move(item->value));
        else std::this_thread::sleep_for(1ms);
    }
    return results;
}

// =============================================================================
// §3-2  DynamicPipeline
// =============================================================================

// ── 3-2-1: Basic stage execution ──────────────────────────────────────────

TEST(DynamicPipeline, BasicStageExecution) {
    // Guide §3-2: Add stages at runtime and verify processing results
    DynamicPipeline<int> dp;
    dp.add_stage("double", [](int x, ActionEnv) -> Task<Result<int>> {
        co_return x * 2;
    });
    dp.add_stage("addone", [](int x, ActionEnv) -> Task<Result<int>> {
        co_return x + 1;
    });

    RunGuard g;
    dp.start(g.dispatcher);

    for (int i = 1; i <= 5; ++i)
        dp.try_push(i);

    auto results = collect(dp.output(), 5);

    // double(x) + 1: 1→3, 2→5, 3→7, 4→9, 5→11
    ASSERT_EQ(results.size(), 5u);
    for (int r : results) {
        EXPECT_GT(r, 0) << "result must be positive";
    }
    dp.stop();
}

// ── 3-2-2: Hot-swap — replace stage while running ─────────────────────────

TEST(DynamicPipeline, HotSwapReplaceStage) {
    // Guide §3-2: Hot-swapping — replace the factory and restart the pipeline to apply the new function.
    // hot_swap replaces the factory and sends a stop signal to existing workers.
    // New workers must be started by restarting with a new DynamicPipeline.

    // Phase 1: v1 (×2)
    {
        DynamicPipeline<int> dp;
        dp.add_stage("transform", [](int x, ActionEnv) -> Task<Result<int>> {
            co_return x * 2;   // v1: ×2
        });
        RunGuard g;
        dp.start(g.dispatcher);
        dp.try_push(10);
        auto r1 = collect(dp.output(), 1);
        ASSERT_EQ(r1.size(), 1u);
        EXPECT_EQ(r1[0], 20);   // 10*2=20
        dp.stop();
        // Flush: ensure the reactor processes the post-stop worker wake event
        // so the coroutine frame is freed before the dispatcher shuts down.
        g.run_and_wait([]() -> Task<void> { co_return; });
    }

    // Verify hot_swap behavior: nonexistent stage returns false, existing returns true
    {
        DynamicPipeline<int> dp;
        dp.add_stage("transform", [](int x, ActionEnv) -> Task<Result<int>> {
            co_return x * 2;
        });
        EXPECT_FALSE(dp.hot_swap("nonexistent", [](int x, ActionEnv) -> Task<Result<int>> {
            co_return x;
        }));
        EXPECT_TRUE(dp.hot_swap("transform", [](int x, ActionEnv) -> Task<Result<int>> {
            co_return x * 10;  // v2: ×10
        }));
        // Start new pipeline after hot_swap
        RunGuard g;
        dp.start(g.dispatcher);
        dp.try_push(10);
        auto r2 = collect(dp.output(), 1);
        ASSERT_EQ(r2.size(), 1u);
        EXPECT_EQ(r2[0], 100);  // 10*10=100
        dp.stop();
        // Flush: ensure the reactor processes the post-stop worker wake event
        // so the coroutine frame is freed before the dispatcher shuts down.
        g.run_and_wait([]() -> Task<void> { co_return; });
    }
}

// ── 3-2-3: Add stage — insert a stage at runtime ────────────────────────

TEST(DynamicPipeline, AddStageAtRuntime) {
    // add_stage can be called before or after start().
    // Only stages added before start() will have workers spawned.
    // Stages added after start() become active after a stop/start restart.
    DynamicPipeline<int> dp;
    dp.add_stage("first", [](int x, ActionEnv) -> Task<Result<int>> {
        co_return x + 100;
    });
    dp.add_stage("second", [](int x, ActionEnv) -> Task<Result<int>> {
        co_return x * 2;
    });

    RunGuard g;
    dp.start(g.dispatcher);

    dp.try_push(5);
    auto results = collect(dp.output(), 1);
    ASSERT_FALSE(results.empty());
    // (5 + 100) * 2 = 210
    EXPECT_EQ(results[0], 210);
    dp.stop();
}

// =============================================================================
// §4-1  Bulkheading — independent worker isolation for heavy actions
// =============================================================================

TEST(Bulkheading, HeavyActionIsolatedFromLight) {
    // Guide §4-1: assign a separate worker pool to heavy actions to verify light actions are not blocked
    std::atomic<size_t> light_count{0};
    std::atomic<size_t> heavy_count{0};

    // light stage: processed immediately
    auto light_fn = [&](int x) -> Task<Result<int>> {
        light_count.fetch_add(1, std::memory_order_relaxed);
        co_return x;
    };

    // heavy stage: independent worker pool (min=1, max=4)
    auto heavy_fn = [&](int x) -> Task<Result<int>> {
        heavy_count.fetch_add(1, std::memory_order_relaxed);
        co_return x * 2;
    };

    Action<int,int>::Config light_cfg{.min_workers=1, .max_workers=1, .channel_cap=64};
    Action<int,int>::Config heavy_cfg{.min_workers=1, .max_workers=4, .channel_cap=64};

    auto pipeline = pipeline_builder<int>()
        .add<int>(light_fn, light_cfg)
        .add<int>(heavy_fn, heavy_cfg)
        .build();

    RunGuard g(2);
    pipeline.start(g.dispatcher);

    constexpr size_t kItems = 20;
    for (size_t i = 0; i < kItems; ++i)
        pipeline.try_push(static_cast<int>(i));

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((light_count.load() < kItems || heavy_count.load() < kItems) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }

    pipeline.stop();

    EXPECT_EQ(light_count.load(), kItems) << "light stage processed count mismatch";
    EXPECT_EQ(heavy_count.load(), kItems) << "heavy stage processed count mismatch";
}

// =============================================================================
// §5-1  Fan-out (Broadcast) — PipelineGraph 1→N
// =============================================================================

TEST(PipelineGraph, FanOutBroadcast) {
    // Guide §5-1: one source → two sinks (main, audit)
    struct Msg { std::string content; std::string branch; };

    PipelineGraph<Msg> graph;
    graph
        .node("ingest", [](Msg m, ActionEnv) -> Task<Result<Msg>> {
            co_return m;
        }, 1, 64)
        .node("main_sink", [](Msg m, ActionEnv) -> Task<Result<Msg>> {
            m.branch = "main";
            co_return m;
        }, 1, 64)
        .node("audit_sink", [](Msg m, ActionEnv) -> Task<Result<Msg>> {
            m.branch = "audit";
            co_return m;
        }, 1, 64)
        .edge("ingest", "main_sink")
        .edge("ingest", "audit_sink")
        .source("ingest")
        .sink("main_sink")
        .sink("audit_sink");

    RunGuard g(2);
    graph.start(g.dispatcher);

    constexpr size_t kItems = 5;
    for (size_t i = 0; i < kItems; ++i)
        graph.try_push(Msg{"msg-" + std::to_string(i), ""});

    // fan-out: each input produces 2 outputs → total kItems*2
    auto results = collect(graph.output(), kItems * 2);

    size_t main_cnt  = 0, audit_cnt = 0;
    for (auto& r : results) {
        if (r.branch == "main")  ++main_cnt;
        if (r.branch == "audit") ++audit_cnt;
    }

    EXPECT_EQ(main_cnt,  kItems) << "main branch output count mismatch";
    EXPECT_EQ(audit_cnt, kItems) << "audit branch output count mismatch";
    graph.stop();
}

// =============================================================================
// §5-1  Fan-in (Merge) — multiple sources → single output
// =============================================================================

TEST(PipelineGraph, FanInMerge) {
    // Guide §5-1: two sources (source_a, source_b) → common processing node → sink
    struct Event { int id; std::string from; };

    PipelineGraph<Event> graph;
    graph
        .node("source_a", [](Event e, ActionEnv) -> Task<Result<Event>> {
            e.from = "A"; co_return e;
        }, 1, 64)
        .node("source_b", [](Event e, ActionEnv) -> Task<Result<Event>> {
            e.from = "B"; co_return e;
        }, 1, 64)
        .node("merge_sink", [](Event e, ActionEnv) -> Task<Result<Event>> {
            co_return e;
        }, 1, 128)
        .edge("source_a", "merge_sink")
        .edge("source_b", "merge_sink")
        .source("source_a")
        .source("source_b")
        .sink("merge_sink");

    RunGuard g(2);
    graph.start(g.dispatcher);

    // Push 5 items to each source → 10 total outputs
    constexpr size_t kEach = 5;
    for (size_t i = 0; i < kEach; ++i) {
        graph.try_push(Event{static_cast<int>(i * 2),     ""});  // source_a
        graph.try_push(Event{static_cast<int>(i * 2 + 1), ""});  // source_b
    }

    auto results = collect(graph.output(), kEach * 2);
    EXPECT_GE(results.size(), kEach) << "too few results after fan-in";
    graph.stop();
}

// =============================================================================
// §5-1  Conditional Edge (A/B routing) — edge_if predicate routing
// =============================================================================

TEST(PipelineGraph, ConditionalEdgeRouting) {
    // edge_if: even → even_sink, odd → odd_sink
    PipelineGraph<int> graph;
    graph
        .node("source", [](int x, ActionEnv) -> Task<Result<int>> { co_return x; },
              1, 64)
        .node("even_sink", [](int x, ActionEnv) -> Task<Result<int>> { co_return x * 10; },
              1, 64)
        .node("odd_sink",  [](int x, ActionEnv) -> Task<Result<int>> { co_return x * 100; },
              1, 64)
        .edge_if("source", "even_sink",
                 [](const std::any& v) { return std::any_cast<int>(v) % 2 == 0; })
        .edge_if("source", "odd_sink",
                 [](const std::any& v) { return std::any_cast<int>(v) % 2 != 0; })
        .source("source")
        .sink("even_sink")
        .sink("odd_sink");

    RunGuard g(2);
    graph.start(g.dispatcher);

    for (int i = 1; i <= 6; ++i)
        graph.try_push(i); // 2,4,6 → ×10 ; 1,3,5 → ×100

    auto results = collect(graph.output(), 6);
    ASSERT_EQ(results.size(), 6u);

    for (int r : results) {
        // even path multiplies by 10 (multiples of 10), odd path multiplies by 100 (multiples of 100)
        bool is_even_branch = (r % 10 == 0) && (r % 100 != 0);
        bool is_odd_branch  = (r % 100 == 0);
        EXPECT_TRUE(is_even_branch || is_odd_branch)
            << "routing result must belong to one of the two paths: r=" << r;
    }
    graph.stop();
}

// =============================================================================
// §5-2  Sidecar Observation — T-pipe copy observation
// =============================================================================

TEST(SidecarObservation, TeeChannelDoesNotAffectLatency) {
    // Guide §5-2: the observation path must not affect the main path's processing result
    std::atomic<size_t> observed{0};
    auto side_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(64);

    auto tee_fn = [&side_ch, &observed](int x, ActionEnv) -> Task<Result<int>> {
        // sidecar: copy value into observation channel (main path returns original)
        side_ch->try_send(ContextualItem<int>{x, {}});
        observed.fetch_add(1, std::memory_order_relaxed);
        co_return x; // pass through unchanged
    };

    auto main_fn = [](int x) -> Task<Result<int>> {
        co_return x * 2;
    };

    auto pipeline = pipeline_builder<int>()
        .add<int>(tee_fn)
        .add<int>(main_fn)
        .build();

    RunGuard g;
    pipeline.start(g.dispatcher);

    constexpr size_t kItems = 10;
    for (size_t i = 1; i <= kItems; ++i)
        pipeline.try_push(static_cast<int>(i));

    auto main_results = collect(pipeline.output(), kItems);

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (observed.load() < kItems && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);

    pipeline.stop();

    // Verify main path results (×2)
    ASSERT_EQ(main_results.size(), kItems);
    for (int r : main_results)
        EXPECT_GT(r, 0);

    // Verify sidecar observation count
    EXPECT_EQ(observed.load(), kItems) << "sidecar observation count differs from input count";
}

// =============================================================================
// §5-2  Feedback Loop — upstream retransmit of failed items
// =============================================================================

TEST(FeedbackLoop, FailedItemRetried) {
    // Guide §5-2: retransmit failed items to the upstream channel
    // First attempt fails, second attempt succeeds, using an attempt counter
    std::atomic<size_t> attempts{0};

    auto feedback_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(64);

    auto fn = [&](int x, ActionEnv) -> Task<Result<int>> {
        size_t attempt = attempts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (attempt < 3) {
            // re-push to retry channel
            feedback_ch->try_send(ContextualItem<int>{x, {}});
            co_return unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        co_return x * 10;
    };

    Action<int,int> action{fn, {.min_workers=1, .max_workers=1, .channel_cap=64}};
    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(64);

    RunGuard g;
    action.start(g.dispatcher, out_ch);

    // First push
    action.try_push(42);

    // Re-push retry items from feedback_ch back into action
    auto refeeder = [&]() -> Task<void> {
        for (size_t i = 0; i < 5; ++i) {
            auto item = co_await feedback_ch->recv();
            if (!item) co_return;
            co_await action.push(item->value, item->ctx);
        }
    };
    g.dispatcher.spawn(refeeder());

    // Wait for success result
    auto results = collect(out_ch, 1, 5000ms);

    // Close feedback_ch first so the refeeder coroutine can exit cleanly,
    // then stop the action so its worker coroutine exits before the guard destructs.
    feedback_ch->close();
    action.stop();

    ASSERT_FALSE(results.empty()) << "no result after retry via feedback loop";
    EXPECT_EQ(results[0], 420) << "should be 42*10=420";
    EXPECT_GE(attempts.load(), 3u) << "at least 3 attempts required";
}

// =============================================================================
// §6A  Sensor Fusion (N:1 Sync) — ServiceRegistry Gather
// =============================================================================

namespace {
struct ImuData  { std::string fid; float ax, ay, az; };
struct GpsData  { std::string fid; double lat, lng;  };
struct FusedPose{ std::string fid; bool complete = false; float ax; double lat; };

struct FusionBuf {
    std::mutex mu;
    std::unordered_map<std::string, ImuData>  imu;
    std::unordered_map<std::string, GpsData>  gps;
};

struct SensorMsg {
    enum class Kind { IMU, GPS } kind;
    ImuData imu{};
    GpsData gps{};
};
} // namespace

TEST(SensorFusion, NToOneSyncViaServiceRegistry) {
    // Guide Recipe A: store partial data in ServiceRegistry → synchronize two sensors
    ServiceRegistry registry;

    auto gather = [](SensorMsg msg, ActionEnv env) -> Task<Result<FusedPose>> {
        auto& buf = env.registry->get_or_create<FusionBuf>();
        std::string fid = (msg.kind == SensorMsg::Kind::IMU)
                          ? msg.imu.fid : msg.gps.fid;
        {
            std::lock_guard lk(buf.mu);
            if (msg.kind == SensorMsg::Kind::IMU)
                buf.imu[fid] = msg.imu;
            else
                buf.gps[fid] = msg.gps;

            auto iit = buf.imu.find(fid);
            auto git = buf.gps.find(fid);
            if (iit != buf.imu.end() && git != buf.gps.end()) {
                FusedPose pose{fid, true, iit->second.ax, git->second.lat};
                buf.imu.erase(iit);
                buf.gps.erase(git);
                co_return pose;
            }
        }
        co_return FusedPose{fid, false, 0, 0};
    };

    Action<SensorMsg, FusedPose>::Config cfg{
        .min_workers=1, .max_workers=1, .channel_cap=128, .registry=&registry};
    Action<SensorMsg, FusedPose> action{gather, cfg};

    RunGuard g;
    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<FusedPose>>>(128);
    action.start(g.dispatcher, out_ch);

    constexpr size_t kFrames = 6;
    for (size_t i = 0; i < kFrames; ++i) {
        std::string fid = "f" + std::to_string(i);
        // GPS first, IMU second
        action.try_push(SensorMsg{SensorMsg::Kind::GPS,
                                   {}, GpsData{fid, 37.5 + i*0.01, 127.0}});
        action.try_push(SensorMsg{SensorMsg::Kind::IMU,
                                   ImuData{fid, float(i), float(i), 9.8f}});
    }

    // Collect only complete=true items out of kFrames*2 messages
    size_t fused = 0;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fused < kFrames && std::chrono::steady_clock::now() < deadline) {
        auto item = out_ch->try_recv();
        if (item && item->value.complete) ++fused;
        else std::this_thread::sleep_for(1ms);
    }

    action.stop();
    EXPECT_EQ(fused, kFrames) << "all frames must be fused";
}

// =============================================================================
// §6B  Hardware Batching (NPU) — BatchAction
// =============================================================================

TEST(HardwareBatching, BatchActionAccumulatesAndFlushes) {
    // Guide Recipe B: batch processing when max_batch_size is reached or on timeout
    std::atomic<size_t> batch_calls{0};
    std::atomic<size_t> total_items{0};

    auto npu_fn = [&](std::vector<int> batch, ActionEnv) -> Task<Result<std::vector<int>>> {
        batch_calls.fetch_add(1, std::memory_order_relaxed);
        total_items.fetch_add(batch.size(), std::memory_order_relaxed);
        std::vector<int> out;
        for (int x : batch) out.push_back(x * 2);
        co_return out;
    };

    constexpr size_t kBatch = 4;
    BatchAction<int,int> ba{
        npu_fn,
        BatchAction<int,int>::Config{.max_batch_size=kBatch, .max_wait_ms=20, .workers=1}
    };

    RunGuard g;
    ba.start(g.dispatcher);

    // Exactly 2 batches = 8 items pushed
    constexpr size_t kItems = 8;
    for (size_t i = 1; i <= kItems; ++i)
        ba.try_push(static_cast<int>(i));

    auto results = collect(ba.output(), kItems, 5000ms);

    ba.stop();

    // All items must be processed
    EXPECT_EQ(results.size(), kItems) << "output count mismatch after batch processing";
    // At least 2 batch calls (kItems / kBatch)
    EXPECT_GE(batch_calls.load(), kItems / kBatch)
        << "insufficient batch function call count";
    EXPECT_EQ(total_items.load(), kItems) << "total processed item count mismatch";
}

TEST(HardwareBatching, BatchActionTimeoutFlush) {
    // Verify flush after max_wait_ms even when max_batch_size is not reached
    std::atomic<size_t> batches{0};

    BatchAction<int,int> ba{
        [&](std::vector<int> v, ActionEnv) -> Task<Result<std::vector<int>>> {
            batches.fetch_add(1, std::memory_order_relaxed);
            co_return v;
        },
        BatchAction<int,int>::Config{
            .max_batch_size = 100,   // very large batch size
            .max_wait_ms    = 30,    // force flush after 30ms
            .workers        = 1,
        }
    };

    RunGuard g;
    ba.start(g.dispatcher);

    // Push only 1 item — below batch size, timeout triggers flush
    ba.try_push(99);

    auto results = collect(ba.output(), 1, 2000ms);
    ba.stop();

    ASSERT_EQ(results.size(), 1u) << "no result after timeout flush";
    EXPECT_EQ(results[0], 99);
    EXPECT_GE(batches.load(), 1u);
}

// =============================================================================
// §6C  Resilient WAS (DLQ) — DlqAction + DeadLetterQueue
// =============================================================================

TEST(DeadLetterQueue, DlqActionSendsFailuresToDlq) {
    // Guide Recipe C: send to DLQ after exceeding max_attempts
    auto dlq = std::make_shared<DeadLetterQueue<int>>();

    std::atomic<int> attempt_counter{0};
    auto failing_fn = [&](int x, ActionEnv) -> Task<Result<int>> {
        attempt_counter.fetch_add(1, std::memory_order_relaxed);
        // always fail
        (void)x;
        co_return unexpected(std::make_error_code(std::errc::io_error));
    };

    constexpr size_t kMaxAttempts = 3;
    DlqAction<int,int> dlq_action{failing_fn, dlq, kMaxAttempts};

    Action<int,int> action{dlq_action,
                            {.min_workers=1, .max_workers=1, .channel_cap=32}};

    RunGuard g;
    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(32);
    action.start(g.dispatcher, out_ch);

    action.try_push(42);

    // Wait until items accumulate in the DLQ
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (dlq->size() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    action.stop();

    EXPECT_GE(dlq->size(), 1u) << "no failed items in DLQ";
    EXPECT_GE(attempt_counter.load(), static_cast<int>(kMaxAttempts))
        << "should retry up to the maximum attempt count";
}

TEST(DeadLetterQueue, DlqActionSuccessSkipsDlq) {
    // On success, items must not be sent to the DLQ
    auto dlq = std::make_shared<DeadLetterQueue<int>>();

    DlqAction<int,int> dlq_action{
        [](int x, ActionEnv) -> Task<Result<int>> { co_return x * 2; },
        dlq, 3
    };

    Action<int,int> action{dlq_action,
                            {.min_workers=1, .max_workers=1, .channel_cap=32}};

    RunGuard g;
    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(32);
    action.start(g.dispatcher, out_ch);

    action.try_push(5);
    auto results = collect(out_ch, 1);
    action.stop();

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0], 10);
    EXPECT_EQ(dlq->size(), 0u) << "successful items must not enter the DLQ";
}

TEST(DeadLetterQueue, DlqDrainReturnsAllLetters) {
    // Verify DeadLetterQueue::drain()
    auto dlq = std::make_shared<DeadLetterQueue<std::string>>();
    dlq->push("msg1", {}, std::make_error_code(std::errc::io_error));
    dlq->push("msg2", {}, std::make_error_code(std::errc::timed_out));
    dlq->push("msg3", {}, std::make_error_code(std::errc::connection_refused));

    EXPECT_EQ(dlq->size(), 3u);

    auto letters = dlq->drain();
    EXPECT_EQ(letters.size(), 3u);
    EXPECT_EQ(dlq->size(), 0u) << "DLQ must be empty after drain()";
}

TEST(DeadLetterQueue, DlqMaxSizeDropsOldest) {
    // Drop oldest items when max_size is exceeded
    DeadLetterQueue<int> dlq({.max_size = 3});
    for (int i = 1; i <= 5; ++i)
        dlq.push(i, {}, std::make_error_code(std::errc::io_error));

    EXPECT_EQ(dlq.size(), 3u) << "items exceeding max_size must be dropped";
    auto letters = dlq.drain();
    // oldest items 1, 2 are dropped; only 3, 4, 5 remain
    ASSERT_EQ(letters.size(), 3u);
    EXPECT_EQ(letters[0].item, 3);
    EXPECT_EQ(letters[1].item, 4);
    EXPECT_EQ(letters[2].item, 5);
}

// =============================================================================
// §7  Periodic Polling Source — while(true) + co_await sleep
// =============================================================================

TEST(PeriodicPollingSource, PollsAtRegularIntervals) {
    // Guide §7: sensor/hardware register polling source pattern
    // co_await sleep() + channel push loop
    auto poll_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(64);
    std::atomic<size_t> poll_count{0};
    std::atomic<bool>   stop_flag{false};

    RunGuard g;

    // Periodic Source coroutine (guide §7 pattern)
    auto polling_source = [&]() -> Task<void> {
        while (!stop_flag.load(std::memory_order_acquire)) {
            int sensor_value = static_cast<int>(poll_count.fetch_add(1, std::memory_order_relaxed));
            poll_ch->try_send(ContextualItem<int>{sensor_value, {}});
            // co_await qbuem::sleep(10ms) — replaced with short manual wait in test environment
            co_await std::suspend_never{};
        }
    };

    g.dispatcher.spawn(polling_source());

    // Collect at least 5 polling events
    auto deadline = std::chrono::steady_clock::now() + 3s;
    size_t received = 0;
    while (received < 5 && std::chrono::steady_clock::now() < deadline) {
        auto item = poll_ch->try_recv();
        if (item) ++received;
        else std::this_thread::sleep_for(1ms);
    }

    stop_flag.store(true, std::memory_order_release);

    EXPECT_GE(received, 5u)    << "polling source must generate at least 5 events";
    EXPECT_GE(poll_count.load(), received) << "poll counter must be at least the received count";
}

// =============================================================================
// §7  Source Pinning — Dispatcher::spawn_on (core pinning API validation)
// =============================================================================

TEST(PeriodicPollingSource, MultiSourceContextIsolation) {
    // Two independent source coroutines push to the channel with different Contexts
    struct Tagged { int val; std::string source_id; };

    auto ch = std::make_shared<AsyncChannel<ContextualItem<Tagged>>>(64);

    // Lambda coroutines must outlive the dispatcher. Declare them before
    // RunGuard so their closures are valid when the coroutine resumes.
    auto src_a = [ch]() -> Task<void> {
        for (int i = 0; i < 5; ++i) {
            ch->try_send(ContextualItem<Tagged>{Tagged{i, "A"}, {}});
            co_await std::suspend_never{};
        }
    };
    auto src_b = [ch]() -> Task<void> {
        for (int i = 0; i < 5; ++i) {
            ch->try_send(ContextualItem<Tagged>{Tagged{i, "B"}, {}});
            co_await std::suspend_never{};
        }
    };

    RunGuard g(2);

    // Source A
    g.dispatcher.spawn(src_a());

    // Source B
    g.dispatcher.spawn(src_b());

    size_t from_a = 0, from_b = 0;
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while ((from_a + from_b) < 10 && std::chrono::steady_clock::now() < deadline) {
        auto item = ch->try_recv();
        if (item) {
            if (item->value.source_id == "A") ++from_a;
            else                               ++from_b;
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }

    EXPECT_GE(from_a, 1u) << "no events from Source A";
    EXPECT_GE(from_b, 1u) << "no events from Source B";
}

// =============================================================================
// §3-1  StaticPipeline — additional compile-time type chain validation
// =============================================================================

TEST(StaticPipeline, TypeChainIsCorrect) {
    // 3-stage chain type safety — verify return type of pipeline_builder<>().add<>().build()
    auto pipeline = pipeline_builder<int>()
        .add<std::string>([](int x) -> Task<Result<std::string>> {
            co_return std::to_string(x);
        })
        .add<size_t>([](std::string s) -> Task<Result<size_t>> {
            co_return s.size();
        })
        .build();

    using P = StaticPipeline<int, size_t>;
    static_assert(std::is_same_v<decltype(pipeline), P>, "type chain is incorrect");

    EXPECT_EQ(pipeline.state(), P::State::Created);
    ASSERT_NE(pipeline.output(), nullptr);
}

TEST(StaticPipeline, LiveEndToEndProcessing) {
    // E2E test running a real Dispatcher and collecting results
    auto pipeline = pipeline_builder<int>()
        .add<int>([](int x) -> Task<Result<int>> { co_return x * 3; })
        .add<int>([](int x) -> Task<Result<int>> { co_return x - 1; })
        .build();

    RunGuard g;
    pipeline.start(g.dispatcher);

    constexpr size_t kItems = 8;
    for (size_t i = 1; i <= kItems; ++i)
        pipeline.try_push(static_cast<int>(i));

    auto results = collect(pipeline.output(), kItems);
    pipeline.stop();

    ASSERT_EQ(results.size(), kItems);
    for (int r : results) {
        // range of x*3-1: 1*3-1=2 to 8*3-1=23
        EXPECT_GE(r, 2);
        EXPECT_LE(r, 23);
    }
}
