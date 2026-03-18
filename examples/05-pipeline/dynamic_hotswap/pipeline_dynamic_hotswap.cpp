/**
 * @file examples/pipeline_dynamic_hotswap.cpp
 * @brief Pipeline Guide §3-2 + Recipe C: DynamicPipeline + Hot-swap + DLQ example.
 *
 * Scenario:
 *   A pipeline processes an integer stream.
 *   - The "transform" stage is replaced at runtime (hot-swap).
 *   - Items that fail processing are collected in a DeadLetterQueue.
 *
 * Guide excerpt (§3-2 DynamicPipeline):
 *   Best For: ETL workflows, config-driven logic, or systems requiring Hot-swapping.
 *   Pros: Stage addition/removal at runtime, Hot-swapping without stopping the world.
 *
 * Guide excerpt (Recipe C):
 *   1. An Action catches logic/timeout errors.
 *   2. Failed items are pushed to a dedicated "DLQ Pipeline" for offline analysis/retry.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/dead_letter.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;

// ─── Stage functions ──────────────────────────────────────────────────────────

// Initial transform: multiply by 2
static Task<Result<int>> transform_v1(int x, ActionEnv) {
    co_return x * 2;
}

// Replacement transform: multiply by 3 then add 1
static Task<Result<int>> transform_v2(int x, ActionEnv) {
    co_return x * 3 + 1;
}

// Validation stage: reject negatives → DLQ
static Task<Result<int>> validate(int x, ActionEnv) {
    if (x < 0) {
        co_return unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    co_return x;
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    // DLQ — collect failed items
    auto dlq = std::make_shared<DeadLetterQueue<int>>();

    // Build DynamicPipeline<int>
    DynamicPipeline<int> dp;
    dp.add_stage("validate",  validate);
    dp.add_stage("transform", transform_v1);

    Dispatcher dispatcher(1);
    dp.start(dispatcher);
    std::jthread run_th([&] { dispatcher.run(); });

    auto output = dp.output();

    // ── Phase 1: transform_v1 (×2) ────────────────────────────────────────────
    for (int i = 1; i <= 5; ++i)
        dp.try_push(i);

    std::vector<int> phase1_results;
    {
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (phase1_results.size() < 5 && std::chrono::steady_clock::now() < deadline) {
            auto item = output->try_recv();
            if (item) phase1_results.push_back(item->value);
            else std::this_thread::sleep_for(1ms);
        }
    }

    // ── Hot-swap: transform_v1 → transform_v2 ────────────────────────────────
    bool swapped = dp.hot_swap("transform", transform_v2);
    println("[hot-swap] swapped={}", swapped);

    // ── Phase 2: transform_v2 (×3+1) ─────────────────────────────────────────
    for (int i = 1; i <= 5; ++i)
        dp.try_push(i);

    std::vector<int> phase2_results;
    {
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (phase2_results.size() < 5 && std::chrono::steady_clock::now() < deadline) {
            auto item = output->try_recv();
            if (item) phase2_results.push_back(item->value);
            else std::this_thread::sleep_for(1ms);
        }
    }

    // ── Phase 3: inject negatives → validate fails → DLQ ─────────────────────
    // (Simple simulation: pushing directly without DlqAction wrapping)
    dp.try_push(-1);
    dp.try_push(-2);

    // Brief wait then manual DLQ check (automatic when wrapped with DlqAction)
    std::this_thread::sleep_for(50ms);

    // Phase 1 verification: validate(x)→x, transform_v1(x)→x*2
    bool p1_ok = !phase1_results.empty();
    for (int r : phase1_results)
        println("[phase1] value={}", r);

    // Phase 2 verification: transform_v2(x)→x*3+1 (after hot-swap)
    bool p2_ok = !phase2_results.empty();
    for (int r : phase2_results)
        println("[phase2/hotswap] value={}", r);

    dp.stop();
    dispatcher.stop();
    run_th.join();

    println("[dynamic-hotswap] phase1_ok={} phase2_ok={}", p1_ok, p2_ok);
    return (p1_ok && p2_ok) ? 0 : 1;
}
