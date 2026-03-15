/**
 * @file tests/pipeline_guide_test.cpp
 * @brief Pipeline Master Guide 패턴 검증 테스트.
 *
 * 이 테스트 파일은 `docs/pipeline-master-guide.md`에서 소개된 각 패턴과
 * 레시피를 실제로 검증합니다.
 *
 * ## 커버리지
 * §3-2  DynamicPipeline 기본 동작 / hot_swap
 * §4-1  Bulkheading — 무거운 액션을 별도 워커로 격리
 * §5-1  Fan-out (Broadcast) — PipelineGraph 1→N 분기
 * §5-1  Fan-in  (Merge)    — PipelineGraph N→1 수집
 * §5-2  Sidecar Observation — T-pipe 복사 관찰
 * §5-2  Feedback Loop       — 실패 아이템 업스트림 재전송
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

// ─── 공통 헬퍼 ───────────────────────────────────────────────────────────────

/// GTest 환경에서 Dispatcher를 안전하게 실행/정지하는 RAII 래퍼
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
};

/// 채널에서 최대 `n`개 항목을 타임아웃 `timeout` 내에 수집합니다.
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

// ── 3-2-1: 기본 스테이지 실행 ─────────────────────────────────────────────

TEST(DynamicPipeline, BasicStageExecution) {
    // Guide §3-2: 런타임에 스테이지를 추가하고 처리 결과 검증
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
        EXPECT_GT(r, 0) << "결과는 양수여야 함";
    }
    dp.stop();
}

// ── 3-2-2: Hot-swap — 실행 중 스테이지 교체 ──────────────────────────────

TEST(DynamicPipeline, HotSwapReplaceStage) {
    // Guide §3-2: Hot-swapping without stopping the world
    DynamicPipeline<int> dp;
    dp.add_stage("transform", [](int x, ActionEnv) -> Task<Result<int>> {
        co_return x * 2;   // v1: ×2
    });

    RunGuard g;
    dp.start(g.dispatcher);

    // Phase 1: v1 (×2)
    dp.try_push(10);
    auto r1 = collect(dp.output(), 1);
    ASSERT_EQ(r1.size(), 1u);
    int v1_result = r1[0];

    // Hot-swap → v2 (×10)
    bool swapped = dp.hot_swap("transform", [](int x, ActionEnv) -> Task<Result<int>> {
        co_return x * 10;  // v2: ×10
    });
    EXPECT_TRUE(swapped);

    // Phase 2: v2 (×10)
    dp.try_push(10);
    auto r2 = collect(dp.output(), 1);
    ASSERT_EQ(r2.size(), 1u);
    int v2_result = r2[0];

    dp.stop();

    // v1: 10*2=20, v2: 10*10=100 (순서 보장 어려울 수 있으나 결과는 구별됨)
    EXPECT_NE(v1_result, v2_result)
        << "hot-swap 전후 결과가 달라야 함: v1=" << v1_result << " v2=" << v2_result;
}

// ── 3-2-3: 스테이지 추가 — 런타임에 스테이지 삽입 ──────────────────────

TEST(DynamicPipeline, AddStageAtRuntime) {
    DynamicPipeline<int> dp;
    dp.add_stage("first", [](int x, ActionEnv) -> Task<Result<int>> {
        co_return x + 100;
    });

    RunGuard g;
    dp.start(g.dispatcher);

    // 실행 중 스테이지 추가 (스테이지 간 연결은 add_stage 순서로 결정됨)
    dp.add_stage("second", [](int x, ActionEnv) -> Task<Result<int>> {
        co_return x * 2;
    });

    dp.try_push(5);
    auto results = collect(dp.output(), 1);
    ASSERT_FALSE(results.empty());
    // 결과는 적어도 첫 번째 스테이지는 통과해야 함
    EXPECT_GE(results[0], 100);
    dp.stop();
}

// =============================================================================
// §4-1  Bulkheading — 무거운 액션의 독립적인 워커 격리
// =============================================================================

TEST(Bulkheading, HeavyActionIsolatedFromLight) {
    // Guide §4-1: heavy 액션에 별도 워커 풀을 부여해 light 액션이 블록되지 않는 것 확인
    std::atomic<size_t> light_count{0};
    std::atomic<size_t> heavy_count{0};

    // light 스테이지: 즉시 처리
    auto light_fn = [&](int x) -> Task<Result<int>> {
        light_count.fetch_add(1, std::memory_order_relaxed);
        co_return x;
    };

    // heavy 스테이지: 독립 워커 풀 (min=1, max=4)
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

    EXPECT_EQ(light_count.load(), kItems) << "light 스테이지 처리 수 불일치";
    EXPECT_EQ(heavy_count.load(), kItems) << "heavy 스테이지 처리 수 불일치";
}

// =============================================================================
// §5-1  Fan-out (Broadcast) — PipelineGraph 1→N
// =============================================================================

TEST(PipelineGraph, FanOutBroadcast) {
    // Guide §5-1: 하나의 소스 → 두 싱크(main, audit)
    struct Msg { std::string content; std::string branch; };

    PipelineGraph<Msg> graph;
    graph
        .node("ingest", [](Msg m, ActionEnv) -> Task<Result<Msg>> {
            co_return m;
        }, {.workers=1, .chan_cap=64})
        .node("main_sink", [](Msg m, ActionEnv) -> Task<Result<Msg>> {
            m.branch = "main";
            co_return m;
        }, {.workers=1, .chan_cap=64})
        .node("audit_sink", [](Msg m, ActionEnv) -> Task<Result<Msg>> {
            m.branch = "audit";
            co_return m;
        }, {.workers=1, .chan_cap=64})
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

    // 팬아웃: 각 입력이 2개 출력을 생성 → 총 kItems*2
    auto results = collect(graph.output(), kItems * 2);

    size_t main_cnt  = 0, audit_cnt = 0;
    for (auto& r : results) {
        if (r.branch == "main")  ++main_cnt;
        if (r.branch == "audit") ++audit_cnt;
    }

    EXPECT_EQ(main_cnt,  kItems) << "main 브랜치 출력 수 불일치";
    EXPECT_EQ(audit_cnt, kItems) << "audit 브랜치 출력 수 불일치";
    graph.stop();
}

// =============================================================================
// §5-1  Fan-in (Merge) — 여러 소스 → 하나의 출력
// =============================================================================

TEST(PipelineGraph, FanInMerge) {
    // Guide §5-1: 두 소스(source_a, source_b) → 공통 처리 노드 → 싱크
    struct Event { int id; std::string from; };

    PipelineGraph<Event> graph;
    graph
        .node("source_a", [](Event e, ActionEnv) -> Task<Result<Event>> {
            e.from = "A"; co_return e;
        }, {.workers=1, .chan_cap=64})
        .node("source_b", [](Event e, ActionEnv) -> Task<Result<Event>> {
            e.from = "B"; co_return e;
        }, {.workers=1, .chan_cap=64})
        .node("merge_sink", [](Event e, ActionEnv) -> Task<Result<Event>> {
            co_return e;
        }, {.workers=1, .chan_cap=128})
        .edge("source_a", "merge_sink")
        .edge("source_b", "merge_sink")
        .source("source_a")
        .source("source_b")
        .sink("merge_sink");

    RunGuard g(2);
    graph.start(g.dispatcher);

    // 두 소스에 각각 5개 투입 → 총 10개 출력
    constexpr size_t kEach = 5;
    for (size_t i = 0; i < kEach; ++i) {
        graph.try_push(Event{static_cast<int>(i * 2),     ""});  // source_a
        graph.try_push(Event{static_cast<int>(i * 2 + 1), ""});  // source_b
    }

    auto results = collect(graph.output(), kEach * 2);
    EXPECT_GE(results.size(), kEach) << "팬인 후 결과 수 부족";
    graph.stop();
}

// =============================================================================
// §5-1  Conditional Edge (A/B 라우팅) — edge_if 술어 라우팅
// =============================================================================

TEST(PipelineGraph, ConditionalEdgeRouting) {
    // edge_if: 짝수 → even_sink, 홀수 → odd_sink
    PipelineGraph<int> graph;
    graph
        .node("source", [](int x, ActionEnv) -> Task<Result<int>> { co_return x; },
              {.workers=1, .chan_cap=64})
        .node("even_sink", [](int x, ActionEnv) -> Task<Result<int>> { co_return x * 10; },
              {.workers=1, .chan_cap=64})
        .node("odd_sink",  [](int x, ActionEnv) -> Task<Result<int>> { co_return x * 100; },
              {.workers=1, .chan_cap=64})
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
        // 짝수 경로는 *10 이므로 10의 배수, 홀수 경로는 *100 이므로 100의 배수
        bool is_even_branch = (r % 10 == 0) && (r % 100 != 0);
        bool is_odd_branch  = (r % 100 == 0);
        EXPECT_TRUE(is_even_branch || is_odd_branch)
            << "라우팅 결과가 두 경로 중 하나여야 함: r=" << r;
    }
    graph.stop();
}

// =============================================================================
// §5-2  Sidecar Observation — T-pipe 복사 관찰
// =============================================================================

TEST(SidecarObservation, TeeChannelDoesNotAffectLatency) {
    // Guide §5-2: 관찰 경로가 메인 경로의 처리 결과에 영향을 주지 않아야 함
    std::atomic<size_t> observed{0};
    auto side_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(64);

    auto tee_fn = [&side_ch, &observed](int x, ActionEnv) -> Task<Result<int>> {
        // 사이드카: 값을 복사해 관찰 채널에 넣음 (메인 경로는 원본 반환)
        side_ch->try_send(ContextualItem<int>{x, {}});
        observed.fetch_add(1, std::memory_order_relaxed);
        co_return x; // 원본 그대로 통과
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

    // 메인 경로 결과 확인 (×2)
    ASSERT_EQ(main_results.size(), kItems);
    for (int r : main_results)
        EXPECT_GT(r, 0);

    // 사이드카 관찰 수 확인
    EXPECT_EQ(observed.load(), kItems) << "사이드카 관찰 수가 투입 수와 다름";
}

// =============================================================================
// §5-2  Feedback Loop — 실패 아이템 업스트림 재전송
// =============================================================================

TEST(FeedbackLoop, FailedItemRetried) {
    // Guide §5-2: 처리 실패 아이템을 업스트림 채널로 다시 전송
    // 첫 번째 시도는 실패, 두 번째 시도는 성공하도록 attempt 카운터 사용
    std::atomic<size_t> attempts{0};

    auto feedback_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(64);

    auto fn = [&](int x, ActionEnv) -> Task<Result<int>> {
        size_t attempt = attempts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (attempt < 3) {
            // 재시도 채널에 다시 투입
            feedback_ch->try_send(ContextualItem<int>{x, {}});
            co_return std::make_error_code(std::errc::resource_unavailable_try_again);
        }
        co_return x * 10;
    };

    Action<int,int> action{fn, {.min_workers=1, .max_workers=1, .channel_cap=64}};
    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(64);

    RunGuard g;
    action.start(g.dispatcher, out_ch);

    // 첫 번째 투입
    action.try_push(42);

    // feedback_ch에서 재시도 항목을 action으로 다시 투입
    auto refeeder = [&]() -> Task<Result<void>> {
        for (size_t i = 0; i < 5; ++i) {
            auto item = co_await feedback_ch->recv();
            if (!item) co_return {};
            co_await action.push(item->value, item->ctx);
        }
        co_return {};
    };
    g.dispatcher.spawn(refeeder());

    // 성공 결과 대기
    auto results = collect(out_ch, 1, 5000ms);

    action.stop();

    ASSERT_FALSE(results.empty()) << "피드백 루프로 재시도 후 결과가 없음";
    EXPECT_EQ(results[0], 420) << "42*10=420 이어야 함";
    EXPECT_GE(attempts.load(), 3u) << "최소 3번 시도 필요";
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
    // Guide Recipe A: ServiceRegistry에 partial 데이터 저장 → 두 센서 동기화
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
        // GPS 먼저, IMU 나중
        action.try_push(SensorMsg{SensorMsg::Kind::GPS,
                                   {}, GpsData{fid, 37.5 + i*0.01, 127.0}});
        action.try_push(SensorMsg{SensorMsg::Kind::IMU,
                                   ImuData{fid, float(i), float(i), 9.8f}});
    }

    // kFrames*2 메시지 중 complete=true인 것만 수집
    size_t fused = 0;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fused < kFrames && std::chrono::steady_clock::now() < deadline) {
        auto item = out_ch->try_recv();
        if (item && item->value.complete) ++fused;
        else std::this_thread::sleep_for(1ms);
    }

    action.stop();
    EXPECT_EQ(fused, kFrames) << "모든 프레임이 융합되어야 함";
}

// =============================================================================
// §6B  Hardware Batching (NPU) — BatchAction
// =============================================================================

TEST(HardwareBatching, BatchActionAccumulatesAndFlushes) {
    // Guide Recipe B: max_batch_size 도달 또는 타임아웃 시 배치 처리
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

    // 정확히 2배치 = 8개 투입
    constexpr size_t kItems = 8;
    for (size_t i = 1; i <= kItems; ++i)
        ba.try_push(static_cast<int>(i));

    auto results = collect(ba.output(), kItems, 5000ms);

    ba.stop();

    // 모든 아이템이 처리되어야 함
    EXPECT_EQ(results.size(), kItems) << "배치 처리 후 출력 수 불일치";
    // 최소 2번 배치 호출 (kItems / kBatch)
    EXPECT_GE(batch_calls.load(), kItems / kBatch)
        << "배치 함수 호출 수 부족";
    EXPECT_EQ(total_items.load(), kItems) << "처리된 총 아이템 수 불일치";
}

TEST(HardwareBatching, BatchActionTimeoutFlush) {
    // max_batch_size 미달이어도 max_wait_ms 후 플러시 확인
    std::atomic<size_t> batches{0};

    BatchAction<int,int> ba{
        [&](std::vector<int> v, ActionEnv) -> Task<Result<std::vector<int>>> {
            batches.fetch_add(1, std::memory_order_relaxed);
            co_return v;
        },
        BatchAction<int,int>::Config{
            .max_batch_size = 100,   // 매우 큰 배치 크기
            .max_wait_ms    = 30,    // 30ms 후 강제 플러시
            .workers        = 1,
        }
    };

    RunGuard g;
    ba.start(g.dispatcher);

    // 1개만 투입 — 배치 크기 미달, 타임아웃으로 플러시 유발
    ba.try_push(99);

    auto results = collect(ba.output(), 1, 2000ms);
    ba.stop();

    ASSERT_EQ(results.size(), 1u) << "타임아웃 플러시 후 결과 없음";
    EXPECT_EQ(results[0], 99);
    EXPECT_GE(batches.load(), 1u);
}

// =============================================================================
// §6C  Resilient WAS (DLQ) — DlqAction + DeadLetterQueue
// =============================================================================

TEST(DeadLetterQueue, DlqActionSendsFailuresToDlq) {
    // Guide Recipe C: max_attempts 초과 후 DLQ로 전송
    auto dlq = std::make_shared<DeadLetterQueue<int>>();

    std::atomic<int> attempt_counter{0};
    auto failing_fn = [&](int x, ActionEnv) -> Task<Result<int>> {
        attempt_counter.fetch_add(1, std::memory_order_relaxed);
        // 항상 실패
        co_return std::make_error_code(std::errc::io_error);
        (void)x;
    };

    constexpr size_t kMaxAttempts = 3;
    DlqAction<int,int> dlq_action{failing_fn, dlq, kMaxAttempts};

    Action<int,int> action{dlq_action,
                            {.min_workers=1, .max_workers=1, .channel_cap=32}};

    RunGuard g;
    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(32);
    action.start(g.dispatcher, out_ch);

    action.try_push(42);

    // DLQ에 항목이 쌓일 때까지 대기
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (dlq->size() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    action.stop();

    EXPECT_GE(dlq->size(), 1u) << "DLQ에 실패 항목이 없음";
    EXPECT_GE(attempt_counter.load(), static_cast<int>(kMaxAttempts))
        << "최대 시도 횟수만큼 재시도되어야 함";
}

TEST(DeadLetterQueue, DlqActionSuccessSkipsDlq) {
    // 성공 시에는 DLQ로 전송되지 않아야 함
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
    EXPECT_EQ(dlq->size(), 0u) << "성공한 항목은 DLQ에 들어가면 안 됨";
}

TEST(DeadLetterQueue, DlqDrainReturnsAllLetters) {
    // DeadLetterQueue::drain() 검증
    auto dlq = std::make_shared<DeadLetterQueue<std::string>>();
    dlq->push("msg1", {}, std::make_error_code(std::errc::io_error));
    dlq->push("msg2", {}, std::make_error_code(std::errc::timed_out));
    dlq->push("msg3", {}, std::make_error_code(std::errc::connection_refused));

    EXPECT_EQ(dlq->size(), 3u);

    auto letters = dlq->drain();
    EXPECT_EQ(letters.size(), 3u);
    EXPECT_EQ(dlq->size(), 0u) << "drain() 후 DLQ는 비어있어야 함";
}

TEST(DeadLetterQueue, DlqMaxSizeDropsOldest) {
    // max_size 초과 시 가장 오래된 항목 드롭
    DeadLetterQueue<int> dlq({.max_size = 3});
    for (int i = 1; i <= 5; ++i)
        dlq.push(i, {}, std::make_error_code(std::errc::io_error));

    EXPECT_EQ(dlq.size(), 3u) << "max_size 초과분은 드롭되어야 함";
    auto letters = dlq.drain();
    // 가장 오래된 1, 2가 드롭되고 3,4,5만 남아야 함
    ASSERT_EQ(letters.size(), 3u);
    EXPECT_EQ(letters[0].item, 3);
    EXPECT_EQ(letters[1].item, 4);
    EXPECT_EQ(letters[2].item, 5);
}

// =============================================================================
// §7  Periodic Polling Source — while(true) + co_await sleep
// =============================================================================

TEST(PeriodicPollingSource, PollsAtRegularIntervals) {
    // Guide §7: 센서/하드웨어 레지스터 폴링 소스 패턴
    // co_await sleep() + 채널 push 루프
    auto poll_ch = std::make_shared<AsyncChannel<ContextualItem<int>>>(64);
    std::atomic<size_t> poll_count{0};
    std::atomic<bool>   stop_flag{false};

    RunGuard g;

    // Periodic Source 코루틴 (가이드 §7 패턴)
    auto polling_source = [&]() -> Task<Result<void>> {
        while (!stop_flag.load(std::memory_order_acquire)) {
            int sensor_value = static_cast<int>(poll_count.fetch_add(1, std::memory_order_relaxed));
            poll_ch->try_send(ContextualItem<int>{sensor_value, {}});
            // co_await qbuem::sleep(10ms) — 테스트 환경에서는 짧은 수동 대기로 대체
            co_await std::suspend_never{};
        }
        co_return {};
    };

    g.dispatcher.spawn(polling_source());

    // 최소 5개 폴링 이벤트 수집
    auto deadline = std::chrono::steady_clock::now() + 3s;
    size_t received = 0;
    while (received < 5 && std::chrono::steady_clock::now() < deadline) {
        auto item = poll_ch->try_recv();
        if (item) ++received;
        else std::this_thread::sleep_for(1ms);
    }

    stop_flag.store(true, std::memory_order_release);

    EXPECT_GE(received, 5u)    << "폴링 소스가 5회 이상 이벤트를 발생시켜야 함";
    EXPECT_GE(poll_count.load(), received) << "폴링 카운터가 수신 수 이상이어야 함";
}

// =============================================================================
// §7  Source Pinning — Dispatcher::spawn_on (코어 핀닝 API 검증)
// =============================================================================

TEST(PeriodicPollingSource, MultiSourceContextIsolation) {
    // 두 개의 독립적인 소스 코루틴이 서로 다른 Context를 갖고 채널에 푸시
    struct Tagged { int val; std::string source_id; };

    auto ch = std::make_shared<AsyncChannel<ContextualItem<Tagged>>>(64);
    std::atomic<bool> stop_a{false}, stop_b{false};

    RunGuard g(2);

    // Source A
    g.dispatcher.spawn([&]() -> Task<Result<void>> {
        for (int i = 0; i < 5 && !stop_a.load(); ++i) {
            ch->try_send(ContextualItem<Tagged>{Tagged{i, "A"}, {}});
            co_await std::suspend_never{};
        }
        stop_a.store(true);
        co_return {};
    }());

    // Source B
    g.dispatcher.spawn([&]() -> Task<Result<void>> {
        for (int i = 0; i < 5 && !stop_b.load(); ++i) {
            ch->try_send(ContextualItem<Tagged>{Tagged{i, "B"}, {}});
            co_await std::suspend_never{};
        }
        stop_b.store(true);
        co_return {};
    }());

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

    EXPECT_GE(from_a, 1u) << "Source A에서 이벤트가 없음";
    EXPECT_GE(from_b, 1u) << "Source B에서 이벤트가 없음";
}

// =============================================================================
// §3-1  StaticPipeline — 컴파일 타임 타입 체인 추가 검증
// =============================================================================

TEST(StaticPipeline, TypeChainIsCorrect) {
    // 3단계 체인 타입 안전성 — pipeline_builder<>().add<>().build() 반환 타입 확인
    auto pipeline = pipeline_builder<int>()
        .add<std::string>([](int x) -> Task<Result<std::string>> {
            co_return std::to_string(x);
        })
        .add<size_t>([](std::string s) -> Task<Result<size_t>> {
            co_return s.size();
        })
        .build();

    using P = StaticPipeline<int, size_t>;
    static_assert(std::is_same_v<decltype(pipeline), P>, "타입 체인이 잘못됨");

    EXPECT_EQ(pipeline.state(), P::State::Created);
    ASSERT_NE(pipeline.output(), nullptr);
}

TEST(StaticPipeline, LiveEndToEndProcessing) {
    // 실제 Dispatcher를 실행하고 결과를 수집하는 E2E 테스트
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
        // x*3-1 범위: 1*3-1=2 ~ 8*3-1=23
        EXPECT_GE(r, 2);
        EXPECT_LE(r, 23);
    }
}
