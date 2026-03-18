/**
 * @file ipc_pipeline_example.cpp
 * @brief Composite example: Pipeline ↔ MessageBus ↔ SHMChannel integration.
 *
 * ## Scenario: Real-time Order Processing System (IPC Pipeline)
 *
 * Connects all three messaging layers of qbuem-stack:
 *
 *  [External Input Simulator]
 *       ↓ SHMChannel (inter-process shared memory)
 *  [SHMSource → PipelineBuilder::with_source()]
 *       ↓ StaticPipeline (parse → enrich → validate)
 *  [PipelineBuilder::with_sink() → MessageBusSink]
 *       ↓ MessageBus pub/sub (intra-process topic routing)
 *  [MessageBusSource → PipelineBuilder::with_source()]
 *       ↓ DynamicPipeline (risk_check → record)
 *  [output channel direct consumption]
 *
 * ## Coverage
 * - SHMSource<T>: init() + next() (fixed-size array ownership)
 * - SHMSink<T>:   init() + sink() (fixed-size array ownership)
 * - PipelineBuilder::with_source(): SHMSource connection
 * - PipelineBuilder::with_sink():   MessageBusSink connection
 * - MessageBusSource<T>: MessageBus topic → Pipeline source
 * - MessageBusSink<T>:   Pipeline tail → MessageBus topic publish
 * - SHMChannel direct usage (inter-process data supply)
 * - Mixed StaticPipeline + DynamicPipeline architecture
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/shm/shm_bus.hpp>
#include <qbuem/shm/shm_channel.hpp>

#include <qbuem/compat/print.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace qbuem::shm;
using namespace std::chrono_literals;
using std::println;

// ─── Domain Types ─────────────────────────────────────────────────────────────

/// @brief SHMChannel-compatible — trivially_copyable (fixed-size array)
struct RawOrder {
    uint64_t order_id;
    char     symbol[16];  ///< Fixed size (std::string not allowed — SHM requirement)
    double   price;
    int      qty;
};
static_assert(std::is_trivially_copyable_v<RawOrder>,
              "RawOrder must be trivially copyable for SHMChannel");

struct ParsedOrder {
    uint64_t    order_id;
    std::string symbol;   ///< Inside the pipeline, std::string is fine
    double      price;
    int         qty;
    bool        valid;
    std::string error_msg;
};

struct ValidatedOrder {
    ParsedOrder base;
    double      notional;   // price × qty
    bool        risk_ok;
};

// ─── Pipeline Stages ─────────────────────────────────────────────────────────

static std::atomic<int> g_parsed{0};
static std::atomic<int> g_validated{0};
static std::atomic<int> g_risk_checked{0};
static std::atomic<int> g_recorded{0};

static Task<Result<ParsedOrder>> stage_parse(RawOrder raw, ActionEnv /*env*/) {
    ParsedOrder p;
    p.order_id = raw.order_id;
    p.symbol   = std::string(raw.symbol);  // char[16] → std::string
    p.price    = raw.price;
    p.qty      = raw.qty;
    p.valid    = (raw.price > 0 && raw.qty > 0 && raw.symbol[0] != '\0');
    if (!p.valid) p.error_msg = "invalid price/qty/symbol";
    g_parsed.fetch_add(1, std::memory_order_relaxed);
    co_return p;
}

static Task<Result<ParsedOrder>> stage_enrich(ParsedOrder p, ActionEnv /*env*/) {
    // Normalize symbol to uppercase (in production: master data lookup)
    for (auto& c : p.symbol) c = static_cast<char>(toupper(c));
    co_return p;
}

static Task<Result<ValidatedOrder>> stage_validate(ParsedOrder p, ActionEnv /*env*/) {
    ValidatedOrder v;
    v.base     = p;
    v.notional = p.price * static_cast<double>(p.qty);
    v.risk_ok  = (v.notional < 10'000'000.0);  // only orders below 10M pass
    g_validated.fetch_add(1, std::memory_order_relaxed);
    co_return v;
}

// DynamicPipeline stages (ValidatedOrder → ValidatedOrder, homogeneous type)
static Task<Result<ValidatedOrder>> stage_risk_check(ValidatedOrder v, ActionEnv /*env*/) {
    if (!v.risk_ok)
        co_return unexpected(std::make_error_code(std::errc::value_too_large));
    g_risk_checked.fetch_add(1, std::memory_order_relaxed);
    co_return v;
}

static Task<Result<ValidatedOrder>> stage_record(ValidatedOrder v, ActionEnv /*env*/) {
    println("  [record] id={} symbol={} qty={} price={:.0f} notional={:.0f}",
            v.base.order_id, v.base.symbol, v.base.qty, v.base.price, v.notional);
    g_recorded.fetch_add(1, std::memory_order_relaxed);
    co_return v;
}

// ─── RunGuard ─────────────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher  dispatcher;
    std::jthread thread;
    explicit RunGuard(size_t n = 2) : dispatcher(n) {
        thread = std::jthread([this] { dispatcher.run(); });
    }
    ~RunGuard() { dispatcher.stop(); if (thread.joinable()) thread.join(); }
    template <typename F>
    void run_and_wait(F&& f, std::chrono::milliseconds timeout = 10s) {
        std::atomic<bool> done{false};
        dispatcher.spawn([&, f = std::forward<F>(f)]() mutable -> Task<void> {
            co_await f(); done.store(true, std::memory_order_release);
        }());
        auto dl = std::chrono::steady_clock::now() + timeout;
        while (!done.load() && std::chrono::steady_clock::now() < dl)
            std::this_thread::sleep_for(1ms);
    }
};

// ─── Scenario 1: SHMChannel → SHMSource → StaticPipeline ────────────────────

static void scenario_shm_source_to_pipeline() {
    println("\n=== Scenario 1: SHMChannel -> SHMSource -> StaticPipeline ===");
    g_parsed.store(0); g_validated.store(0);

    const char* channel_name = "qbuem_orders_test";

    // 1) Create SHM channel (producer side)
    auto shm_chan = SHMChannel<RawOrder>::create(channel_name, 32);
    if (!shm_chan) {
        println("[SKIP] SHMChannel creation failed: {}", shm_chan.error().message());
        return;
    }

    // 2) StaticPipeline with SHMSource at head
    RunGuard guard;

    auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
        .with_source(SHMSource<RawOrder>(channel_name))
        .add<ParsedOrder>(stage_parse)
        .add<ParsedOrder>(stage_enrich)
        .add<ValidatedOrder>(stage_validate)
        .build();

    pipeline.start(guard.dispatcher);

    // 3) Write orders to SHM channel (simulates data from another process)
    std::vector<RawOrder> orders = {
        {1001, "samsung", 72500.0, 10},
        {1002, "sk_hynix", 93000.0, 5},
        {1003, "kakao",    -1.0,   0},   // invalid
        {1004, "naver",    210000.0, 3},
    };

    guard.run_and_wait([&]() -> Task<void> {
        for (auto& ord : orders) {
            (*shm_chan)->try_send(ord);
        }
        // Wait for pipeline to finish processing
        std::this_thread::sleep_for(30ms);
    });

    println("[result] parsed={} validated={}", g_parsed.load(), g_validated.load());

    // Cleanup SHM channel
    (*shm_chan)->close();
    SHMChannel<RawOrder>::unlink(channel_name);
}

// ─── Scenario 2: StaticPipeline → MessageBusSink → MessageBus publish ────────

static void scenario_pipeline_to_messagebus() {
    println("\n=== Scenario 2: StaticPipeline -> MessageBusSink -> MessageBus ===");
    g_parsed.store(0); g_validated.store(0);

    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    // MessageBus subscriber (receives ValidatedOrder)
    std::atomic<int> received{0};
    auto sub = bus.subscribe("validated_orders",
        [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            auto& v = std::any_cast<const ValidatedOrder&>(msg);
            received.fetch_add(1, std::memory_order_relaxed);
            println("  [subscriber] order received: id={} notional={:.0f}",
                    v.base.order_id, v.notional);
            co_return Result<void>{};
        });

    // StaticPipeline: with_sink() → MessageBusSink
    auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
        .add<ParsedOrder>(stage_parse)
        .add<ParsedOrder>(stage_enrich)
        .add<ValidatedOrder>(stage_validate)
        .with_sink(MessageBusSink<ValidatedOrder>(bus, "validated_orders"))
        .build();

    pipeline.start(guard.dispatcher);

    // Push orders
    guard.run_and_wait([&]() -> Task<void> {
        co_await pipeline.push(RawOrder{2001, "lg_elec", 105000.0, 20});
        co_await pipeline.push(RawOrder{2002, "posco",   382000.0, 3});
        co_await pipeline.push(RawOrder{2003, "hyundai", 205000.0, 8});
        // Wait for processing to complete
        std::this_thread::sleep_for(30ms);
    });

    println("[result] parsed={} validated={} subscriber_received={}",
            g_parsed.load(), g_validated.load(), received.load());
}

// ─── Scenario 3: MessageBusSource → StaticPipeline ───────────────────────────

static void scenario_messagebus_source_to_pipeline() {
    println("\n=== Scenario 3: MessageBusSource -> StaticPipeline ===");
    g_parsed.store(0); g_validated.store(0);

    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    // StaticPipeline: MessageBusSource at head
    auto pipeline = PipelineBuilder<ValidatedOrder, ValidatedOrder>{}
        .with_source(MessageBusSource<ValidatedOrder>(bus, "raw_validated"))
        .add<ValidatedOrder>(stage_risk_check)
        .add<ValidatedOrder>(stage_record)
        .build();

    pipeline.start(guard.dispatcher);

    // Supply data via bus.publish()
    guard.run_and_wait([&]() -> Task<void> {
        std::this_thread::sleep_for(5ms);  // wait for source to initialize

        co_await bus.publish("raw_validated",
            ValidatedOrder{{3001, "SAMSUNG", 72500.0, 10, true, ""}, 725000.0, true});
        co_await bus.publish("raw_validated",
            ValidatedOrder{{3002, "SK_HYNIX", 93000.0, 50, true, ""}, 4650000.0, true});
        co_await bus.publish("raw_validated",
            ValidatedOrder{{3003, "KAKAO", 55000.0, 200, true, ""}, 11000000.0, false}); // risk fail

        std::this_thread::sleep_for(30ms);
    });

    println("[result] risk_checked={} recorded={} (qty>200 -> risk rejected)",
            g_risk_checked.load(), g_recorded.load());
}

// ─── Scenario 4: Full Integration Pipeline ───────────────────────────────────
//
// RawOrder (push) → StaticPipeline (parse+enrich+validate)
//                 → MessageBusSink("stage1_out")
//                 → MessageBus fan-out
//                 → MessageBusSource("stage1_out") → DynamicPipeline (risk+record)

static void scenario_full_integration() {
    println("\n=== Scenario 4: Full Integration (StaticPipeline -> MessageBus -> DynamicPipeline) ===");
    g_parsed.store(0); g_validated.store(0);
    g_risk_checked.store(0); g_recorded.store(0);

    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    // Stage1: StaticPipeline (parse/enrich/validate) + MessageBusSink
    auto stage1 = PipelineBuilder<RawOrder, RawOrder>{}
        .add<ParsedOrder>(stage_parse)
        .add<ParsedOrder>(stage_enrich)
        .add<ValidatedOrder>(stage_validate)
        .with_sink(MessageBusSink<ValidatedOrder>(bus, "stage1_output"))
        .build();
    stage1.start(guard.dispatcher);

    // Stage2: DynamicPipeline (risk_check + record)
    DynamicPipeline<ValidatedOrder> stage2;
    stage2.add_stage("risk_check", stage_risk_check);
    stage2.add_stage("record",     stage_record);
    stage2.start(guard.dispatcher);

    // Bridge: MessageBusSource → stage2
    // Subscribe to "stage1_output" → forward to stage2.push()
    auto bridge_sub = bus.subscribe("stage1_output",
        [&](MessageBus::Msg msg, Context ctx) -> Task<Result<void>> {
            auto& v = std::any_cast<const ValidatedOrder&>(msg);
            co_return co_await stage2.push(v, ctx);
        });

    // Feed data
    std::vector<RawOrder> batch = {
        {4001, "samsung",  72500.0,  5},
        {4002, "lg_elec",  105000.0, 10},
        {4003, "kakao",    55000.0,  200},  // notional=11M → risk fail
        {4004, "naver",    210000.0, 3},
        {4005, "krafton",  235000.0, 4},
    };

    guard.run_and_wait([&]() -> Task<void> {
        for (auto& ord : batch)
            co_await stage1.push(ord);
        // Wait for full pipeline to finish processing
        std::this_thread::sleep_for(50ms);
    });

    println("[integration result] parsed={} validated={} risk_passed={} recorded={}",
            g_parsed.load(), g_validated.load(),
            g_risk_checked.load(), g_recorded.load());
    println("[expected] 5 parsed, 5 validated, 4 risk-passed (kakao 11M excluded), 4 recorded");
}

// ─── Scenario 5: SHMBus (LOCAL_ONLY) + Pipeline Bridge ──────────────────────

static void scenario_shm_bus_bridge() {
    println("\n=== Scenario 5: SHMBus (LOCAL_ONLY) -> Pipeline bridge ===");
    g_parsed.store(0);

    RunGuard guard;

    // Declare SHMBus topic
    SHMBus shm_bus;
    shm_bus.declare<RawOrder>("shm.raw_orders", TopicScope::LOCAL_ONLY, 64);

    // SHMBus subscriber → bridge to pipeline.push()
    auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
        .add<ParsedOrder>(stage_parse)
        .add<ParsedOrder>(stage_enrich)
        .add<ValidatedOrder>(stage_validate)
        .build();
    pipeline.start(guard.dispatcher);

    // Subscribe to SHMBus topic
    auto sub = shm_bus.subscribe<RawOrder>("shm.raw_orders");
    if (!sub) {
        println("[SKIP] SHMBus subscribe failed");
        return;
    }

    // Bridge coroutine: subscription → pipeline
    guard.dispatcher.spawn([&, s = std::move(sub)]() mutable -> Task<void> {
        for (int i = 0; i < 3; ++i) {
            auto msg = co_await s->recv();
            if (!msg) break;
            co_await pipeline.push(**msg);
        }
    }());

    // Publish data to SHMBus
    guard.run_and_wait([&]() -> Task<void> {
        shm_bus.try_publish("shm.raw_orders",
                            RawOrder{5001, "coupang", 45000.0, 15});
        shm_bus.try_publish("shm.raw_orders",
                            RawOrder{5002, "krafton", 235000.0, 2});
        shm_bus.try_publish("shm.raw_orders",
                            RawOrder{5003, "celltrion", 178000.0, 7});
        std::this_thread::sleep_for(30ms);
    });

    println("[result] parsed={}", g_parsed.load());
}

int main() {
    scenario_shm_source_to_pipeline();
    scenario_pipeline_to_messagebus();
    scenario_messagebus_source_to_pipeline();
    scenario_full_integration();
    scenario_shm_bus_bridge();
    println("\nipc_pipeline_example: ALL OK");
    return 0;
}
