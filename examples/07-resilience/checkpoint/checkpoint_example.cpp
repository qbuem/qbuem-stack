/**
 * @file checkpoint_example.cpp
 * @brief CheckpointedPipeline — batch log processor checkpoint composite example.
 *
 * ## Scenario: Real-time log collection pipeline
 * Log events are processed through parse → enrich → persist stages
 * while saving processing progress as checkpoints.
 *
 * ## Stages
 * 1. [Parse]   Raw log string → structured LogEvent
 * 2. [Enrich]  Service name lookup, severity classification
 * 3. [Persist] Increment processed event counter (actual: DB/S3 storage)
 *
 * ## Checkpoint scenarios
 * - Automatic checkpoint save every N items processed
 * - Manual save_checkpoint(): explicit save after batch completion
 * - resume_from_checkpoint(): resume from last offset on restart
 *
 * ## Coverage
 * - CheckpointedPipeline<T>: constructor / pipeline() / enable_checkpoint
 * - push_counted(): item delivery + counter increment
 * - save_checkpoint(): manual save (with metadata_json)
 * - resume_from_checkpoint(): restore saved offset
 * - items_processed(): query cumulative processed count
 * - InMemoryCheckpointStore: save / load / size
 * - CheckpointData: offset / metadata_json / saved_at
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/checkpoint.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/compat/print.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Domain types ─────────────────────────────────────────────────────────────

struct LogEvent {
    uint64_t    event_id;
    std::string raw;
    std::string service;
    int         severity;  // 0=INFO 1=WARN 2=ERROR
    bool        parsed    = false;
    bool        enriched  = false;
    bool        persisted = false;
};

// ─── Pipeline stages ─────────────────────────────────────────────────────────

static std::atomic<int> g_persisted_count{0};

static Task<Result<LogEvent>> stage_parse(LogEvent ev, ActionEnv /*env*/) {
    // Parse severity from raw string
    if (ev.raw.find("ERROR") != std::string::npos) ev.severity = 2;
    else if (ev.raw.find("WARN") != std::string::npos) ev.severity = 1;
    else ev.severity = 0;
    ev.parsed = true;
    co_return ev;
}

static Task<Result<LogEvent>> stage_enrich(LogEvent ev, ActionEnv /*env*/) {
    // Simulate service registry lookup
    ev.service  = (ev.event_id % 3 == 0) ? "payment"
                : (ev.event_id % 3 == 1) ? "inventory"
                                          : "shipping";
    ev.enriched = true;
    co_return ev;
}

static Task<Result<LogEvent>> stage_persist(LogEvent ev, ActionEnv /*env*/) {
    // In production: write to DB or S3
    ev.persisted = true;
    g_persisted_count.fetch_add(1, std::memory_order_relaxed);
    co_return ev;
}

// ─── RunGuard ─────────────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher  dispatcher;
    std::jthread thread;
    explicit RunGuard(size_t n = 1) : dispatcher(n) {
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

// ─── Scenario 1: Basic batch processing + automatic checkpoint ───────────────

static void scenario_auto_checkpoint() {
    std::println("\n=== Scenario 1: Batch processing + auto checkpoint (every 5 items) ===");
    g_persisted_count.store(0);

    auto store = std::make_shared<InMemoryCheckpointStore>();

    RunGuard guard;
    CheckpointedPipeline<LogEvent> cp("log-pipeline", store);

    cp.pipeline().add_stage("parse",   stage_parse);
    cp.pipeline().add_stage("enrich",  stage_enrich);
    cp.pipeline().add_stage("persist", stage_persist);

    // Automatic checkpoint every 5 items processed
    cp.enable_checkpoint(60s, /*every_n=*/5);
    cp.pipeline().start(guard.dispatcher);

    // Send 10 events
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
                std::println("  [WARN] push_counted failed: {}",
                            res.error().message());
        }
    });

    // Wait briefly for pipeline to finish processing
    std::this_thread::sleep_for(50ms);

    std::println("[result] items_processed={}", cp.items_processed());
    std::println("[result] store checkpoints={}", store->size());

    // Verify checkpoint contents
    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await store->load("log-pipeline");
        if (res.has_value()) {
            std::println("[checkpoint] offset={} metadata={}",
                        res->offset, res->metadata_json);
        } else {
            std::println("[checkpoint] no checkpoint saved");
        }
    });
}

// ─── Scenario 2: Manual checkpoint save ──────────────────────────────────────

static void scenario_manual_checkpoint() {
    std::println("\n=== Scenario 2: Manual checkpoint save ===");
    g_persisted_count.store(0);

    auto store = std::make_shared<InMemoryCheckpointStore>();

    RunGuard guard;
    CheckpointedPipeline<LogEvent> cp("log-pipeline-manual", store);
    cp.pipeline().add_stage("parse",   stage_parse);
    cp.pipeline().add_stage("enrich",  stage_enrich);
    cp.pipeline().add_stage("persist", stage_persist);
    cp.pipeline().start(guard.dispatcher);

    // Send items without checkpoint enabled
    guard.run_and_wait([&]() -> Task<void> {
        for (uint64_t i = 0; i < 7; ++i) {
            LogEvent ev{i + 100, "INFO: manual batch item", "", 0};
            co_await cp.push_counted(std::move(ev));
        }
        // Manual save after batch completion
        auto res = co_await cp.save_checkpoint("{\"phase\":\"first_batch_done\"}");
        if (res.has_value())
            std::println("  [checkpoint] manual save completed");
        else
            std::println("  [checkpoint] save failed: {}",
                        res.error().message());
    });

    std::println("[result] items_processed={}, store={}",
                cp.items_processed(), store->size());
    std::println("[result] checkpoint_enabled={}",
                cp.checkpoint_enabled() ? "YES" : "NO");
}

// ─── Scenario 3: Resume from checkpoint on restart ───────────────────────────

static void scenario_resume_from_checkpoint() {
    std::println("\n=== Scenario 3: Resume from checkpoint (crash recovery simulation) ===");

    auto store = std::make_shared<InMemoryCheckpointStore>();

    // Phase 1: Process 20 items then save checkpoint
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

        std::println("[phase 1] processed={}, checkpoint saved",
                    cp.items_processed());
    }  // RunGuard destroyed → simulates process restart

    // Phase 2: New instance restores from checkpoint
    {
        RunGuard guard;
        CheckpointedPipeline<LogEvent> cp("crash-recovery", store);
        cp.pipeline().add_stage("parse",   stage_parse);
        cp.pipeline().add_stage("enrich",  stage_enrich);
        cp.pipeline().add_stage("persist", stage_persist);
        cp.pipeline().start(guard.dispatcher);

        guard.run_and_wait([&]() -> Task<void> {
            // Before resume — offset is 0
            std::println("[phase 2] offset before restore={}",
                        cp.items_processed());

            auto res = co_await cp.resume_from_checkpoint();
            if (res.has_value()) {
                std::println("[phase 2] offset after restore={} (reprocessing from position 20)",
                            cp.items_processed());
            } else {
                std::println("[phase 2] restore failed: {}",
                            res.error().message());
            }
        });
    }
}

// ─── Scenario 4: Attempt to resume with no checkpoint ────────────────────────

static void scenario_resume_no_checkpoint() {
    std::println("\n=== Scenario 4: No checkpoint → error handling ===");

    auto store = std::make_shared<InMemoryCheckpointStore>();
    RunGuard guard;
    CheckpointedPipeline<LogEvent> cp("fresh-pipeline", store);
    cp.pipeline().add_stage("parse", stage_parse);
    cp.pipeline().start(guard.dispatcher);

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await cp.resume_from_checkpoint();
        if (!res.has_value()) {
            std::println("[result] confirmed no checkpoint: error={}",
                        res.error().message());
        }
    });

    std::println("[result] items_processed={} (unchanged)",
                cp.items_processed());
}

int main() {
    scenario_auto_checkpoint();
    scenario_manual_checkpoint();
    scenario_resume_from_checkpoint();
    scenario_resume_no_checkpoint();
    std::println("\ncheckpoint_example: ALL OK");
    return 0;
}
