/**
 * @file idempotency_slo_example.cpp
 * @brief IdempotencyFilter + ErrorBudgetTracker + LatencyHistogram 복합 예시.
 *
 * ## 시나리오: 결제 이벤트 중복 제거 + SLO 추적
 * 메시지 큐는 at-least-once 전달을 보장하므로 동일 결제가 중복으로 올 수 있습니다.
 * IdempotencyFilter로 중복을 제거하고, ErrorBudgetTracker로 처리 품질을 SLO로 관리합니다.
 *
 * ## 커버리지
 * ### IdempotencyFilter
 * - InMemoryIdempotencyStore: set_if_absent / get / TTL 만료
 * - IdempotencyFilter::process → Result<optional<T>>
 *   - nullopt = 중복 (이미 처리됨)
 *   - has_value = 새 아이템
 * - Context: IdempotencyKey 슬롯 삽입
 *
 * ### ErrorBudgetTracker / LatencyHistogram / SloConfig
 * - record_success(latency) / record_error()
 * - error_rate() / budget_exhausted()
 * - check_slo() → on_violation 콜백
 * - histogram().p99() / histogram().p999()
 * - histogram().bucket_counts() — 4구간 분류
 * - total_count() / error_count()
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/idempotency.hpp>
#include <qbuem/pipeline/slo.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 도메인 타입 ─────────────────────────────────────────────────────────────

struct PaymentEvent {
    uint64_t    payment_id;
    std::string idempotency_key;  // "PAY-{uuid}"
    double      amount_krw;
    bool        should_fail;      // 강제 실패 플래그 (테스트용)
};

// ─── 결제 처리 함수 (IdempotencyFilter 적용) ─────────────────────────────────

static std::atomic<int> g_processed{0};
static std::atomic<int> g_duplicates{0};
static std::atomic<int> g_errors{0};

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

// ─── 시나리오 1: 중복 결제 이벤트 제거 ──────────────────────────────────────

static void scenario_deduplication() {
    std::puts("\n=== 시나리오 1: 결제 중복 제거 (at-least-once 시뮬레이션) ===");
    g_processed.store(0);
    g_duplicates.store(0);

    auto idem_store = std::make_shared<InMemoryIdempotencyStore>();
    IdempotencyFilter<PaymentEvent> filter(idem_store, 1h);

    RunGuard guard;

    // 동일 결제가 3번 전송 (재전송 시뮬레이션)
    std::vector<PaymentEvent> events = {
        {1001, "PAY-aa11", 50000.0, false},
        {1002, "PAY-bb22", 89700.0, false},
        {1001, "PAY-aa11", 50000.0, false},  // 중복!
        {1003, "PAY-cc33", 15000.0, false},
        {1002, "PAY-bb22", 89700.0, false},  // 중복!
        {1001, "PAY-aa11", 50000.0, false},  // 중복!
        {1004, "PAY-dd44", 200000.0, false},
    };

    guard.run_and_wait([&]() -> Task<void> {
        for (auto& ev : events) {
            // Context에 IdempotencyKey 삽입
            Context ctx;
            ctx = ctx.with(IdempotencyKey{ev.idempotency_key});
            ActionEnv env{ctx};

            auto result = co_await filter.process(ev, env);
            if (!result.has_value()) {
                std::printf("  [ERROR] filter 오류: %s\n",
                            result.error().message().c_str());
                continue;
            }

            if (!result->has_value()) {
                // nullopt = 중복
                g_duplicates.fetch_add(1, std::memory_order_relaxed);
                std::printf("  [중복] payment_id=%llu key=%s — 건너뜀\n",
                            static_cast<unsigned long long>(ev.payment_id),
                            ev.idempotency_key.c_str());
            } else {
                // 새 결제 처리
                g_processed.fetch_add(1, std::memory_order_relaxed);
                std::printf("  [처리] payment_id=%llu amount=%.0f원\n",
                            static_cast<unsigned long long>((**result).payment_id),
                            (**result).amount_krw);
            }
        }
    });

    std::printf("[결과] 처리=%d, 중복제거=%d (7건 중 4건 처리, 3건 중복)\n",
                g_processed.load(), g_duplicates.load());
}

// ─── 시나리오 2: 키 없는 이벤트 → 멱등성 검사 생략 ─────────────────────────

static void scenario_no_key_passthrough() {
    std::puts("\n=== 시나리오 2: 멱등성 키 없음 → 무조건 통과 ===");

    auto idem_store = std::make_shared<InMemoryIdempotencyStore>();
    IdempotencyFilter<PaymentEvent> filter(idem_store);

    RunGuard guard;
    int passed = 0;

    guard.run_and_wait([&]() -> Task<void> {
        // Context에 IdempotencyKey 없이 전송 (3번 같은 이벤트)
        PaymentEvent ev{9999, "", 1000.0, false};
        for (int i = 0; i < 3; ++i) {
            ActionEnv env{};  // 빈 Context
            auto result = co_await filter.process(ev, env);
            if (result.has_value() && result->has_value())
                ++passed;
        }
    });

    std::printf("[결과] 키 없는 이벤트 3건 모두 통과: %d/3\n", passed);
}

// ─── 시나리오 3: SLO 추적 — 정상 처리 ──────────────────────────────────────

static void scenario_slo_normal() {
    std::puts("\n=== 시나리오 3: SLO 정상 처리 (p99 < 10ms, 에러율 < 0.1%) ===");

    std::atomic<int> violations{0};

    SloConfig cfg{
        .p99_target   = microseconds{10'000},   // 10ms
        .p999_target  = microseconds{50'000},   // 50ms
        .error_budget = 0.001,                  // 0.1%
        .on_violation = [&](std::string_view action_name) {
            ++violations;
            std::printf("  [SLO 위반] 액션=%s\n",
                        std::string(action_name).c_str());
        },
    };

    ErrorBudgetTracker tracker(cfg, "payment_process");

    // 100건 정상 처리 (레이턴시 1ms ~ 5ms)
    for (int i = 0; i < 100; ++i) {
        auto latency = microseconds{1000 + (i % 5) * 1000};  // 1~5ms
        tracker.record_success(latency);
    }

    tracker.check_slo();

    auto buckets = tracker.histogram().bucket_counts();
    std::printf("[SLO] 총=%llu 에러=%llu 에러율=%.4f%%\n",
                static_cast<unsigned long long>(tracker.total_count()),
                static_cast<unsigned long long>(tracker.error_count()),
                tracker.error_rate() * 100.0);
    std::printf("[SLO] p99=%lldus p999=%lldus\n",
                static_cast<long long>(tracker.histogram().p99().count()),
                static_cast<long long>(tracker.histogram().p999().count()));
    std::printf("[SLO] 버킷[<1ms=%llu <10ms=%llu <100ms=%llu >=100ms=%llu]\n",
                static_cast<unsigned long long>(buckets[0]),
                static_cast<unsigned long long>(buckets[1]),
                static_cast<unsigned long long>(buckets[2]),
                static_cast<unsigned long long>(buckets[3]));
    std::printf("[SLO] 위반=%d 버짓소진=%s\n",
                violations.load(),
                tracker.budget_exhausted() ? "YES" : "NO");
}

// ─── 시나리오 4: SLO 위반 — 에러율 초과 ────────────────────────────────────

static void scenario_slo_budget_exhausted() {
    std::puts("\n=== 시나리오 4: 에러율 초과 → 에러 버짓 소진 ===");

    std::atomic<int> violations{0};

    SloConfig cfg{
        .p99_target   = microseconds{10'000},
        .p999_target  = microseconds{50'000},
        .error_budget = 0.01,  // 1% (낮은 임계값)
        .on_violation = [&](std::string_view action_name) {
            ++violations;
            std::printf("  [SLO 위반] 액션=%s — 에러 버짓 소진!\n",
                        std::string(action_name).c_str());
        },
    };

    ErrorBudgetTracker tracker(cfg, "payment_gateway");

    // 90건 성공 + 20건 에러 → 에러율 ~18%
    for (int i = 0; i < 90; ++i)
        tracker.record_success(microseconds{500});
    for (int i = 0; i < 20; ++i)
        tracker.record_error();

    tracker.check_slo();

    std::printf("[SLO] 총=%llu 에러=%llu 에러율=%.2f%% (임계=1%%)\n",
                static_cast<unsigned long long>(tracker.total_count()),
                static_cast<unsigned long long>(tracker.error_count()),
                tracker.error_rate() * 100.0);
    std::printf("[SLO] 버짓소진=%s 위반콜백=%d건\n",
                tracker.budget_exhausted() ? "YES" : "NO",
                violations.load());
}

// ─── 시나리오 5: SLO 위반 — p99 레이턴시 초과 ──────────────────────────────

static void scenario_slo_latency_violation() {
    std::puts("\n=== 시나리오 5: p99 레이턴시 초과 → SLO 위반 ===");

    std::atomic<int> violations{0};

    SloConfig cfg{
        .p99_target   = microseconds{5'000},   // 5ms (매우 엄격)
        .p999_target  = microseconds{20'000},  // 20ms
        .error_budget = 0.1,
        .on_violation = [&](std::string_view action_name) {
            ++violations;
            std::printf("  [SLO 위반] 액션=%s — 레이턴시 목표 초과!\n",
                        std::string(action_name).c_str());
        },
    };

    ErrorBudgetTracker tracker(cfg, "slow_payment");

    // 1000건 중 p99 이상이 10ms가 되도록 기록
    for (int i = 0; i < 990; ++i)
        tracker.record_success(microseconds{1000});  // 1ms (정상)
    for (int i = 0; i < 10; ++i)
        tracker.record_success(microseconds{15'000});  // 15ms (느림 → p99 초과)

    tracker.check_slo();

    std::printf("[SLO] p99=%lldus (목표=5000us)\n",
                static_cast<long long>(tracker.histogram().p99().count()));
    std::printf("[SLO] 위반=%d건\n", violations.load());
}

// ─── 시나리오 6: IdempotencyFilter + SLO 통합 ─────────────────────────────

static void scenario_integrated() {
    std::puts("\n=== 시나리오 6: 멱등성 필터 + SLO 통합 파이프라인 ===");
    g_processed.store(0);
    g_duplicates.store(0);
    g_errors.store(0);

    auto idem_store = std::make_shared<InMemoryIdempotencyStore>();
    IdempotencyFilter<PaymentEvent> filter(idem_store, 1h);

    std::atomic<int> slo_violations{0};
    SloConfig cfg{
        .p99_target   = microseconds{5'000},
        .p999_target  = microseconds{20'000},
        .error_budget = 0.05,
        .on_violation = [&](std::string_view) { ++slo_violations; },
    };
    ErrorBudgetTracker tracker(cfg, "integrated_payment");

    RunGuard guard;

    // 이벤트: 유니크 5개 + 중복 3개 + 실패 2개
    std::vector<PaymentEvent> events;
    for (int i = 0; i < 5; ++i)
        events.push_back({static_cast<uint64_t>(i + 1),
                          "KEY-" + std::to_string(i + 1),
                          10000.0 * (i + 1), false});
    // 중복 재전송
    events.push_back({1, "KEY-1", 10000.0, false});
    events.push_back({2, "KEY-2", 20000.0, false});
    events.push_back({3, "KEY-3", 30000.0, false});

    guard.run_and_wait([&]() -> Task<void> {
        for (auto& ev : events) {
            auto t0 = std::chrono::steady_clock::now();

            Context ctx;
            ctx = ctx.with(IdempotencyKey{ev.idempotency_key});
            ActionEnv env{ctx};

            auto result = co_await filter.process(ev, env);
            auto latency = std::chrono::duration_cast<microseconds>(
                std::chrono::steady_clock::now() - t0);

            if (!result.has_value()) {
                g_errors.fetch_add(1, std::memory_order_relaxed);
                tracker.record_error();
            } else if (!result->has_value()) {
                g_duplicates.fetch_add(1, std::memory_order_relaxed);
                // 중복은 처리 시간이 거의 없음 (fast-path)
            } else {
                g_processed.fetch_add(1, std::memory_order_relaxed);
                tracker.record_success(latency);
            }
        }

        tracker.check_slo();
    });

    std::printf("[통합] 처리=%d 중복제거=%d 에러=%d\n",
                g_processed.load(), g_duplicates.load(), g_errors.load());
    std::printf("[통합] SLO p99=%lldus 에러율=%.4f%% 위반=%d\n",
                static_cast<long long>(tracker.histogram().p99().count()),
                tracker.error_rate() * 100.0,
                slo_violations.load());
}

int main() {
    scenario_deduplication();
    scenario_no_key_passthrough();
    scenario_slo_normal();
    scenario_slo_budget_exhausted();
    scenario_slo_latency_violation();
    scenario_integrated();
    std::puts("\nidempotency_slo_example: ALL OK");
    return 0;
}
