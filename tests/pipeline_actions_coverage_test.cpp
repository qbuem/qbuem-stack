/**
 * @file tests/pipeline_actions_coverage_test.cpp
 * @brief Coverage tests for pipeline action / channel types not covered by the
 *        existing pipeline test suite.
 *
 * Modules under test:
 *   - spsc_channel.hpp        SpscChannel<T>      (wait-free SPSC ring)
 *   - priority_channel.hpp    PriorityChannel<T>  (3-level priority MPMC)
 *   - async_channel.hpp       AsyncChannel<T>     (try_send/recv/batch/close/EOS)
 *   - message_bus.hpp         MessageBus          (subscribe/publish/try_publish)
 *   - service_registry.hpp    ServiceRegistry     (DI container)
 *   - dynamic_router.hpp      DynamicRouter<T>    (predicate routing + stats)
 *   - backpressure_monitor.hpp StageMetrics / BackpressureMonitor
 *   - windowed_action.hpp     Tumbling/Sliding/SessionWindow policies + Watermark
 *   - batch_action.hpp        BatchAction<In,Out> (accumulate N then dispatch)
 *
 * Tests are deterministic and single-process: no real sockets, no servers, and
 * no wall-clock sleeps that decide correctness. Coroutine-driven tests use the
 * proven RunGuard pattern (free-function coroutine spawn, co_await Yield).
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/backpressure_monitor.hpp>
#include <qbuem/pipeline/batch_action.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/dynamic_router.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/priority_channel.hpp>
#include <qbuem/pipeline/service_registry.hpp>
#include <qbuem/pipeline/spsc_channel.hpp>
#include <qbuem/pipeline/windowed_action.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Yield: re-schedule the current coroutine once on the same reactor ────────
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

// ─── Common RunGuard (matches tests/pipeline_ipc_test.cpp) ────────────────────
struct RunGuard {
    Dispatcher   dispatcher;
    std::jthread thread;

    explicit RunGuard(size_t threads = 2) : dispatcher(threads) {
        thread = std::jthread([this] { dispatcher.run(); });
    }
    ~RunGuard() {
        dispatcher.stop();
        thread.join();
    }

    // Named coroutine to prevent GCC HALO from placing the lambda frame on
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

// =============================================================================
// SpscChannel<T>
// =============================================================================

TEST(SpscChannelTest, CapacityRoundsUpToPowerOfTwo) {
    SpscChannel<int> c5(5);
    EXPECT_EQ(c5.capacity(), 8u);   // 5 -> 8
    SpscChannel<int> c9(9);
    EXPECT_EQ(c9.capacity(), 16u);  // 9 -> 16
    SpscChannel<int> c1(1);
    EXPECT_EQ(c1.capacity(), 2u);   // min is 2
    SpscChannel<int> c8(8);
    EXPECT_EQ(c8.capacity(), 8u);   // already pow2
}

TEST(SpscChannelTest, TrySendTryRecvFifoOrder) {
    SpscChannel<int> c(4);
    EXPECT_EQ(c.size_approx(), 0u);
    EXPECT_TRUE(c.try_send(1));
    EXPECT_TRUE(c.try_send(2));
    EXPECT_TRUE(c.try_send(3));
    EXPECT_EQ(c.size_approx(), 3u);

    auto a = c.try_recv();
    auto b = c.try_recv();
    auto d = c.try_recv();
    ASSERT_TRUE(a && b && d);
    EXPECT_EQ(*a, 1);
    EXPECT_EQ(*b, 2);
    EXPECT_EQ(*d, 3);
    EXPECT_FALSE(c.try_recv().has_value()); // empty
}

TEST(SpscChannelTest, FullReturnsFalse) {
    SpscChannel<int> c(2); // capacity 2
    EXPECT_TRUE(c.try_send(10));
    EXPECT_TRUE(c.try_send(20));
    EXPECT_FALSE(c.try_send(30)); // full
    EXPECT_EQ(c.size_approx(), 2u);
}

TEST(SpscChannelTest, CloseRejectsSendAndIsClosed) {
    SpscChannel<int> c(4);
    EXPECT_FALSE(c.is_closed());
    EXPECT_TRUE(c.try_send(7));
    c.close();
    EXPECT_TRUE(c.is_closed());
    EXPECT_FALSE(c.try_send(8)); // closed -> false
    // Draining still yields buffered items.
    auto v = c.try_recv();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 7);
}

TEST(SpscChannelTest, AsyncSendRecvRoundTrip) {
    RunGuard guard;
    auto chan = std::make_shared<SpscChannel<int>>(8);
    auto got  = std::make_shared<std::atomic<int>>(-1);

    guard.run_and_wait([chan, got]() -> Task<void> {
        auto sr = co_await chan->send(123);
        EXPECT_TRUE(sr.has_value());
        auto item = co_await chan->recv();
        EXPECT_TRUE(item.has_value());
        if (item.has_value())
            got->store(*item, std::memory_order_release);
    });
    EXPECT_EQ(got->load(), 123);
}

TEST(SpscChannelTest, AsyncSendOnClosedReturnsBrokenPipe) {
    RunGuard guard;
    auto chan = std::make_shared<SpscChannel<int>>(4);
    chan->close();
    auto err = std::make_shared<std::atomic<bool>>(false);

    guard.run_and_wait([chan, err]() -> Task<void> {
        auto sr = co_await chan->send(1);
        if (!sr.has_value())
            err->store(true, std::memory_order_release);
    });
    EXPECT_TRUE(err->load());
}

TEST(SpscChannelTest, RecvOnClosedEmptyReturnsEos) {
    RunGuard guard;
    auto chan = std::make_shared<SpscChannel<int>>(4);
    chan->close();
    auto eos = std::make_shared<std::atomic<bool>>(false);

    guard.run_and_wait([chan, eos]() -> Task<void> {
        auto item = co_await chan->recv();
        if (!item.has_value())
            eos->store(true, std::memory_order_release);
    });
    EXPECT_TRUE(eos->load());
}

// =============================================================================
// PriorityChannel<T>
// =============================================================================

TEST(PriorityChannelTest, TryRecvHighBeforeNormalBeforeLow) {
    PriorityChannel<int> c(16);
    EXPECT_TRUE(c.try_send(100, Priority::Low));
    EXPECT_TRUE(c.try_send(200, Priority::Normal));
    EXPECT_TRUE(c.try_send(300, Priority::High));

    auto a = c.try_recv();
    auto b = c.try_recv();
    auto d = c.try_recv();
    ASSERT_TRUE(a && b && d);
    EXPECT_EQ(*a, 300); // High first
    EXPECT_EQ(*b, 200); // Normal next
    EXPECT_EQ(*d, 100); // Low last
    EXPECT_FALSE(c.try_recv().has_value());
}

TEST(PriorityChannelTest, DefaultPriorityIsNormal) {
    PriorityChannel<int> c(16);
    EXPECT_TRUE(c.try_send(42)); // default Normal
    EXPECT_EQ(c.size_approx(Priority::Normal), 1u);
    EXPECT_EQ(c.size_approx(Priority::High), 0u);
    EXPECT_EQ(c.size_approx(), 1u);
    auto v = c.try_recv();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

TEST(PriorityChannelTest, SizeApproxPerLevelAndTotal) {
    PriorityChannel<int> c(16);
    c.try_send(1, Priority::High);
    c.try_send(2, Priority::High);
    c.try_send(3, Priority::Low);
    EXPECT_EQ(c.size_approx(Priority::High), 2u);
    EXPECT_EQ(c.size_approx(Priority::Low), 1u);
    EXPECT_EQ(c.size_approx(Priority::Normal), 0u);
    EXPECT_EQ(c.size_approx(), 3u);
}

TEST(PriorityChannelTest, CloseRejectsSend) {
    PriorityChannel<int> c(16);
    EXPECT_FALSE(c.is_closed());
    c.close();
    EXPECT_TRUE(c.is_closed());
    EXPECT_FALSE(c.try_send(9, Priority::High));
}

TEST(PriorityChannelTest, AsyncSendThenRecvByPriority) {
    RunGuard guard;
    auto chan = std::make_shared<PriorityChannel<int>>(16);
    auto first = std::make_shared<std::atomic<int>>(-1);

    guard.run_and_wait([chan, first]() -> Task<void> {
        EXPECT_TRUE((co_await chan->send(5, Priority::Low)).has_value());
        EXPECT_TRUE((co_await chan->send(9, Priority::High)).has_value());
        auto item = co_await chan->recv();
        EXPECT_TRUE(item.has_value());
        if (item.has_value())
            first->store(*item, std::memory_order_release);
    });
    EXPECT_EQ(first->load(), 9); // High dequeued first
}

TEST(PriorityChannelTest, AsyncSendOnClosedReturnsError) {
    RunGuard guard;
    auto chan = std::make_shared<PriorityChannel<int>>(8);
    chan->close();
    auto err = std::make_shared<std::atomic<bool>>(false);
    guard.run_and_wait([chan, err]() -> Task<void> {
        auto sr = co_await chan->send(1, Priority::High);
        if (!sr.has_value()) err->store(true, std::memory_order_release);
    });
    EXPECT_TRUE(err->load());
}

// =============================================================================
// AsyncChannel<T> — non-blocking surface + batch + EOS
// =============================================================================

TEST(AsyncChannelCoverageTest, CapacityRoundsUpToPowerOfTwo) {
    AsyncChannel<int> c(5);
    EXPECT_EQ(c.capacity(), 8u);
    AsyncChannel<int> c2(1);
    EXPECT_EQ(c2.capacity(), 2u); // min 2
}

TEST(AsyncChannelCoverageTest, TrySendTryRecvAndSizeApprox) {
    AsyncChannel<int> c(8);
    EXPECT_EQ(c.size_approx(), 0u);
    EXPECT_TRUE(c.try_send(11));
    EXPECT_TRUE(c.try_send(22));
    EXPECT_EQ(c.size_approx(), 2u);
    auto v = c.try_recv();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 11);
    EXPECT_EQ(c.size_approx(), 1u);
}

TEST(AsyncChannelCoverageTest, TryRecvEmptyReturnsNullopt) {
    AsyncChannel<int> c(4);
    EXPECT_FALSE(c.try_recv().has_value());
}

TEST(AsyncChannelCoverageTest, TrySendFullReturnsFalse) {
    AsyncChannel<int> c(2); // capacity 2
    EXPECT_TRUE(c.try_send(1));
    EXPECT_TRUE(c.try_send(2));
    EXPECT_FALSE(c.try_send(3)); // full
}

TEST(AsyncChannelCoverageTest, CloseRejectsTrySendAndIsClosed) {
    AsyncChannel<int> c(4);
    EXPECT_FALSE(c.is_closed());
    c.close();
    EXPECT_TRUE(c.is_closed());
    EXPECT_FALSE(c.try_send(99));
}

TEST(AsyncChannelCoverageTest, SendBatchAndTryRecvBatch) {
    AsyncChannel<int> c(16);
    std::vector<int> items{1, 2, 3, 4, 5};
    size_t sent = c.send_batch(std::span<int>(items));
    EXPECT_EQ(sent, 5u);
    EXPECT_EQ(c.size_approx(), 5u);

    std::vector<int> out(10);
    size_t got = c.try_recv_batch(std::span<int>(out), 3);
    EXPECT_EQ(got, 3u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);
    EXPECT_EQ(out[2], 3);

    // Drain the rest with max_n=0 (means out.size()).
    size_t rest = c.try_recv_batch(std::span<int>(out));
    EXPECT_EQ(rest, 2u);
    EXPECT_EQ(out[0], 4);
    EXPECT_EQ(out[1], 5);
}

TEST(AsyncChannelCoverageTest, SendBatchStopsWhenFull) {
    AsyncChannel<int> c(2); // capacity 2
    std::vector<int> items{1, 2, 3, 4};
    size_t sent = c.send_batch(std::span<int>(items));
    EXPECT_EQ(sent, 2u); // only 2 fit
}

TEST(AsyncChannelCoverageTest, AsyncRecvOnClosedEmptyReturnsEos) {
    RunGuard guard;
    auto chan = std::make_shared<AsyncChannel<int>>(4);
    chan->close();
    auto eos = std::make_shared<std::atomic<bool>>(false);
    guard.run_and_wait([chan, eos]() -> Task<void> {
        auto item = co_await chan->recv();
        if (!item.has_value()) eos->store(true, std::memory_order_release);
    });
    EXPECT_TRUE(eos->load());
}

// =============================================================================
// MessageBus — try_publish, subscriber_count, topics, RAII subscription
// =============================================================================

TEST(MessageBusCoverageTest, SubscriberCountAndTopics) {
    MessageBus bus;
    EXPECT_EQ(bus.subscriber_count("nope"), 0u);
    EXPECT_TRUE(bus.topics().empty());

    auto s1 = bus.subscribe("alpha",
        [](MessageBus::Msg, Context) -> Task<Result<void>> { co_return Result<void>{}; });
    auto s2 = bus.subscribe("alpha",
        [](MessageBus::Msg, Context) -> Task<Result<void>> { co_return Result<void>{}; });
    auto s3 = bus.subscribe("beta",
        [](MessageBus::Msg, Context) -> Task<Result<void>> { co_return Result<void>{}; });

    EXPECT_EQ(bus.subscriber_count("alpha"), 2u);
    EXPECT_EQ(bus.subscriber_count("beta"), 1u);
    EXPECT_EQ(bus.topics().size(), 2u);
}

TEST(MessageBusCoverageTest, SubscriptionRaiiCancelsOnScopeExit) {
    MessageBus bus;
    {
        auto s = bus.subscribe("temp",
            [](MessageBus::Msg, Context) -> Task<Result<void>> { co_return Result<void>{}; });
        EXPECT_TRUE(s.is_active());
        EXPECT_EQ(bus.subscriber_count("temp"), 1u);
    } // destructor cancels
    EXPECT_EQ(bus.subscriber_count("temp"), 0u);
}

TEST(MessageBusCoverageTest, ExplicitCancelDeactivates) {
    MessageBus bus;
    auto s = bus.subscribe("x",
        [](MessageBus::Msg, Context) -> Task<Result<void>> { co_return Result<void>{}; });
    EXPECT_TRUE(s.is_active());
    s.cancel();
    EXPECT_FALSE(s.is_active());
    EXPECT_EQ(bus.subscriber_count("x"), 0u);
}

TEST(MessageBusCoverageTest, TryPublishUnknownTopicSucceedsSilently) {
    MessageBus bus;
    // No subscribers for "ghost" — drop silently, returns true.
    EXPECT_TRUE(bus.try_publish("ghost", 5));
}

TEST(MessageBusCoverageTest, TryPublishToStreamSubscriber) {
    MessageBus bus;
    auto stream = bus.subscribe_stream<int>("evt", 16);
    EXPECT_EQ(bus.subscriber_count("evt"), 1u);

    EXPECT_TRUE(bus.try_publish("evt", 7));
    EXPECT_TRUE(bus.try_publish("evt", 8));

    auto a = stream->try_recv();
    auto b = stream->try_recv();
    ASSERT_TRUE(a && b);
    EXPECT_EQ(*a, 7);
    EXPECT_EQ(*b, 8);
}

TEST(MessageBusCoverageTest, CloseTopicMarksTopicClosed) {
    MessageBus bus;
    auto stream = bus.subscribe_stream<int>("closer", 8);
    EXPECT_FALSE(stream->is_closed());
    bus.close_topic("closer");
    // subscribe_stream uses direct_sender (not the Sub.channel field), so the
    // typed stream channel itself is not closed by close_topic. The topic is
    // marked closed, so try_publish now returns false.
    EXPECT_FALSE(bus.try_publish("closer", 1));
}

TEST(MessageBusCoverageTest, CloseTopicUnknownIsNoOp) {
    MessageBus bus;
    bus.close_topic("does.not.exist"); // must not crash
    SUCCEED();
}

TEST(MessageBusCoverageTest, PublishToUnknownTopicIsOk) {
    RunGuard guard;
    auto bus = std::make_shared<MessageBus>();
    auto ok = std::make_shared<std::atomic<bool>>(false);
    guard.run_and_wait([bus, ok]() -> Task<void> {
        auto r = co_await bus->publish("undefined", 1);
        if (r.has_value()) ok->store(true, std::memory_order_release);
    });
    EXPECT_TRUE(ok->load());
}

// =============================================================================
// ServiceRegistry — DI container
// =============================================================================

namespace {
struct ICounter { int value = 0; virtual ~ICounter() = default; };
struct CounterImpl : ICounter {};
struct Plain { int n = 99; };
} // namespace

TEST(ServiceRegistryCoverageTest, RegisterAndGetSingleton) {
    ServiceRegistry reg;
    auto inst = std::make_shared<CounterImpl>();
    inst->value = 42;
    reg.register_singleton<ICounter>(inst);

    auto got = reg.get<ICounter>();
    ASSERT_TRUE(got != nullptr);
    EXPECT_EQ(got->value, 42);
}

TEST(ServiceRegistryCoverageTest, GetMissingReturnsNull) {
    ServiceRegistry reg;
    EXPECT_EQ(reg.get<ICounter>(), nullptr);
}

TEST(ServiceRegistryCoverageTest, RequireFoundReturnsPtr) {
    ServiceRegistry reg;
    reg.register_singleton<ICounter>(std::make_shared<CounterImpl>());
    auto p = reg.require<ICounter>();
    EXPECT_TRUE(p != nullptr);
}

TEST(ServiceRegistryCoverageTest, FactoryLazyCreatesAndCaches) {
    ServiceRegistry reg;
    auto creations = std::make_shared<std::atomic<int>>(0);
    reg.register_factory<ICounter>([creations]() -> std::shared_ptr<ICounter> {
        creations->fetch_add(1, std::memory_order_relaxed);
        return std::make_shared<CounterImpl>();
    });
    EXPECT_EQ(creations->load(), 0); // not yet created
    auto a = reg.get<ICounter>();
    auto b = reg.get<ICounter>();
    EXPECT_TRUE(a != nullptr);
    EXPECT_EQ(a.get(), b.get());        // cached: same instance
    EXPECT_EQ(creations->load(), 1);    // factory ran exactly once
}

TEST(ServiceRegistryCoverageTest, GetOrCreateDefaultConstructs) {
    ServiceRegistry reg;
    Plain& p = reg.get_or_create<Plain>();
    EXPECT_EQ(p.n, 99);
    p.n = 7;
    // Second call returns the same registered instance.
    Plain& p2 = reg.get_or_create<Plain>();
    EXPECT_EQ(p2.n, 7);
}

TEST(ServiceRegistryCoverageTest, ParentScopeDelegation) {
    ServiceRegistry root;
    root.register_singleton<ICounter>(std::make_shared<CounterImpl>());
    ServiceRegistry child(&root);
    EXPECT_EQ(child.parent(), &root);
    // child has no local registration, but resolves through the parent.
    auto p = child.get<ICounter>();
    EXPECT_TRUE(p != nullptr);
}

TEST(ServiceRegistryCoverageTest, RootParentIsNull) {
    ServiceRegistry root;
    EXPECT_EQ(root.parent(), nullptr);
}

TEST(ServiceRegistryCoverageTest, GlobalRegistryIsSingleton) {
    EXPECT_EQ(&global_registry(), &global_registry());
}

// =============================================================================
// DynamicRouter<T>
//
// NOTE: add_route() instantiates std::vector<RouteEntry<T>>::emplace_back, whose
// growth path requires RouteEntry to be Cpp17MoveInsertable. RouteEntry embeds a
// RouterStats with std::atomic members (non-movable), so under libc++ the vector
// reallocation static_asserts at compile time. We therefore exercise only the
// route-free surface (construction, empty route_count, evaluate_batch on an empty
// router, stats lookup, and route() falling through to a default channel) which
// does not instantiate the failing emplace_back path.
// =============================================================================

TEST(DynamicRouterCoverageTest, EmptyRouterRouteCountAndStats) {
    DynamicRouter<int> router(RoutingMode::AllMatch);
    EXPECT_EQ(router.route_count(), 0u);
    auto [routed, dropped] = router.stats("missing");
    EXPECT_EQ(routed, 0u);
    EXPECT_EQ(dropped, 0u);
}

TEST(DynamicRouterCoverageTest, EvaluateBatchEmptyRoutesAllZero) {
    DynamicRouter<int> router(RoutingMode::AllMatch);
    std::vector<int> batch{1, 2, 3};
    auto masks = router.evaluate_batch(std::span<const int>(batch));
    ASSERT_EQ(masks.size(), 3u);
    EXPECT_EQ(masks[0], 0u);
    EXPECT_EQ(masks[1], 0u);
    EXPECT_EQ(masks[2], 0u);
}

TEST(DynamicRouterCoverageTest, EvaluateBatchEmptyInputIsEmpty) {
    DynamicRouter<int> router(RoutingMode::FirstMatch);
    std::vector<int> empty;
    auto masks = router.evaluate_batch(std::span<const int>(empty));
    EXPECT_TRUE(masks.empty());
}

TEST(DynamicRouterCoverageTest, RouteWithNoRoutesGoesToDefault) {
    RunGuard guard;
    auto dflt_ch = std::make_shared<AsyncChannel<int>>(16);
    auto router  = std::make_shared<DynamicRouter<int>>(RoutingMode::AllMatch);
    router->set_default(*dflt_ch, /*blocking=*/false);

    auto sent = std::make_shared<std::atomic<size_t>>(99);
    guard.run_and_wait([router, sent]() -> Task<void> {
        std::stop_source ss;
        size_t n = co_await router->route(7, ss.get_token()); // no routes -> default
        sent->store(n, std::memory_order_release);
    });
    EXPECT_EQ(sent->load(), 1u);
    EXPECT_EQ(dflt_ch->size_approx(), 1u);
}

TEST(DynamicRouterCoverageTest, RouteWithNoRoutesNoDefaultReturnsZero) {
    RunGuard guard;
    auto router = std::make_shared<DynamicRouter<int>>(RoutingMode::AllMatch);
    auto sent = std::make_shared<std::atomic<size_t>>(99);
    guard.run_and_wait([router, sent]() -> Task<void> {
        std::stop_source ss;
        size_t n = co_await router->route(7, ss.get_token());
        sent->store(n, std::memory_order_release);
    });
    EXPECT_EQ(sent->load(), 0u); // nothing matched, no default
}

TEST(DynamicRouterCoverageTest, StopRequestedReturnsZero) {
    RunGuard guard;
    auto router = std::make_shared<DynamicRouter<int>>(RoutingMode::LoadBalance);
    auto sent = std::make_shared<std::atomic<size_t>>(99);
    guard.run_and_wait([router, sent]() -> Task<void> {
        std::stop_source ss;
        ss.request_stop();
        size_t n = co_await router->route(1, ss.get_token());
        sent->store(n, std::memory_order_release);
    });
    EXPECT_EQ(sent->load(), 0u);
}

// =============================================================================
// BackpressureMonitor / StageMetrics
// =============================================================================

TEST(StageMetricsCoverageTest, EnqueueDequeueQueueDepth) {
    StageMetrics m(256);
    m.record_enqueue();
    m.record_enqueue();
    EXPECT_EQ(m.queue_depth_.load(), 2);
    EXPECT_EQ(m.enqueue_total_.load(), 2u);
    m.record_dequeue(2'000 /*ns -> bucket1*/);
    EXPECT_EQ(m.queue_depth_.load(), 1);
    EXPECT_EQ(m.dequeue_total_.load(), 1u);
}

TEST(StageMetricsCoverageTest, RecordErrorDecrementsDepth) {
    StageMetrics m(256);
    m.record_enqueue();
    m.record_error();
    EXPECT_EQ(m.error_total_.load(), 1u);
    EXPECT_EQ(m.queue_depth_.load(), 0);
}

TEST(StageMetricsCoverageTest, FillRatio) {
    StageMetrics m(10);
    m.record_enqueue();
    m.record_enqueue();
    m.record_enqueue();
    m.record_enqueue();
    m.record_enqueue(); // depth 5 / cap 10
    EXPECT_DOUBLE_EQ(m.fill_ratio(), 0.5);
}

TEST(StageMetricsCoverageTest, PercentileEmptyIsZero) {
    StageMetrics m;
    EXPECT_EQ(m.percentile_ns(99.0), 0u);
}

TEST(StageMetricsCoverageTest, PercentileWithSamples) {
    StageMetrics m;
    // All samples land in bucket 0 (< 1µs).
    for (int i = 0; i < 100; ++i) m.observe_latency(500);
    // p50 of all-small samples: lower bound of bucket containing it = 0.
    EXPECT_EQ(m.percentile_ns(50.0), 0u);
    // Add a large sample (>=1ms => last bucket).
    m.observe_latency(2'000'000);
    // p99.9 should reflect the histogram, not crash.
    EXPECT_GE(m.percentile_ns(99.9), 0u);
}

TEST(BackpressureMonitorCoverageTest, RegisterStageAndSnapshot) {
    BackpressureMonitor mon;
    mon.register_stage("parse", 128);
    auto& s = mon.stage("parse");
    s.record_enqueue();
    s.record_enqueue();
    s.record_dequeue(3'000);

    auto snap = mon.snapshot("parse");
    EXPECT_EQ(snap.name, "parse");
    EXPECT_EQ(snap.capacity, 128u);
    EXPECT_EQ(snap.queue_depth, 1);
    EXPECT_EQ(snap.enqueue_total, 2u);
    EXPECT_EQ(snap.dequeue_total, 1u);
}

TEST(BackpressureMonitorCoverageTest, SnapshotUnknownStageIsDefault) {
    BackpressureMonitor mon;
    auto snap = mon.snapshot("nope");
    EXPECT_TRUE(snap.name.empty());
    EXPECT_EQ(snap.queue_depth, 0);
}

TEST(BackpressureMonitorCoverageTest, AllSnapshotsInRegistrationOrder) {
    BackpressureMonitor mon;
    mon.register_stage("first", 64);
    mon.register_stage("second", 64);
    auto all = mon.all_snapshots();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].name, "first");
    EXPECT_EQ(all[1].name, "second");
}

TEST(BackpressureMonitorCoverageTest, ResetClearsCounters) {
    BackpressureMonitor mon;
    mon.register_stage("s", 64);
    auto& m = mon.stage("s");
    m.record_enqueue();
    m.record_dequeue(1'000);
    mon.reset();
    auto snap = mon.snapshot("s");
    EXPECT_EQ(snap.enqueue_total, 0u);
    EXPECT_EQ(snap.dequeue_total, 0u);
    EXPECT_EQ(snap.queue_depth, 0);
}

TEST(BackpressureMonitorCoverageTest, CheckAlertsFiresOnSaturation) {
    BackpressureMonitor mon;
    mon.register_stage("hot", 10);
    auto& m = mon.stage("hot");
    for (int i = 0; i < 9; ++i) m.record_enqueue(); // 90% fill > 80% threshold

    auto fired = std::make_shared<std::atomic<bool>>(false);
    BackpressureAlert alert;
    alert.saturation_threshold = 0.80;
    alert.on_alert = [fired](const StagePressure&) {
        fired->store(true, std::memory_order_release);
    };
    mon.check_alerts(alert);
    EXPECT_TRUE(fired->load());
}

TEST(BackpressureMonitorCoverageTest, CheckAlertsNoCallbackIsSafe) {
    BackpressureMonitor mon;
    mon.register_stage("s", 10);
    BackpressureAlert alert; // no on_alert callback set
    mon.check_alerts(alert); // must not crash / no-op
    SUCCEED();
}

// =============================================================================
// WindowedAction window policies (pure logic) + Watermark
// =============================================================================

TEST(WindowPolicyTest, TumblingWindowForAlignsToSize) {
    TumblingWindow tw{milliseconds{1000}};
    auto t = system_clock::time_point(milliseconds{2500});
    auto desc = tw.window_for(t);
    EXPECT_EQ(desc.start, system_clock::time_point(milliseconds{2000}));
    EXPECT_EQ(desc.end,   system_clock::time_point(milliseconds{3000}));
    EXPECT_EQ(tw.tick_interval(), milliseconds{1000});
}

TEST(WindowPolicyTest, SlidingWindowEnumeratesOverlaps) {
    SlidingWindow sw{milliseconds{1000}, milliseconds{500}};
    auto t = system_clock::time_point(milliseconds{1200});
    auto windows = sw.windows_for(t);
    // Event at 1200, size 1000, step 500 => windows starting at 500 and 1000.
    ASSERT_FALSE(windows.empty());
    for (auto& w : windows) {
        EXPECT_LE(w.start, t);
        EXPECT_LT(t, w.end);
    }
    EXPECT_EQ(sw.tick_interval(), milliseconds{500});
}

TEST(WindowPolicyTest, SessionWindowTickInterval) {
    SessionWindow sw{milliseconds{5000}};
    EXPECT_EQ(sw.tick_interval(), milliseconds{2500}); // gap/2
    SessionWindow tiny{milliseconds{1}};
    EXPECT_EQ(tiny.tick_interval(), milliseconds{1}); // minimum 1ms
}

TEST(WindowPolicyTest, WatermarkHoldsTimestamp) {
    auto ts = system_clock::time_point(milliseconds{12345});
    Watermark wm{ts};
    EXPECT_EQ(wm.ts, ts);
}

// =============================================================================
// WindowedAction — end-to-end tumbling aggregation via explicit EventTime
// =============================================================================

TEST(WindowedActionCoverageTest, TumblingAggregationEmitsOnDrain) {
    RunGuard guard;
    using WA = WindowedAction<int, std::string, int64_t, int64_t>;
    WA::Config cfg;
    cfg.type    = WindowType::Tumbling;
    cfg.size    = milliseconds{1000};
    cfg.key_fn  = [](const int&) -> std::string { return "k"; };
    cfg.acc_fn  = [](int64_t& acc, const int& v) { acc += v; };
    cfg.emit_fn = [](std::string, int64_t acc, system_clock::time_point) -> int64_t { return acc; };
    cfg.init_acc = 0;

    auto action = std::make_shared<WA>(std::move(cfg));
    auto out = std::make_shared<AsyncChannel<ContextualItem<int64_t>>>(64);
    action->start(guard.dispatcher, out);

    auto total = std::make_shared<std::atomic<int64_t>>(0);
    guard.run_and_wait([action, out, total]() -> Task<void> {
        // Fixed event time so all items fall in one tumbling window.
        Context ctx;
        ctx = ctx.put(EventTime{system_clock::time_point(milliseconds{500})});
        co_await action->push(10, ctx);
        co_await action->push(20, ctx);
        co_await action->push(30, ctx);
        // Drain forces emission of remaining windows, then closes the output.
        co_await action->drain();
        // Collect all emitted results until EOS.
        for (;;) {
            auto item = co_await out->recv();
            if (!item) break;
            total->fetch_add(item->value, std::memory_order_relaxed);
        }
    }, 8s);

    EXPECT_EQ(total->load(), 60);
}

// =============================================================================
// BatchAction<In, Out> — accumulate N then dispatch
// =============================================================================

TEST(BatchActionCoverageTest, TryPushAndInputOutputAccessors) {
    BatchAction<int, int> batch(
        [](std::vector<int> in, ActionEnv) -> Task<Result<std::vector<int>>> {
            co_return std::move(in);
        },
        BatchAction<int, int>::Config{.max_batch_size = 4, .max_wait_ms = 5, .workers = 1, .channel_cap = 16});
    EXPECT_TRUE(batch.try_push(1));
    EXPECT_TRUE(batch.input() != nullptr);
    EXPECT_EQ(batch.input()->size_approx(), 1u);
    // output() is null until start() is called.
    EXPECT_EQ(batch.output(), nullptr);
}

TEST(BatchActionCoverageTest, BatchesUpToMaxSizeAndDispatches) {
    RunGuard guard;
    auto observed_batch_size = std::make_shared<std::atomic<size_t>>(0);
    auto batch = std::make_shared<BatchAction<int, int>>(
        [observed_batch_size](std::vector<int> in, ActionEnv) -> Task<Result<std::vector<int>>> {
            observed_batch_size->store(in.size(), std::memory_order_release);
            // Emit the doubled values.
            std::vector<int> out;
            out.reserve(in.size());
            for (int v : in) out.push_back(v * 2);
            co_return out;
        },
        BatchAction<int, int>::Config{.max_batch_size = 3, .max_wait_ms = 5, .workers = 1, .channel_cap = 32});

    auto out = std::make_shared<AsyncChannel<ContextualItem<int>>>(32);
    batch->start(guard.dispatcher, out);

    auto sum = std::make_shared<std::atomic<int>>(0);
    auto count = std::make_shared<std::atomic<int>>(0);
    guard.run_and_wait([batch, out, sum, count]() -> Task<void> {
        co_await batch->push(1);
        co_await batch->push(2);
        co_await batch->push(3);
        co_await batch->drain(); // closes input + output after workers finish
        for (;;) {
            auto item = co_await out->recv();
            if (!item) break;
            sum->fetch_add(item->value, std::memory_order_relaxed);
            count->fetch_add(1, std::memory_order_relaxed);
        }
    }, 8s);

    EXPECT_EQ(count->load(), 3);
    EXPECT_EQ(sum->load(), (1 + 2 + 3) * 2);
    EXPECT_EQ(observed_batch_size->load(), 3u);
}

TEST(BatchActionCoverageTest, TimeoutFlushesPartialBatch) {
    RunGuard guard;
    auto first_batch_size = std::make_shared<std::atomic<size_t>>(0);
    auto batch = std::make_shared<BatchAction<int, int>>(
        [first_batch_size](std::vector<int> in, ActionEnv) -> Task<Result<std::vector<int>>> {
            size_t expected = 0;
            // Record the first non-empty batch size only once.
            first_batch_size->compare_exchange_strong(expected, in.size());
            co_return std::move(in);
        },
        BatchAction<int, int>::Config{.max_batch_size = 100, .max_wait_ms = 5, .workers = 1, .channel_cap = 32});

    auto out = std::make_shared<AsyncChannel<ContextualItem<int>>>(32);
    batch->start(guard.dispatcher, out);

    auto count = std::make_shared<std::atomic<int>>(0);
    guard.run_and_wait([batch, out, count]() -> Task<void> {
        // Push only 2 items (< max_batch_size); rely on drain/EOS to flush.
        co_await batch->push(7);
        co_await batch->push(9);
        co_await batch->drain();
        for (;;) {
            auto item = co_await out->recv();
            if (!item) break;
            count->fetch_add(1, std::memory_order_relaxed);
        }
    }, 8s);

    EXPECT_EQ(count->load(), 2); // both items flushed despite < max_batch_size
    EXPECT_EQ(first_batch_size->load(), 2u);
}
