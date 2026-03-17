/**
 * @file tests/pipeline_advanced_test.cpp
 * @brief Unit tests for advanced pipeline features
 */

#include <gtest/gtest.h>

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/checkpoint.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/health.hpp>
#include <qbuem/pipeline/idempotency.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/observability.hpp>
#include <qbuem/pipeline/saga.hpp>
#include <qbuem/pipeline/slo.hpp>
#include <qbuem/pipeline/task_group.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── RunGuard helper ──────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher dispatcher;
    std::thread thread;

    explicit RunGuard(size_t threads = 1) : dispatcher(threads) {
        thread = std::thread([this] { dispatcher.run(); });
    }
    ~RunGuard() {
        dispatcher.stop();
        if (thread.joinable()) thread.join();
    }

    // Named coroutine (not lambda) to prevent GCC HALO placing the frame on
    // run_and_wait's stack and causing stack-use-after-scope under ASan.
    template <typename F>
    static Task<void> run_coro(F f, std::shared_ptr<std::atomic<bool>> done) {
        co_await f();
        done->store(true, std::memory_order_release);
    }

    template <typename F>
    void run_and_wait(F&& f, std::chrono::milliseconds timeout = 5s) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        dispatcher.spawn(run_coro(std::forward<F>(f), done));
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done->load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        EXPECT_TRUE(done->load()) << "Timed out waiting for task";
    }
};

// ─── HistogramMetrics ─────────────────────────────────────────────────────────

TEST(HistogramMetricsTest, InitialCountsAreZero) {
    HistogramMetrics hist({1000, 10000, 100000});
    auto counts = hist.bucket_counts();
    EXPECT_EQ(counts.size(), 4u);
    for (auto c : counts) EXPECT_EQ(c, 0u);
}

TEST(HistogramMetricsTest, ObserveBelowFirstBound) {
    HistogramMetrics hist({1000, 10000});
    hist.observe(500);
    auto counts = hist.bucket_counts();
    EXPECT_EQ(counts[0], 1u);
    EXPECT_EQ(counts[1], 0u);
    EXPECT_EQ(counts[2], 0u);
}

TEST(HistogramMetricsTest, ObserveExactBound) {
    HistogramMetrics hist({1000, 10000});
    hist.observe(1000);
    auto counts = hist.bucket_counts();
    EXPECT_EQ(counts[0], 1u);
}

TEST(HistogramMetricsTest, ObserveBeyondAllBounds) {
    HistogramMetrics hist({1000, 10000});
    hist.observe(99999);
    auto counts = hist.bucket_counts();
    EXPECT_EQ(counts[2], 1u);
}

TEST(HistogramMetricsTest, MultipleObservations) {
    HistogramMetrics hist({1000, 10000});
    hist.observe(100);
    hist.observe(500);
    hist.observe(2000);
    hist.observe(50000);
    auto counts = hist.bucket_counts();
    EXPECT_EQ(counts[0], 2u);
    EXPECT_EQ(counts[1], 1u);
    EXPECT_EQ(counts[2], 1u);
}

TEST(HistogramMetricsTest, ResetClearsAllBuckets) {
    HistogramMetrics hist({1000, 10000});
    hist.observe(500);
    hist.observe(5000);
    hist.reset();
    auto counts = hist.bucket_counts();
    for (auto c : counts) EXPECT_EQ(c, 0u);
}

TEST(HistogramMetricsTest, UpperBoundsAccessible) {
    HistogramMetrics hist({100, 500, 1000});
    const auto& bounds = hist.upper_bounds();
    ASSERT_EQ(bounds.size(), 3u);
    EXPECT_EQ(bounds[0], 100u);
    EXPECT_EQ(bounds[1], 500u);
    EXPECT_EQ(bounds[2], 1000u);
}

// ─── PipelineVersion ──────────────────────────────────────────────────────────

TEST(PipelineVersionTest, ToStringFormat) {
    PipelineVersion v{1, 2, 3};
    EXPECT_EQ(v.to_string(), "1.2.3");
}

TEST(PipelineVersionTest, ZeroVersion) {
    PipelineVersion v{};
    EXPECT_EQ(v.to_string(), "0.0.0");
}

TEST(PipelineVersionTest, CompatibleWithSameMajor) {
    PipelineVersion v1{2, 0, 0};
    PipelineVersion v2{2, 5, 1};
    EXPECT_TRUE(v1.compatible_with(v2));
    EXPECT_TRUE(v2.compatible_with(v1));
}

TEST(PipelineVersionTest, IncompatibleDifferentMajor) {
    PipelineVersion v1{1, 9, 9};
    PipelineVersion v2{2, 0, 0};
    EXPECT_FALSE(v1.compatible_with(v2));
}

TEST(PipelineVersionTest, ComparisonOperators) {
    PipelineVersion v1{1, 0, 0};
    PipelineVersion v2{2, 0, 0};
    EXPECT_LT(v1, v2);
    EXPECT_GT(v2, v1);
    EXPECT_EQ(v1, v1);
}

// ─── ActionHealth ─────────────────────────────────────────────────────────────

TEST(ActionHealthTest, DefaultState) {
    ActionHealth ah;
    EXPECT_EQ(ah.circuit_state, "CLOSED");
    EXPECT_DOUBLE_EQ(ah.error_rate_1m, 0.0);
    EXPECT_EQ(ah.p99_us, 0u);
    EXPECT_EQ(ah.queue_depth, 0u);
    EXPECT_EQ(ah.items_processed, 0u);
}

TEST(ActionHealthTest, ToJsonContainsName) {
    ActionHealth ah;
    ah.name = "my_action";
    ah.circuit_state = "OPEN";
    ah.items_processed = 42;
    std::string json = ah.to_json();
    EXPECT_NE(json.find("my_action"), std::string::npos);
    EXPECT_NE(json.find("OPEN"), std::string::npos);
}

// ─── SloConfig ────────────────────────────────────────────────────────────────

TEST(SloConfigTest, CanBeConstructed) {
    SloConfig cfg;
    SUCCEED();
}

// ─── CheckpointData ───────────────────────────────────────────────────────────

TEST(CheckpointDataTest, DefaultValues) {
    CheckpointData cd;
    EXPECT_EQ(cd.offset, 0u);
    EXPECT_TRUE(cd.metadata_json.empty());
}

TEST(CheckpointDataTest, CanSetFields) {
    CheckpointData cd;
    cd.offset = 12345;
    cd.metadata_json = "{\"batch\":42}";
    cd.saved_at = std::chrono::system_clock::now();
    EXPECT_EQ(cd.offset, 12345u);
    EXPECT_EQ(cd.metadata_json, "{\"batch\":42}");
}

// ─── InMemoryCheckpointStore ──────────────────────────────────────────────────

TEST(InMemoryCheckpointStoreTest, SaveAndLoad) {
    RunGuard rg;
    rg.run_and_wait([]() -> Task<void> {
        auto store = std::make_shared<InMemoryCheckpointStore>();
        auto save_result = co_await store->save("pipeline-A", 100, "{\"key\":1}");
        EXPECT_TRUE(save_result.has_value());
        auto load_result = co_await store->load("pipeline-A");
        EXPECT_TRUE(load_result.has_value());
        EXPECT_EQ(load_result->offset, 100u);
        EXPECT_EQ(load_result->metadata_json, "{\"key\":1}");
    });
}

TEST(InMemoryCheckpointStoreTest, LoadNonExistentReturnsError) {
    RunGuard rg;
    rg.run_and_wait([]() -> Task<void> {
        auto store = std::make_shared<InMemoryCheckpointStore>();
        auto result = co_await store->load("nonexistent-pipeline");
        EXPECT_FALSE(result.has_value());
    });
}

// ─── InMemoryIdempotencyStore ─────────────────────────────────────────────────

TEST(InMemoryIdempotencyStoreTest, NewKeyIsInserted) {
    RunGuard rg;
    rg.run_and_wait([]() -> Task<void> {
        auto store = std::make_shared<InMemoryIdempotencyStore>();
        bool is_new = co_await store->set_if_absent("key-1", std::chrono::hours{1});
        EXPECT_TRUE(is_new);
    });
}

TEST(InMemoryIdempotencyStoreTest, DuplicateKeyIsRejected) {
    RunGuard rg;
    rg.run_and_wait([]() -> Task<void> {
        auto store = std::make_shared<InMemoryIdempotencyStore>();
        co_await store->set_if_absent("key-dup", std::chrono::hours{1});
        bool is_new = co_await store->set_if_absent("key-dup", std::chrono::hours{1});
        EXPECT_FALSE(is_new);
    });
}

TEST(InMemoryIdempotencyStoreTest, GetReturnsTrueForExistingKey) {
    RunGuard rg;
    rg.run_and_wait([]() -> Task<void> {
        auto store = std::make_shared<InMemoryIdempotencyStore>();
        co_await store->set_if_absent("key-get", std::chrono::hours{1});
        bool exists = co_await store->get("key-get");
        EXPECT_TRUE(exists);
    });
}

TEST(InMemoryIdempotencyStoreTest, GetReturnsFalseForMissingKey) {
    RunGuard rg;
    rg.run_and_wait([]() -> Task<void> {
        auto store = std::make_shared<InMemoryIdempotencyStore>();
        bool exists = co_await store->get("missing-key");
        EXPECT_FALSE(exists);
    });
}

// ─── SagaOrchestrator ─────────────────────────────────────────────────────────

TEST(SagaOrchestratorTest, AllStepsSucceed) {
    RunGuard rg;
    rg.run_and_wait([]() -> Task<void> {
        SagaOrchestrator<int> saga;
        saga.add_step(SagaStep<int, int>{
            .name    = "double",
            .execute = [](int x) -> Task<Result<int>> { co_return x * 2; },
            .compensate = [](int) -> Task<void> { co_return; },
        });
        saga.add_step(SagaStep<int, int>{
            .name    = "add_one",
            .execute = [](int x) -> Task<Result<int>> { co_return x + 1; },
            .compensate = [](int) -> Task<void> { co_return; },
        });
        auto r = co_await saga.run(5);
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*r, 11);  // (5 * 2) + 1 = 11
    });
}

TEST(SagaOrchestratorTest, FailureTriggersCompensation) {
    RunGuard rg;
    rg.run_and_wait([]() -> Task<void> {
        // atomic<int> in the coroutine frame (heap) — safe to capture by ref
        // from compensate lambda because saga awaits compensation before returning.
        std::atomic<int> compensated{0};

        SagaOrchestrator<int> saga;
        saga.add_step(SagaStep<int, int>{
            .name    = "step-1",
            .execute = [](int x) -> Task<Result<int>> { co_return x + 1; },
            .compensate = [&compensated](int) -> Task<void> {
                compensated.fetch_add(1, std::memory_order_relaxed);
                co_return;
            },
        });
        saga.add_step(SagaStep<int, int>{
            .name    = "step-2-fail",
            .execute = [](int) -> Task<Result<int>> {
                co_return unexpected(std::make_error_code(std::errc::operation_canceled));
            },
            .compensate = [](int) -> Task<void> { co_return; },
        });

        auto r = co_await saga.run(10);
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(compensated.load(), 1);
        EXPECT_TRUE(saga.compensation_failures().empty());
    });
}

// ─── MessageBus ───────────────────────────────────────────────────────────────

TEST(MessageBusTest, SubscribeAndPublish) {
    RunGuard rg(2);
    // received is shared_ptr because the subscriber lambda is a concurrent coroutine
    // not awaited by the outer coroutine, so its frame is independent.
    auto received = std::make_shared<std::atomic<int>>(0);
    rg.run_and_wait([&rg, received]() -> Task<void> {
        MessageBus bus;
        bus.start(rg.dispatcher);

        auto sub = bus.subscribe("test-topic",
            [received](MessageBus::Msg msg, Context) -> Task<Result<void>> {
                (void)msg;
                received->fetch_add(1, std::memory_order_relaxed);
                co_return {};
            });

        co_await bus.publish("test-topic", std::string{"hello"});
        co_await bus.publish("test-topic", std::string{"world"});
        // publish() co_awaits each handler synchronously — no extra wait needed.
        EXPECT_EQ(received->load(), 2);
    });
}

TEST(MessageBusTest, SubscriberCount) {
    RunGuard rg;
    rg.run_and_wait([&rg]() -> Task<void> {
        MessageBus bus;
        bus.start(rg.dispatcher);

        EXPECT_EQ(bus.subscriber_count("topic-x"), 0u);

        auto sub1 = bus.subscribe("topic-x",
            [](MessageBus::Msg, Context) -> Task<Result<void>> { co_return {}; });
        auto sub2 = bus.subscribe("topic-x",
            [](MessageBus::Msg, Context) -> Task<Result<void>> { co_return {}; });

        EXPECT_EQ(bus.subscriber_count("topic-x"), 2u);

        {
            auto s = std::move(sub1);
        }
        // After sub1 destroyed, count drops; just verify no crash
        co_return;
    });
}

TEST(MessageBusTest, TryPublishNonBlocking) {
    RunGuard rg;
    rg.run_and_wait([&rg]() -> Task<void> {
        MessageBus bus;
        bus.start(rg.dispatcher);

        auto sub = bus.subscribe("fire-forget",
            [](MessageBus::Msg, Context) -> Task<Result<void>> { co_return {}; });

        bool ok = bus.try_publish("fire-forget", 42);
        (void)ok;
        co_return;
    });
}

// ─── TaskGroup ────────────────────────────────────────────────────────────────

// Named (non-lambda) coroutine: GCC HALO does not elide heap allocation for
// named functions, unlike lambdas where HALO can place the frame on C stack.
static Task<Result<void>> task_group_worker(
        std::shared_ptr<std::atomic<int>> total, int i) {
    total->fetch_add(i + 1, std::memory_order_relaxed);
    co_return {};
}

TEST(TaskGroupTest, SpawnAndJoinAll) {
    RunGuard rg(2);
    rg.run_and_wait([]() -> Task<void> {
        auto total = std::make_shared<std::atomic<int>>(0);
        TaskGroup group;

        for (int i = 0; i < 5; ++i) {
            group.spawn(task_group_worker(total, i));
        }

        co_await group.join();
        EXPECT_EQ(total->load(), 1 + 2 + 3 + 4 + 5);
    });
}
