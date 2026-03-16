/**
 * @file tests/pipeline_ipc_test.cpp
 * @brief Pipeline ↔ MessageBus 연계 통합 테스트
 *
 * 커버리지:
 * - PipelineBuilder::with_source<MessageBusSource>: MessageBus 토픽 → Pipeline
 * - PipelineBuilder::with_sink<MessageBusSink>:    Pipeline → MessageBus 토픽
 * - MessageBusSource<T>: init() + next() 정확성
 * - MessageBusSink<T>:   init() + sink() 정확성
 * - 통합: source → pipeline → sink 전체 흐름
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 공통 RunGuard ───────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher dispatcher;
    std::thread thread;

    explicit RunGuard(size_t threads = 2) : dispatcher(threads) {
        thread = std::thread([this] { dispatcher.run(); });
    }
    ~RunGuard() {
        dispatcher.stop();
        if (thread.joinable()) thread.join();
    }

    template <typename F>
    void run_and_wait(F&& f, std::chrono::milliseconds timeout = 5s) {
        std::atomic<bool> done{false};
        dispatcher.spawn([&, f = std::forward<F>(f)]() mutable -> Task<void> {
            co_await f();
            done.store(true, std::memory_order_release);
        }());
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        EXPECT_TRUE(done.load()) << "run_and_wait timed out";
    }
};

// ─── MessageBusSink: init() 는 ok, sink() 는 bus.publish() ───────────────────

TEST(MessageBusSinkTest, InitAlwaysOk) {
    MessageBus bus;
    MessageBusSink<int> snk(bus, "test.topic");
    auto res = snk.init();
    EXPECT_TRUE(res.has_value());
}

TEST(MessageBusSinkTest, SinkPublishesToBus) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    std::atomic<int> received{0};
    std::atomic<int> last_val{0};
    auto sub = bus.subscribe("numbers",
        [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            received.fetch_add(1, std::memory_order_relaxed);
            last_val.store(std::any_cast<int>(msg), std::memory_order_relaxed);
            co_return Result<void>{};
        });

    MessageBusSink<int> snk(bus, "numbers");
    ASSERT_TRUE(snk.init().has_value());

    guard.run_and_wait([&]() -> Task<void> {
        co_await snk.sink(42);
        co_await snk.sink(99);
        std::this_thread::sleep_for(20ms);
    });

    EXPECT_EQ(received.load(), 2);
    EXPECT_EQ(last_val.load(), 99);
}

// ─── MessageBusSource: init() 구독, next() 메시지 수신 ──────────────────────

TEST(MessageBusSourceTest, InitSubscribesStream) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    MessageBusSource<int> src(bus, "stream.topic");
    ASSERT_TRUE(src.init().has_value());

    std::atomic<int> got{0};
    guard.run_and_wait([&]() -> Task<void> {
        // 발행 후 next()로 수신
        co_await bus.publish("stream.topic", 7);
        auto opt = co_await src.next();
        EXPECT_TRUE(opt.has_value());
        if (opt.has_value() && opt.value())
            got.store(*opt.value(), std::memory_order_relaxed);
    });

    EXPECT_EQ(got.load(), 7);
}

TEST(MessageBusSourceTest, CloseStopsStream) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    MessageBusSource<int> src(bus, "closeable.topic");
    ASSERT_TRUE(src.init().has_value());

    // close()를 호출하면 next()가 nullopt를 반환해야 함
    src.close();

    std::atomic<bool> got_nullopt{false};
    guard.run_and_wait([&]() -> Task<void> {
        auto opt = co_await src.next();
        if (!opt.has_value()) got_nullopt.store(true);
    });

    EXPECT_TRUE(got_nullopt.load());
}

// ─── PipelineBuilder::with_sink ──────────────────────────────────────────────

TEST(PipelineBuilderWithSinkTest, SinkReceivesPipelineOutput) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    std::atomic<int> received{0};
    auto sub = bus.subscribe("doubled",
        [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            received.fetch_add(1, std::memory_order_relaxed);
            co_return Result<void>{};
        });

    auto pipeline = PipelineBuilder<int, int>{}
        .add<int>([](int v, ActionEnv) -> Task<Result<int>> { co_return v * 2; })
        .with_sink(MessageBusSink<int>(bus, "doubled"))
        .build();
    pipeline.start(guard.dispatcher);

    guard.run_and_wait([&]() -> Task<void> {
        co_await pipeline.push(1);
        co_await pipeline.push(2);
        co_await pipeline.push(3);
        std::this_thread::sleep_for(30ms);
    });

    EXPECT_EQ(received.load(), 3);
}

// ─── PipelineBuilder::with_source ────────────────────────────────────────────

TEST(PipelineBuilderWithSourceTest, SourceFeedsDataIntoPipeline) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    std::atomic<int> processed{0};
    std::atomic<int> sum{0};

    auto pipeline = PipelineBuilder<int, int>{}
        .with_source(MessageBusSource<int>(bus, "input.items"))
        .add<int>([&](int v, ActionEnv) -> Task<Result<int>> {
            processed.fetch_add(1, std::memory_order_relaxed);
            sum.fetch_add(v, std::memory_order_relaxed);
            co_return v;
        })
        .build();
    pipeline.start(guard.dispatcher);

    guard.run_and_wait([&]() -> Task<void> {
        std::this_thread::sleep_for(5ms);  // source init 완료 대기
        co_await bus.publish("input.items", 10);
        co_await bus.publish("input.items", 20);
        co_await bus.publish("input.items", 30);
        std::this_thread::sleep_for(30ms);
    });

    EXPECT_EQ(processed.load(), 3);
    EXPECT_EQ(sum.load(), 60);
}

// ─── 통합: with_source + stages + with_sink ──────────────────────────────────

TEST(PipelineIpcIntegration, SourceStageSinkEndToEnd) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    // source: "raw" 토픽 → pipeline (×10) → sink: "result" 토픽
    std::atomic<int> result_count{0};
    std::atomic<int> result_sum{0};
    auto sub = bus.subscribe("result",
        [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            result_count.fetch_add(1, std::memory_order_relaxed);
            result_sum.fetch_add(std::any_cast<int>(msg), std::memory_order_relaxed);
            co_return Result<void>{};
        });

    auto pipeline = PipelineBuilder<int, int>{}
        .with_source(MessageBusSource<int>(bus, "raw", 64))
        .add<int>([](int v, ActionEnv) -> Task<Result<int>> { co_return v * 10; })
        .with_sink(MessageBusSink<int>(bus, "result"))
        .build();
    pipeline.start(guard.dispatcher);

    guard.run_and_wait([&]() -> Task<void> {
        std::this_thread::sleep_for(5ms);
        for (int i = 1; i <= 5; ++i)
            co_await bus.publish("raw", i);
        std::this_thread::sleep_for(50ms);
    });

    EXPECT_EQ(result_count.load(), 5);
    EXPECT_EQ(result_sum.load(), (1 + 2 + 3 + 4 + 5) * 10);  // 150
}
