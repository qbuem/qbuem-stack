/**
 * @file checkpoint_example.cpp
 * @brief CheckpointedPipeline — 배치 로그 처리기 체크포인트 복합 예시.
 *
 * ## 시나리오: 실시간 로그 수집 파이프라인
 * 로그 이벤트를 파싱 → 보강 → 저장 단계로 처리하면서
 * 처리 진행상황을 체크포인트로 저장합니다.
 *
 * ## 단계
 * 1. [파싱]   원시 로그 문자열 → 구조화된 LogEvent
 * 2. [보강]   서비스명 조회, 심각도 분류
 * 3. [저장]   처리된 이벤트 카운터 증가 (실제는 DB/S3 저장)
 *
 * ## 체크포인트 시나리오
 * - 매 N개 처리마다 자동 체크포인트 저장
 * - 수동 save_checkpoint(): 배치 완료 후 명시적 저장
 * - resume_from_checkpoint(): 재시작 시 마지막 오프셋에서 재개
 *
 * ## 커버리지
 * - CheckpointedPipeline<T>: 생성자 / pipeline() / enable_checkpoint
 * - push_counted(): 아이템 전달 + 카운터 증가
 * - save_checkpoint(): 수동 저장 (metadata_json 포함)
 * - resume_from_checkpoint(): 저장된 오프셋 복원
 * - items_processed(): 누적 처리 수 조회
 * - InMemoryCheckpointStore: save / load / size
 * - CheckpointData: offset / metadata_json / saved_at
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/checkpoint.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 도메인 타입 ─────────────────────────────────────────────────────────────

struct LogEvent {
    uint64_t    event_id;
    std::string raw;
    std::string service;
    int         severity;  // 0=INFO 1=WARN 2=ERROR
    bool        parsed    = false;
    bool        enriched  = false;
    bool        persisted = false;
};

// ─── 파이프라인 스테이지 ─────────────────────────────────────────────────────

static std::atomic<int> g_persisted_count{0};

static Task<Result<LogEvent>> stage_parse(LogEvent ev, ActionEnv /*env*/) {
    // raw 문자열에서 severity 파싱
    if (ev.raw.find("ERROR") != std::string::npos) ev.severity = 2;
    else if (ev.raw.find("WARN") != std::string::npos) ev.severity = 1;
    else ev.severity = 0;
    ev.parsed = true;
    co_return ev;
}

static Task<Result<LogEvent>> stage_enrich(LogEvent ev, ActionEnv /*env*/) {
    // 실제 서비스 레지스트리 조회를 시뮬레이션
    ev.service  = (ev.event_id % 3 == 0) ? "payment"
                : (ev.event_id % 3 == 1) ? "inventory"
                                          : "shipping";
    ev.enriched = true;
    co_return ev;
}

static Task<Result<LogEvent>> stage_persist(LogEvent ev, ActionEnv /*env*/) {
    // 실제로는 DB 또는 S3에 기록
    ev.persisted = true;
    g_persisted_count.fetch_add(1, std::memory_order_relaxed);
    co_return ev;
}

// ─── RunGuard ─────────────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher  dispatcher;
    std::thread thread;
    explicit RunGuard(size_t n = 1) : dispatcher(n) {
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

// ─── 시나리오 1: 기본 배치 처리 + 자동 체크포인트 ──────────────────────────

static void scenario_auto_checkpoint() {
    std::puts("\n=== 시나리오 1: 배치 처리 + 자동 체크포인트 (매 5개) ===");
    g_persisted_count.store(0);

    auto store = std::make_shared<InMemoryCheckpointStore>();

    RunGuard guard;
    CheckpointedPipeline<LogEvent> cp("log-pipeline", store);

    cp.pipeline().add_stage("parse",   stage_parse);
    cp.pipeline().add_stage("enrich",  stage_enrich);
    cp.pipeline().add_stage("persist", stage_persist);

    // 매 5개 처리마다 자동 체크포인트
    cp.enable_checkpoint(60s, /*every_n=*/5);
    cp.pipeline().start(guard.dispatcher);

    // 10개 이벤트 전송
    const char* messages[] = {
        "INFO: order created",
        "WARN: payment retry",
        "ERROR: stock unavailable",
        "INFO: shipment queued",
        "INFO: order delivered",
        "ERROR: payment declined",
        "WARN: inventory low",
        "INFO: refund issued",
        "ERROR: connection timeout",
        "INFO: batch complete",
    };

    guard.run_and_wait([&]() -> Task<void> {
        for (uint64_t i = 0; i < 10; ++i) {
            LogEvent ev;
            ev.event_id = i + 1;
            ev.raw      = messages[i];
            auto res = co_await cp.push_counted(
                std::move(ev), {},
                "{\"batch\":\"A\",\"seq\":" + std::to_string(i + 1) + "}"
            );
            if (!res.has_value())
                std::printf("  [WARN] push_counted 실패: %s\n",
                            res.error().message().c_str());
        }
    });

    // 파이프라인이 처리 완료할 때까지 잠시 대기
    std::this_thread::sleep_for(50ms);

    std::printf("[결과] items_processed=%llu\n",
                static_cast<unsigned long long>(cp.items_processed()));
    std::printf("[결과] 저장소 체크포인트=%zu개\n", store->size());

    // 체크포인트 내용 확인
    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await store->load("log-pipeline");
        if (res.has_value()) {
            std::printf("[체크포인트] offset=%llu metadata=%s\n",
                        static_cast<unsigned long long>(res->offset),
                        res->metadata_json.c_str());
        } else {
            std::puts("[체크포인트] 저장 없음");
        }
    });
}

// ─── 시나리오 2: 수동 체크포인트 저장 ─────────────────────────────────────

static void scenario_manual_checkpoint() {
    std::puts("\n=== 시나리오 2: 수동 체크포인트 저장 ===");
    g_persisted_count.store(0);

    auto store = std::make_shared<InMemoryCheckpointStore>();

    RunGuard guard;
    CheckpointedPipeline<LogEvent> cp("log-pipeline-manual", store);
    cp.pipeline().add_stage("parse",   stage_parse);
    cp.pipeline().add_stage("enrich",  stage_enrich);
    cp.pipeline().add_stage("persist", stage_persist);
    cp.pipeline().start(guard.dispatcher);

    // 체크포인트 미활성화 상태에서 아이템 전송
    guard.run_and_wait([&]() -> Task<void> {
        for (uint64_t i = 0; i < 7; ++i) {
            LogEvent ev{i + 100, "INFO: manual batch item", "", 0};
            co_await cp.push_counted(std::move(ev));
        }
        // 배치 완료 후 수동 저장
        auto res = co_await cp.save_checkpoint("{\"phase\":\"first_batch_done\"}");
        if (res.has_value())
            std::puts("  [체크포인트] 수동 저장 완료");
        else
            std::printf("  [체크포인트] 저장 실패: %s\n",
                        res.error().message().c_str());
    });

    std::printf("[결과] items_processed=%llu, 저장소=%zu개\n",
                static_cast<unsigned long long>(cp.items_processed()),
                store->size());
    std::printf("[결과] checkpoint_enabled=%s\n",
                cp.checkpoint_enabled() ? "YES" : "NO");
}

// ─── 시나리오 3: 재시작 시 체크포인트에서 재개 ─────────────────────────────

static void scenario_resume_from_checkpoint() {
    std::puts("\n=== 시나리오 3: 체크포인트 재개 (장애 복구 시뮬레이션) ===");

    auto store = std::make_shared<InMemoryCheckpointStore>();

    // 1단계: 20개 처리 후 체크포인트 저장
    {
        RunGuard guard;
        CheckpointedPipeline<LogEvent> cp("crash-recovery", store);
        cp.pipeline().add_stage("parse",   stage_parse);
        cp.pipeline().add_stage("enrich",  stage_enrich);
        cp.pipeline().add_stage("persist", stage_persist);
        cp.pipeline().start(guard.dispatcher);

        guard.run_and_wait([&]() -> Task<void> {
            for (uint64_t i = 0; i < 20; ++i) {
                LogEvent ev{i + 1, "INFO: item processed", "", 0};
                co_await cp.push_counted(std::move(ev));
            }
            co_await cp.save_checkpoint(
                "{\"last_event_id\":20,\"status\":\"normal\"}");
        });

        std::printf("[1단계] 처리=%llu, 체크포인트 저장됨\n",
                    static_cast<unsigned long long>(cp.items_processed()));
    }  // RunGuard 파괴 → 프로세스 재시작 시뮬레이션

    // 2단계: 새 인스턴스가 체크포인트에서 복원
    {
        RunGuard guard;
        CheckpointedPipeline<LogEvent> cp("crash-recovery", store);
        cp.pipeline().add_stage("parse",   stage_parse);
        cp.pipeline().add_stage("enrich",  stage_enrich);
        cp.pipeline().add_stage("persist", stage_persist);
        cp.pipeline().start(guard.dispatcher);

        guard.run_and_wait([&]() -> Task<void> {
            // 재시작 직후 — 오프셋 0
            std::printf("[2단계] 복원 전 offset=%llu\n",
                        static_cast<unsigned long long>(cp.items_processed()));

            auto res = co_await cp.resume_from_checkpoint();
            if (res.has_value()) {
                std::printf("[2단계] 복원 후 offset=%llu (20번 이후부터 재처리)\n",
                            static_cast<unsigned long long>(cp.items_processed()));
            } else {
                std::printf("[2단계] 복원 실패: %s\n",
                            res.error().message().c_str());
            }
        });
    }
}

// ─── 시나리오 4: 존재하지 않는 체크포인트 복원 시도 ────────────────────────

static void scenario_resume_no_checkpoint() {
    std::puts("\n=== 시나리오 4: 체크포인트 없음 → 에러 처리 ===");

    auto store = std::make_shared<InMemoryCheckpointStore>();
    RunGuard guard;
    CheckpointedPipeline<LogEvent> cp("fresh-pipeline", store);
    cp.pipeline().add_stage("parse", stage_parse);
    cp.pipeline().start(guard.dispatcher);

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await cp.resume_from_checkpoint();
        if (!res.has_value()) {
            std::printf("[결과] 체크포인트 없음 확인: error=%s\n",
                        res.error().message().c_str());
        }
    });

    std::printf("[결과] items_processed=%llu (변화 없음)\n",
                static_cast<unsigned long long>(cp.items_processed()));
}

int main() {
    scenario_auto_checkpoint();
    scenario_manual_checkpoint();
    scenario_resume_from_checkpoint();
    scenario_resume_no_checkpoint();
    std::puts("\ncheckpoint_example: ALL OK");
    return 0;
}
