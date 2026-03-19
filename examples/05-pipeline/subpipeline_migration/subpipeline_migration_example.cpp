/**
 * @file subpipeline_migration_example.cpp
 * @brief SubpipelineAction + MigrationAction + DlqReprocessor example.
 *
 * ## Coverage
 * - SubpipelineAction<In,Out>   — wrap StaticPipeline as an Action
 * - MigrationAction<OldT,NewT> — inline type conversion action
 * - DlqReprocessor<T>          — reprocess DLQ messages
 * - DlqReprocessor::register_migration() — register migration function
 * - DlqReprocessor::reprocess()          — execute DLQ item reprocessing
 *
 * ## Scenario
 * Gradually migrate event schema from V1 → V2.
 * - MigrationAction: insert a V1→V2 conversion stage in the pipeline
 * - DlqReprocessor: reprocess accumulated V1 DLQ events as V2
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
#include <string>
#include <thread>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;

// ─── Schema versions ──────────────────────────────────────────────────────────

struct EventV1 {
    int         id;
    std::string data;   // V1: simple string
};

struct EventV2 {
    int         id;
    std::string payload;    // V2: "data" renamed to "payload"
    std::string source;     // V2: new field added
    int         version{2};
};

struct ProcessedEvent {
    int         id;
    std::string result;
};

// ─────────────────────────────────────────────────────────────────────────────
// §1  SubpipelineAction — reuse StaticPipeline as an Action
// ─────────────────────────────────────────────────────────────────────────────

static void demo_subpipeline() {
    println("── §1  SubpipelineAction ──");

    // Inner pipeline: EventV2 → ProcessedEvent (2 stages)
    auto inner = pipeline_builder<EventV2>()
        .add<EventV2>([](EventV2 e, ActionEnv) -> Task<Result<EventV2>> {
            // Normalization stage
            e.source = "normalized";
            co_return e;
        })
        .add<ProcessedEvent>([](EventV2 e, ActionEnv) -> Task<Result<ProcessedEvent>> {
            // Transformation stage
            co_return ProcessedEvent{e.id, "processed:" + e.payload};
        })
        .build();

    // StaticPipeline cannot be moved (has atomic members) — store via shared_ptr
    auto sub_action = std::make_shared<SubpipelineAction<EventV2, ProcessedEvent>>(std::move(inner));

    Dispatcher disp(2);
    std::jthread t([&] { disp.run(); });

    // Start inner pipeline (inside SubpipelineAction)
    sub_action->inner().start(disp);

    // Outer pipeline: process EventV1 (DynamicPipeline only allows same-type T→T stages)
    DynamicPipeline<EventV1> outer;

    // Result collection channel
    std::vector<ProcessedEvent> results;
    std::mutex results_mtx;

    // Single stage: receive V1, convert to V2, call SubpipelineAction, return V1 passthrough
    // DynamicPipeline<EventV1> requires EventV1→EventV1 signature —
    // internally performs V2 conversion and SubpipelineAction invocation,
    // then returns the original EventV1
    outer.add_stage("process", [sub_action](EventV1 e, ActionEnv env)
            -> Task<Result<EventV1>> {
        // V1 → V2 conversion
        EventV2 v2{e.id, e.data, "outer-pipeline"};
        // Invoke SubpipelineAction directly
        auto r = co_await (*sub_action)(v2, env);
        if (!r) co_return unexpected(r.error());
        // Log ProcessedEvent result (pass through EventV1)
        println("  inner pipeline result: id={} result={}", r->id, r->result);
        co_return e;
    });

    outer.start(disp);

    // Push events
    for (int i = 1; i <= 3; ++i)
        outer.try_push(EventV1{i, "data_" + std::to_string(i)});

    // Brief wait for processing
    std::this_thread::sleep_for(200ms);

    outer.stop();
    disp.stop();
    t.join();

    println("  SubpipelineAction done (3 items in outer pipeline)\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  MigrationAction — V1 → V2 inline conversion
// ─────────────────────────────────────────────────────────────────────────────

static Task<void> demo_migration_task() {
    println("── §2  MigrationAction ──");

    // Create V1 → V2 migration action
    MigrationAction<EventV1, EventV2> migration(
        "v1->v2",
        [](EventV1 old) -> Result<EventV2> {
            return EventV2{old.id, old.data, "migrated"};
        });

    println("  migration name: {}",
                std::string(migration.name()));

    // Single item conversion test
    auto result = co_await migration.process(EventV1{42, "hello"});
    if (result) {
        println("  V1{{id=42, data=hello}} -> V2{{id={}, payload={}, source={}}}",
                    result->id, result->payload, result->source);
    }

    println("");
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  DlqReprocessor — reprocess DLQ messages
// ─────────────────────────────────────────────────────────────────────────────

static Task<void> demo_dlq_reprocessor() {
    println("── §3  DlqReprocessor ──");

    // Load V1 events into DLQ (simulating previously failed items)
    DeadLetterQueue<EventV1> dlq(DeadLetterQueue<EventV1>::Config{.max_size = 100});
    auto dummy_err = std::make_error_code(std::errc::io_error);
    dlq.push(EventV1{1, "failed_order_001"}, {}, dummy_err);
    dlq.push(EventV1{2, "failed_order_002"}, {}, dummy_err);
    dlq.push(EventV1{3, "failed_order_003"}, {}, dummy_err);
    println("  DLQ size: {}", dlq.size());

    // New pipeline (channel that receives V2 events — simulated)
    std::vector<EventV2> migrated_events;
    std::mutex mtx;

    // DlqReprocessor: register V1 → V2 migration
    DlqReprocessor<EventV1> reprocessor;
    reprocessor.register_migration<EventV2>(
        "v1->v2",
        [](EventV1 old) -> Result<EventV2> {
            // Conversion logic
            return EventV2{old.id, old.data, "reprocessed"};
        },
        [&migrated_events, &mtx](EventV2 v2) -> bool {
            std::lock_guard lock(mtx);
            println("  reprocessed: V2{{id={}, payload={}}}", v2.id, v2.payload);
            migrated_events.push_back(std::move(v2));
            return true;
        });

    println("  registered migration count: {}",
                reprocessor.migration_count());

    // Execute reprocessing
    auto summary = co_await reprocessor.reprocess(dlq);
    println("  reprocess result: migrated={} failed={} skipped={}",
                summary.migrated, summary.failed, summary.skipped);
    println("  remaining DLQ size: {}\n", dlq.size());

    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("=== qbuem SubpipelineAction + Migration Example ===\n");

    demo_subpipeline();

    // Migration demos are coroutine-based; run with simple synchronous execution
    Dispatcher disp(1);
    std::jthread t([&] { disp.run(); });

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

    println("=== Done ===");
    return 0;
}
