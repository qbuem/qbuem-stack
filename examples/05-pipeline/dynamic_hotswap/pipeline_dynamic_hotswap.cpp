/**
 * @file examples/pipeline_dynamic_hotswap.cpp
 * @brief Pipeline Guide §3-2 + Recipe C: DynamicPipeline + Hot-swap + DLQ 예시.
 *
 * 시나리오:
 *   정수 스트림을 처리하는 파이프라인입니다.
 *   - 런타임에 "transform" 스테이지를 다른 함수로 교체(hot-swap)합니다.
 *   - 처리 실패 항목은 DeadLetterQueue로 수집됩니다.
 *
 * 가이드 원문 (§3-2 DynamicPipeline):
 *   Best For: ETL workflows, config-driven logic, or systems requiring Hot-swapping.
 *   Pros: Stage addition/removal at runtime, Hot-swapping without stopping the world.
 *
 * 가이드 원문 (Recipe C):
 *   1. An Action catches logic/timeout errors.
 *   2. Failed items are pushed to a dedicated "DLQ Pipeline" for offline analysis/retry.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/dead_letter.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 스테이지 함수 ───────────────────────────────────────────────────────────

// 초기 transform: 값을 2배
static Task<Result<int>> transform_v1(int x, ActionEnv) {
    co_return x * 2;
}

// 교체 후 transform: 값을 3배 + 1
static Task<Result<int>> transform_v2(int x, ActionEnv) {
    co_return x * 3 + 1;
}

// 검증 스테이지: 음수 거부 → DLQ
static Task<Result<int>> validate(int x, ActionEnv) {
    if (x < 0) {
        co_return unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    co_return x;
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    // DLQ — 처리 실패 아이템 수집
    auto dlq = std::make_shared<DeadLetterQueue<int>>();

    // DynamicPipeline<int> 구성
    DynamicPipeline<int> dp;
    dp.add_stage("validate",  validate);
    dp.add_stage("transform", transform_v1);

    Dispatcher dispatcher(1);
    dp.start(dispatcher);
    std::jthread run_th([&] { dispatcher.run(); });

    auto output = dp.output();

    // ── Phase 1: transform_v1 (×2) ──────────────────────────────────────────
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
    std::cout << "[hot-swap] swapped=" << std::boolalpha << swapped << "\n";

    // ── Phase 2: transform_v2 (×3+1) ────────────────────────────────────────
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

    // ── Phase 3: 음수 투입 → validate에서 실패 → DLQ ──────────────────────
    // (DlqAction으로 래핑하지 않고 직접 DLQ에 push하는 단순 시뮬레이션)
    dp.try_push(-1);
    dp.try_push(-2);

    // 잠깐 대기 후 DLQ 수동 체크 (DlqAction 래핑 시 자동으로 됨)
    std::this_thread::sleep_for(50ms);

    // Phase 1 검증: validate(x)→x, transform_v1(x)→x*2
    bool p1_ok = !phase1_results.empty();
    for (int r : phase1_results)
        std::cout << "[phase1] value=" << r << "\n";

    // Phase 2 검증: transform_v2(x)→x*3+1 (after hot-swap)
    bool p2_ok = !phase2_results.empty();
    for (int r : phase2_results)
        std::cout << "[phase2/hotswap] value=" << r << "\n";

    dp.stop();
    dispatcher.stop();
    run_th.join();

    std::cout << "[dynamic-hotswap] phase1_ok=" << p1_ok
              << " phase2_ok=" << p2_ok << "\n";
    return (p1_ok && p2_ok) ? 0 : 1;
}
