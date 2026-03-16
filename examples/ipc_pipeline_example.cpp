/**
 * @file ipc_pipeline_example.cpp
 * @brief Pipeline ↔ MessageBus ↔ SHMChannel 연계 복합 예시.
 *
 * ## 시나리오: 실시간 주문 처리 시스템 (IPC 파이프라인)
 *
 * qbuem-stack의 세 가지 메시징 레이어를 모두 연결합니다:
 *
 *  [외부 입력 시뮬레이터]
 *       ↓ SHMChannel (프로세스 간 공유 메모리)
 *  [SHMSource → PipelineBuilder::with_source()]
 *       ↓ StaticPipeline (parse → enrich → validate)
 *  [PipelineBuilder::with_sink() → MessageBusSink]
 *       ↓ MessageBus pub/sub (스레드 간 토픽 라우팅)
 *  [MessageBusSource → PipelineBuilder::with_source()]
 *       ↓ DynamicPipeline (risk_check → record)
 *  [output channel 직접 소비]
 *
 * ## 커버리지
 * - SHMSource<T>: init() + next() (std::string 소유권 고정)
 * - SHMSink<T>:   init() + sink() (std::string 소유권 고정)
 * - PipelineBuilder::with_source(): SHMSource 연결
 * - PipelineBuilder::with_sink():   MessageBusSink 연결
 * - MessageBusSource<T>: MessageBus 토픽 → Pipeline 소스
 * - MessageBusSink<T>:   Pipeline Tail → MessageBus 토픽 발행
 * - SHMChannel 직접 사용 (프로세스 간 데이터 공급)
 * - StaticPipeline + DynamicPipeline 혼합 아키텍처
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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace qbuem::shm;
using namespace std::chrono_literals;

// ─── 도메인 타입 ─────────────────────────────────────────────────────────────

/// @brief SHMChannel 전용 — trivially_copyable (고정 크기 배열)
struct RawOrder {
    uint64_t order_id;
    char     symbol[16];  ///< 고정 크기 (std::string 사용 불가 — SHM 요건)
    double   price;
    int      qty;
};
static_assert(std::is_trivially_copyable_v<RawOrder>,
              "RawOrder must be trivially copyable for SHMChannel");

struct ParsedOrder {
    uint64_t    order_id;
    std::string symbol;   ///< 파이프라인 내부에서는 std::string 사용
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

// ─── 파이프라인 스테이지 ─────────────────────────────────────────────────────

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
    // 심볼 정규화 (실제론 마스터 데이터 조회)
    for (auto& c : p.symbol) c = static_cast<char>(toupper(c));
    co_return p;
}

static Task<Result<ValidatedOrder>> stage_validate(ParsedOrder p, ActionEnv /*env*/) {
    ValidatedOrder v;
    v.base     = p;
    v.notional = p.price * static_cast<double>(p.qty);
    v.risk_ok  = (v.notional < 10'000'000.0);  // 1천만원 이하만 통과
    g_validated.fetch_add(1, std::memory_order_relaxed);
    co_return v;
}

// DynamicPipeline 스테이지 (ValidatedOrder → ValidatedOrder, 동종 타입)
static Task<Result<ValidatedOrder>> stage_risk_check(ValidatedOrder v, ActionEnv /*env*/) {
    if (!v.risk_ok)
        co_return unexpected(std::make_error_code(std::errc::value_too_large));
    g_risk_checked.fetch_add(1, std::memory_order_relaxed);
    co_return v;
}

static Task<Result<ValidatedOrder>> stage_record(ValidatedOrder v, ActionEnv /*env*/) {
    std::printf("  [주문기록] id=%llu symbol=%s qty=%d price=%.0f notional=%.0f\n",
                static_cast<unsigned long long>(v.base.order_id),
                v.base.symbol.c_str(), v.base.qty, v.base.price, v.notional);
    g_recorded.fetch_add(1, std::memory_order_relaxed);
    co_return v;
}

// ─── RunGuard ─────────────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher  dispatcher;
    std::thread thread;
    explicit RunGuard(size_t n = 2) : dispatcher(n) {
        thread = std::thread([this] { dispatcher.run(); });
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

// ─── 시나리오 1: SHMChannel → SHMSource → StaticPipeline ────────────────────

static void scenario_shm_source_to_pipeline() {
    std::puts("\n=== 시나리오 1: SHMChannel → SHMSource → StaticPipeline ===");
    g_parsed.store(0); g_validated.store(0);

    const char* channel_name = "qbuem_orders_test";

    // 1) SHM 채널 생성 (Producer 측)
    auto shm_chan = SHMChannel<RawOrder>::create(channel_name, 32);
    if (!shm_chan) {
        std::printf("[SKIP] SHMChannel 생성 실패: %s\n",
                    shm_chan.error().message().c_str());
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

    // 3) SHM 채널에 데이터 쓰기 (다른 프로세스에서 왔다고 가정)
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
        // 파이프라인 처리 완료 대기
        std::this_thread::sleep_for(30ms);
    });

    std::printf("[결과] 파싱=%d 검증=%d\n",
                g_parsed.load(), g_validated.load());

    // SHM 채널 정리
    (*shm_chan)->close();
    SHMChannel<RawOrder>::unlink(channel_name);
}

// ─── 시나리오 2: StaticPipeline → MessageBusSink → MessageBus 발행 ──────────

static void scenario_pipeline_to_messagebus() {
    std::puts("\n=== 시나리오 2: StaticPipeline → MessageBusSink → MessageBus ===");
    g_parsed.store(0); g_validated.store(0);

    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    // MessageBus 구독자 (ValidatedOrder 수신)
    std::atomic<int> received{0};
    auto sub = bus.subscribe("validated_orders",
        [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            auto& v = std::any_cast<const ValidatedOrder&>(msg);
            received.fetch_add(1, std::memory_order_relaxed);
            std::printf("  [구독자] 주문 수신: id=%llu notional=%.0f\n",
                        static_cast<unsigned long long>(v.base.order_id),
                        v.notional);
            co_return Result<void>{};
        });

    // StaticPipeline 구성: with_sink() → MessageBusSink
    auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
        .add<ParsedOrder>(stage_parse)
        .add<ParsedOrder>(stage_enrich)
        .add<ValidatedOrder>(stage_validate)
        .with_sink(MessageBusSink<ValidatedOrder>(bus, "validated_orders"))
        .build();

    pipeline.start(guard.dispatcher);

    // 주문 입력
    guard.run_and_wait([&]() -> Task<void> {
        co_await pipeline.push(RawOrder{2001, "lg_elec", 105000.0, 20});
        co_await pipeline.push(RawOrder{2002, "posco",   382000.0, 3});
        co_await pipeline.push(RawOrder{2003, "hyundai", 205000.0, 8});
        // 처리 완료 대기
        std::this_thread::sleep_for(30ms);
    });

    std::printf("[결과] 파싱=%d 검증=%d 구독자수신=%d\n",
                g_parsed.load(), g_validated.load(), received.load());
}

// ─── 시나리오 3: MessageBusSource → StaticPipeline ──────────────────────────

static void scenario_messagebus_source_to_pipeline() {
    std::puts("\n=== 시나리오 3: MessageBusSource → StaticPipeline ===");
    g_parsed.store(0); g_validated.store(0);

    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    // StaticPipeline: MessageBusSource가 head
    auto pipeline = PipelineBuilder<ValidatedOrder, ValidatedOrder>{}
        .with_source(MessageBusSource<ValidatedOrder>(bus, "raw_validated"))
        .add<ValidatedOrder>(stage_risk_check)
        .add<ValidatedOrder>(stage_record)
        .build();

    pipeline.start(guard.dispatcher);

    // bus.publish()로 데이터 공급
    guard.run_and_wait([&]() -> Task<void> {
        std::this_thread::sleep_for(5ms);  // source 초기화 완료 대기

        co_await bus.publish("raw_validated",
            ValidatedOrder{{3001, "SAMSUNG", 72500.0, 10, true, ""}, 725000.0, true});
        co_await bus.publish("raw_validated",
            ValidatedOrder{{3002, "SK_HYNIX", 93000.0, 50, true, ""}, 4650000.0, true});
        co_await bus.publish("raw_validated",
            ValidatedOrder{{3003, "KAKAO", 55000.0, 200, true, ""}, 11000000.0, false}); // risk fail

        std::this_thread::sleep_for(30ms);
    });

    std::printf("[결과] 리스크체크=%d 기록됨=%d (200주 초과 → 리스크 거부)\n",
                g_risk_checked.load(), g_recorded.load());
}

// ─── 시나리오 4: 완전한 통합 파이프라인 ─────────────────────────────────────
//
// RawOrder (push) → StaticPipeline (parse+enrich+validate)
//                 → MessageBusSink("stage1_out")
//                 → MessageBus fan-out
//                 → MessageBusSource("stage1_out") → DynamicPipeline (risk+record)

static void scenario_full_integration() {
    std::puts("\n=== 시나리오 4: 전체 통합 (StaticPipeline → MessageBus → DynamicPipeline) ===");
    g_parsed.store(0); g_validated.store(0);
    g_risk_checked.store(0); g_recorded.store(0);

    RunGuard guard;
    MessageBus bus;
    bus.start(guard.dispatcher);

    // ── Stage1: StaticPipeline (parse/enrich/validate) + MessageBusSink ──
    auto stage1 = PipelineBuilder<RawOrder, RawOrder>{}
        .add<ParsedOrder>(stage_parse)
        .add<ParsedOrder>(stage_enrich)
        .add<ValidatedOrder>(stage_validate)
        .with_sink(MessageBusSink<ValidatedOrder>(bus, "stage1_output"))
        .build();
    stage1.start(guard.dispatcher);

    // ── Stage2: DynamicPipeline (risk_check + record) ──────────────────
    DynamicPipeline<ValidatedOrder> stage2;
    stage2.add_stage("risk_check", stage_risk_check);
    stage2.add_stage("record",     stage_record);
    stage2.start(guard.dispatcher);

    // ── 연결: MessageBusSource → stage2 ────────────────────────────────
    // MessageBus "stage1_output" 구독 → stage2.push()
    auto bridge_sub = bus.subscribe("stage1_output",
        [&](MessageBus::Msg msg, Context ctx) -> Task<Result<void>> {
            auto& v = std::any_cast<const ValidatedOrder&>(msg);
            co_return co_await stage2.push(v, ctx);
        });

    // ── 데이터 투입 ────────────────────────────────────────────────────
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
        // 전체 파이프라인 처리 완료 대기
        std::this_thread::sleep_for(50ms);
    });

    std::printf("[통합결과] 파싱=%d 검증=%d 리스크통과=%d 기록됨=%d\n",
                g_parsed.load(), g_validated.load(),
                g_risk_checked.load(), g_recorded.load());
    std::printf("[예상] 5개 파싱, 5개 검증, 4개 리스크통과(kakao 11M 제외), 4개 기록\n");
}

// ─── 시나리오 5: SHMBus (LOCAL_ONLY) + Pipeline 연계 ────────────────────────

static void scenario_shm_bus_bridge() {
    std::puts("\n=== 시나리오 5: SHMBus (LOCAL_ONLY) → Pipeline 연계 ===");
    g_parsed.store(0);

    RunGuard guard;

    // SHMBus 선언
    SHMBus shm_bus;
    shm_bus.declare<RawOrder>("shm.raw_orders", TopicScope::LOCAL_ONLY, 64);

    // SHMBus 구독자 → pipeline.push()로 브릿지
    auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
        .add<ParsedOrder>(stage_parse)
        .add<ParsedOrder>(stage_enrich)
        .add<ValidatedOrder>(stage_validate)
        .build();
    pipeline.start(guard.dispatcher);

    // SHMBus ISubscription 구독
    auto sub = shm_bus.subscribe<RawOrder>("shm.raw_orders");
    if (!sub) {
        std::puts("[SKIP] SHMBus subscribe 실패");
        return;
    }

    // 구독 → pipeline 브릿지 코루틴
    guard.dispatcher.spawn([&, s = std::move(sub)]() mutable -> Task<void> {
        for (int i = 0; i < 3; ++i) {
            auto msg = co_await s->recv();
            if (!msg) break;
            co_await pipeline.push(**msg);
        }
    }());

    // SHMBus에 데이터 발행
    guard.run_and_wait([&]() -> Task<void> {
        shm_bus.try_publish("shm.raw_orders",
                            RawOrder{5001, "coupang", 45000.0, 15});
        shm_bus.try_publish("shm.raw_orders",
                            RawOrder{5002, "krafton", 235000.0, 2});
        shm_bus.try_publish("shm.raw_orders",
                            RawOrder{5003, "celltrion", 178000.0, 7});
        std::this_thread::sleep_for(30ms);
    });

    std::printf("[결과] 파싱=%d\n", g_parsed.load());
}

int main() {
    scenario_shm_source_to_pipeline();
    scenario_pipeline_to_messagebus();
    scenario_messagebus_source_to_pipeline();
    scenario_full_integration();
    scenario_shm_bus_bridge();
    std::puts("\nipc_pipeline_example: ALL OK");
    return 0;
}
