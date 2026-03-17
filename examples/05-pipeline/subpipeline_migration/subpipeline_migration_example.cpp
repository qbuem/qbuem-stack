/**
 * @file subpipeline_migration_example.cpp
 * @brief SubpipelineAction + MigrationAction + DlqReprocessor 예제.
 *
 * ## 커버리지
 * - SubpipelineAction<In,Out>   — StaticPipeline을 Action으로 래핑
 * - MigrationAction<OldT,NewT> — 인라인 타입 변환 액션
 * - DlqReprocessor<T>          — DLQ 메시지 재처리
 * - DlqReprocessor::register_migration() — 마이그레이션 함수 등록
 * - DlqReprocessor::reprocess()          — DLQ 항목 재처리 실행
 *
 * ## 시나리오
 * 이벤트 스키마를 V1 → V2로 점진적으로 마이그레이션합니다.
 * - MigrationAction: 파이프라인에 V1→V2 변환 스테이지 삽입
 * - DLQ에 쌓인 V1 이벤트를 DlqReprocessor로 V2로 재처리
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/dead_letter.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/pipeline/migration.hpp>
#include <qbuem/pipeline/pipeline.hpp>
#include <qbuem/pipeline/subpipeline_action.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 스키마 버전 ──────────────────────────────────────────────────────────────

struct EventV1 {
    int         id;
    std::string data;   // V1: 단순 문자열
};

struct EventV2 {
    int         id;
    std::string payload;    // V2: "data" → "payload" 리네임
    std::string source;     // V2: 새 필드 추가
    int         version{2};
};

struct ProcessedEvent {
    int         id;
    std::string result;
};

// ─────────────────────────────────────────────────────────────────────────────
// §1  SubpipelineAction — StaticPipeline을 Action으로 재사용
// ─────────────────────────────────────────────────────────────────────────────

static void demo_subpipeline() {
    std::printf("── §1  SubpipelineAction ──\n");

    // 내부 파이프라인: EventV2 → ProcessedEvent (2단계)
    auto inner = pipeline_builder<EventV2>()
        .add<EventV2>([](EventV2 e, ActionEnv) -> Task<Result<EventV2>> {
            // 정규화 스테이지
            e.source = "normalized";
            co_return e;
        })
        .add<ProcessedEvent>([](EventV2 e, ActionEnv) -> Task<Result<ProcessedEvent>> {
            // 변환 스테이지
            co_return ProcessedEvent{e.id, "processed:" + e.payload};
        })
        .build();

    // StaticPipeline은 atomic 멤버 때문에 move 불가 → shared_ptr로 보관
    auto sub_action = std::make_shared<SubpipelineAction<EventV2, ProcessedEvent>>(std::move(inner));

    Dispatcher disp(2);
    std::thread t([&] { disp.run(); });

    // 내부 파이프라인 시작 (SubpipelineAction 내부 파이프라인)
    sub_action->inner().start(disp);

    // 외부 파이프라인: EventV1 처리 (DynamicPipeline은 동종 T→T 스테이지만 허용)
    DynamicPipeline<EventV1> outer;

    // 처리 결과 수집용 채널
    std::vector<ProcessedEvent> results;
    std::mutex results_mtx;

    // V1 수신 후 V2로 변환하여 SubpipelineAction을 호출하는 단일 스테이지
    // DynamicPipeline<EventV1>은 EventV1→EventV1 시그니처 필요 —
    // 내부적으로 V2 변환과 SubpipelineAction 호출을 수행하고 원래 EventV1을 반환
    outer.add_stage("process", [sub_action](EventV1 e, ActionEnv env)
            -> Task<Result<EventV1>> {
        // V1 → V2 변환
        EventV2 v2{e.id, e.data, "outer-pipeline"};
        // SubpipelineAction을 직접 호출
        auto r = co_await (*sub_action)(v2, env);
        if (!r) co_return unexpected(r.error());
        // ProcessedEvent 결과를 로그 (EventV1을 패스스루)
        std::printf("  내부 파이프라인 결과: id=%d result=%s\n",
                    r->id, r->result.c_str());
        co_return e;
    });

    outer.start(disp);

    // 이벤트 투입
    for (int i = 1; i <= 3; ++i)
        outer.try_push(EventV1{i, "data_" + std::to_string(i)});

    // 잠시 처리 대기
    std::this_thread::sleep_for(200ms);

    outer.stop();
    disp.stop();
    t.join();

    std::printf("  SubpipelineAction 처리 완료 (외부 파이프라인 3건)\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  MigrationAction — V1 → V2 인라인 변환
// ─────────────────────────────────────────────────────────────────────────────

static Task<void> demo_migration_task() {
    std::printf("── §2  MigrationAction ──\n");

    // V1 → V2 마이그레이션 액션 생성
    MigrationAction<EventV1, EventV2> migration(
        "v1→v2",
        [](EventV1 old) -> Result<EventV2> {
            return EventV2{old.id, old.data, "migrated"};
        });

    std::printf("  마이그레이션 이름: %s\n",
                std::string(migration.name()).c_str());

    // 단일 아이템 변환 테스트
    auto result = co_await migration.process(EventV1{42, "hello"});
    if (result) {
        std::printf("  V1{id=42, data=hello} → V2{id=%d, payload=%s, source=%s}\n",
                    result->id, result->payload.c_str(), result->source.c_str());
    }

    std::printf("\n");
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  DlqReprocessor — DLQ 메시지 재처리
// ─────────────────────────────────────────────────────────────────────────────

static Task<void> demo_dlq_reprocessor() {
    std::printf("── §3  DlqReprocessor ──\n");

    // DLQ에 V1 이벤트 적재 (실패한 처리 항목 시뮬레이션)
    DeadLetterQueue<EventV1> dlq(DeadLetterQueue<EventV1>::Config{.max_size = 100});
    auto dummy_err = std::make_error_code(std::errc::io_error);
    dlq.push(EventV1{1, "failed_order_001"}, {}, dummy_err);
    dlq.push(EventV1{2, "failed_order_002"}, {}, dummy_err);
    dlq.push(EventV1{3, "failed_order_003"}, {}, dummy_err);
    std::printf("  DLQ 크기: %zu\n", dlq.size());

    // 새 파이프라인 (V2 이벤트를 받는 채널 시뮬레이션)
    std::vector<EventV2> migrated_events;
    std::mutex mtx;

    // DlqReprocessor: V1 → V2 마이그레이션 등록
    DlqReprocessor<EventV1> reprocessor;
    reprocessor.register_migration<EventV2>(
        "v1→v2",
        [](EventV1 old) -> Result<EventV2> {
            // 변환 로직
            return EventV2{old.id, old.data, "reprocessed"};
        },
        [&migrated_events, &mtx](EventV2 v2) -> bool {
            std::lock_guard lock(mtx);
            std::printf("  재처리: V2{id=%d, payload=%s}\n",
                        v2.id, v2.payload.c_str());
            migrated_events.push_back(std::move(v2));
            return true;
        });

    std::printf("  등록된 마이그레이션 수: %zu\n",
                reprocessor.migration_count());

    // 재처리 실행
    auto summary = co_await reprocessor.reprocess(dlq);
    std::printf("  재처리 결과: migrated=%zu failed=%zu skipped=%zu\n",
                summary.migrated, summary.failed, summary.skipped);
    std::printf("  DLQ 남은 수: %zu\n\n", dlq.size());

    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem SubpipelineAction + Migration 예제 ===\n\n");

    demo_subpipeline();

    // 마이그레이션 데모는 코루틴이므로 간단한 동기 실행
    Dispatcher disp(1);
    std::thread t([&] { disp.run(); });

    std::atomic<bool> done1{false}, done2{false};
    disp.spawn([&]() -> Task<void> {
        co_await demo_migration_task();
        done1.store(true);
    }());
    disp.spawn([&]() -> Task<void> {
        co_await demo_dlq_reprocessor();
        done2.store(true);
    }());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while ((!done1.load() || !done2.load()) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    std::printf("=== 완료 ===\n");
    return 0;
}
