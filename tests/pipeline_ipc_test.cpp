/**
 * @file tests/pipeline_ipc_test.cpp
 * @brief Pipeline ↔ MessageBus 연계 통합 테스트
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
#include <memory>
#include <string>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Yield: 코루틴을 한 번 yield해 같은 리액터의 다른 코루틴이 실행되도록 함 ──

// Suspends the current coroutine once and re-schedules it on the same reactor.
// Use this instead of std::this_thread::sleep_for inside a coroutine's
// spin-wait loop: sleep_for blocks the reactor thread, starving other
// coroutines queued on the same reactor.
struct Yield {
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        if (auto* r = Reactor::current())
            r->post([h]() mutable { h.resume(); });
        else
            h.resume();
    }
    void await_resume() noexcept {}
};

// ─── 공통 RunGuard ───────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher dispatcher;
    std::jthread thread;

    explicit RunGuard(size_t threads = 2) : dispatcher(threads) {
        thread = std::jthread([this] { dispatcher.run(); });
    }
    ~RunGuard() {
        dispatcher.stop();
        thread.join();
    }

    // Named coroutine to prevent GCC HALO placing the lambda frame on
    // run_and_wait's stack (stack-use-after-scope under ASan).
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
        EXPECT_TRUE(done->load()) << "run_and_wait timed out";
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

    // shared_ptr because subscriber lambda is a concurrent coroutine (not awaited)
    auto received = std::make_shared<std::atomic<int>>(0);
    auto last_val = std::make_shared<std::atomic<int>>(0);
    auto sub = bus.subscribe("numbers",
        [received, last_val](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            received->fetch_add(1, std::memory_order_relaxed);
            last_val->store(std::any_cast<int>(msg), std::memory_order_relaxed);
            co_return Result<void>{};
        });

    MessageBusSink<int> snk(bus, "numbers");
    ASSERT_TRUE(snk.init().has_value());

    guard.run_and_wait([&snk]() -> Task<void> {
        co_await snk.sink(42);
        co_await snk.sink(99);
        std::this_thread::sleep_for(20ms);
    });

    EXPECT_EQ(received->load(), 2);
    EXPECT_EQ(last_val->load(), 99);
}

// ─── MessageBusSource: init() 구독, next() 메시지 수신 ──────────────────────

TEST(MessageBusSourceTest, InitSubscribesStream) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    MessageBusSource<int> src(bus, "stream.topic");
    ASSERT_TRUE(src.init().has_value());

    auto got = std::make_shared<std::atomic<int>>(0);
    guard.run_and_wait([&bus, &src, got]() -> Task<void> {
        co_await bus.publish("stream.topic", 7);
        auto opt = co_await src.next();
        EXPECT_TRUE(opt.has_value());
        if (opt.has_value() && opt.value())
            got->store(*opt.value(), std::memory_order_relaxed);
    });

    EXPECT_EQ(got->load(), 7);
}

TEST(MessageBusSourceTest, CloseStopsStream) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    MessageBusSource<int> src(bus, "closeable.topic");
    ASSERT_TRUE(src.init().has_value());

    src.close();

    auto got_nullopt = std::make_shared<std::atomic<bool>>(false);
    guard.run_and_wait([&src, got_nullopt]() -> Task<void> {
        auto opt = co_await src.next();
        if (!opt.has_value()) got_nullopt->store(true);
    });

    EXPECT_TRUE(got_nullopt->load());
}

// ─── PipelineBuilder::with_sink ──────────────────────────────────────────────

TEST(PipelineBuilderWithSinkTest, SinkReceivesPipelineOutput) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    auto received = std::make_shared<std::atomic<int>>(0);
    auto sub = bus.subscribe("doubled",
        [received](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            received->fetch_add(1, std::memory_order_relaxed);
            co_return Result<void>{};
        });

    auto pipeline = PipelineBuilder<int, int>{}
        .add<int>([](int v, ActionEnv) -> Task<Result<int>> { co_return v * 2; })
        .with_sink(MessageBusSink<int>(bus, "doubled"))
        .build();
    pipeline.start(guard.dispatcher);

    guard.run_and_wait([&pipeline, received]() -> Task<void> {
        co_await pipeline.push(1);
        co_await pipeline.push(2);
        co_await pipeline.push(3);
        // Spin-wait (up to 2s) for all subscriber callbacks to complete.
        // Use co_await Yield{} instead of sleep_for: sleep_for blocks the
        // reactor thread, starving other coroutines (worker, sink_pump) that
        // are queued on the same reactor.
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (received->load(std::memory_order_acquire) < 3 &&
               std::chrono::steady_clock::now() < deadline)
            co_await Yield{};
    });

    pipeline.stop();  // close channels so worker coroutines exit before dispatcher stops
    // Flush: ensure the reactor processes the post-stop wake events (worker,
    // sink_pump) before the dispatcher shuts down, preventing frame leaks.
    guard.run_and_wait([]() -> Task<void> { co_return; });
    EXPECT_EQ(received->load(), 3);
}

// ─── PipelineBuilder::with_source ────────────────────────────────────────────

TEST(PipelineBuilderWithSourceTest, SourceFeedsDataIntoPipeline) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    // shared_ptr because pipeline stage action runs as an independent coroutine
    auto processed = std::make_shared<std::atomic<int>>(0);
    auto sum       = std::make_shared<std::atomic<int>>(0);

    auto pipeline = PipelineBuilder<int, int>{}
        .with_source(MessageBusSource<int>(bus, "input.items"))
        .add<int>([processed, sum](int v, ActionEnv) -> Task<Result<int>> {
            processed->fetch_add(1, std::memory_order_relaxed);
            sum->fetch_add(v, std::memory_order_relaxed);
            co_return v;
        })
        .build();
    pipeline.start(guard.dispatcher);

    guard.run_and_wait([&bus, processed]() -> Task<void> {
        std::this_thread::sleep_for(5ms);
        co_await bus.publish("input.items", 10);
        co_await bus.publish("input.items", 20);
        co_await bus.publish("input.items", 30);
        // Spin-wait (up to 2s) for all items to be processed by the pipeline.
        // Use co_await Yield{} instead of sleep_for to avoid blocking the
        // reactor thread and starving coroutines on the same reactor.
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (processed->load(std::memory_order_acquire) < 3 &&
               std::chrono::steady_clock::now() < deadline)
            co_await Yield{};
    });

    pipeline.stop();  // close channels so worker coroutines exit before dispatcher stops
    // Flush: ensure post-stop pump coroutines (source_pump, pump_channels,
    // worker) are processed by the reactor before the dispatcher shuts down.
    guard.run_and_wait([]() -> Task<void> { co_return; });
    EXPECT_EQ(processed->load(), 3);
    EXPECT_EQ(sum->load(), 60);
}

// ─── 통합: with_source + stages + with_sink ──────────────────────────────────

TEST(PipelineIpcIntegration, SourceStageSinkEndToEnd) {
    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    auto result_count = std::make_shared<std::atomic<int>>(0);
    auto result_sum   = std::make_shared<std::atomic<int>>(0);
    auto sub = bus.subscribe("result",
        [result_count, result_sum](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            result_count->fetch_add(1, std::memory_order_relaxed);
            result_sum->fetch_add(std::any_cast<int>(msg), std::memory_order_relaxed);
            co_return Result<void>{};
        });

    auto pipeline = PipelineBuilder<int, int>{}
        .with_source(MessageBusSource<int>(bus, "raw", 64))
        .add<int>([](int v, ActionEnv) -> Task<Result<int>> { co_return v * 10; })
        .with_sink(MessageBusSink<int>(bus, "result"))
        .build();
    pipeline.start(guard.dispatcher);

    guard.run_and_wait([&bus, result_count]() -> Task<void> {
        std::this_thread::sleep_for(5ms);
        for (int i = 1; i <= 5; ++i)
            co_await bus.publish("raw", i);
        // Spin-wait (up to 2s) for all subscriber callbacks to complete.
        // Use co_await Yield{} instead of sleep_for to avoid blocking the
        // reactor thread and starving coroutines on the same reactor.
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (result_count->load(std::memory_order_acquire) < 5 &&
               std::chrono::steady_clock::now() < deadline)
            co_await Yield{};
    });

    pipeline.stop();  // close channels so worker coroutines exit before dispatcher stops
    // Flush: ensure post-stop pump coroutines (source_pump, pump_channels,
    // worker, sink_pump) are all processed before the dispatcher shuts down.
    guard.run_and_wait([]() -> Task<void> { co_return; });
    EXPECT_EQ(result_count->load(), 5);
    EXPECT_EQ(result_sum->load(), (1 + 2 + 3 + 4 + 5) * 10);
}
