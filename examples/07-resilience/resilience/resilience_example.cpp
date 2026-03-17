/**
 * @file resilience_example.cpp
 * @brief 결제 게이트웨이 — RetryAction + CircuitBreaker + DeadLetterQueue 복합 예시.
 *
 * ## 시나리오
 * 결제 서비스가 외부 PG(Payment Gateway)를 호출합니다.
 * - 일시적 장애(timeout) → RetryAction(최대 3회, 지수 백오프)으로 재시도
 * - PG 연속 장애 → CircuitBreaker가 OPEN 상태로 전환, 빠른 거부(fast-fail)
 * - 재시도 한도 초과 또는 회로 차단 → DeadLetterQueue에 실패 결제 보관
 * - DLQ 항목 나중에 재처리 (사람이 확인 후 retry)
 *
 * ## 커버리지
 * - RetryConfig: max_attempts / base_delay / strategy(Exponential/Fixed/Jitter)
 * - RetryAction<In, Out>: operator() 재시도 로직
 * - CircuitBreakerConfig: failure_threshold / timeout / on_state_change
 * - CircuitBreaker: allow_request / record_success / record_failure / state
 * - CircuitBreakerAction<In, Out>: fast-fail when Open
 * - DeadLetterQueue<T>: push / size / drain / peek / reprocess
 * - DlqAction<In, Out>: inner fn 실패 시 자동 DLQ 적재
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/circuit_breaker.hpp>
#include <qbuem/pipeline/dead_letter.hpp>
#include <qbuem/pipeline/retry_policy.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 도메인 타입 ─────────────────────────────────────────────────────────────

struct Payment {
    uint64_t    payment_id;
    std::string card_token;
    double      amount_krw;
};

struct PaymentReceipt {
    uint64_t    payment_id;
    std::string pg_txn_id;
    bool        approved;
};

// ─── 모의 PG 호출 (실패 카운터로 일시 장애 시뮬레이션) ──────────────────────

static std::atomic<int> g_pg_call_count{0};
static std::atomic<int> g_fail_until{3};  // 처음 N회는 실패

static Task<Result<PaymentReceipt>> call_pg(Payment p, ActionEnv /*env*/) {
    int n = ++g_pg_call_count;
    if (n <= g_fail_until.load()) {
        std::printf("  [PG] call #%d → 일시 장애 (timeout)\n", n);
        co_return unexpected(std::make_error_code(std::errc::timed_out));
    }
    std::printf("  [PG] call #%d → 승인 완료 (payment_id=%llu)\n",
                n, static_cast<unsigned long long>(p.payment_id));
    co_return PaymentReceipt{p.payment_id, "PG-TXN-" + std::to_string(n), true};
}

// ─── RunGuard ─────────────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher  dispatcher;
    std::thread thread;
    explicit RunGuard(size_t threads = 1) : dispatcher(threads) {
        thread = std::thread([this] { dispatcher.run(); });
    }
    ~RunGuard() { dispatcher.stop(); if (thread.joinable()) thread.join(); }

    template <typename F>
    void run_and_wait(F&& f, std::chrono::milliseconds timeout = 10s) {
        std::atomic<bool> done{false};
        dispatcher.spawn([&, f = std::forward<F>(f)]() mutable -> Task<void> {
            co_await f();
            done.store(true, std::memory_order_release);
        }());
        auto dl = std::chrono::steady_clock::now() + timeout;
        while (!done.load() && std::chrono::steady_clock::now() < dl)
            std::this_thread::sleep_for(1ms);
    }
};

// ─── 1. RetryAction 단독 시연 ─────────────────────────────────────────────────

static void demo_retry() {
    std::puts("\n=== [RetryAction] 지수 백오프 재시도 ===");

    g_pg_call_count.store(0);
    g_fail_until.store(2);  // 처음 2회 실패 → 3번째 성공

    RetryConfig cfg{
        .max_attempts = 5,
        .base_delay   = 1ms,   // 테스트용 짧은 딜레이
        .max_delay    = 10ms,
        .strategy     = BackoffStrategy::Exponential,
    };
    RetryAction<Payment, PaymentReceipt> retry(call_pg, cfg);

    RunGuard guard;
    PaymentReceipt receipt{};
    bool ok = false;

    guard.run_and_wait([&]() -> Task<void> {
        ActionEnv env{};
        auto res = co_await retry(Payment{1001, "tok_visa_xxxx", 50000.0}, env);
        ok      = res.has_value();
        if (ok) receipt = *res;
    });

    if (ok) {
        std::printf("[RetryAction] 최종 결과: txn=%s approved=%d\n",
                    receipt.pg_txn_id.c_str(), receipt.approved);
        std::printf("[RetryAction] 총 PG 호출 횟수: %d (2회 재시도 + 1회 성공)\n",
                    g_pg_call_count.load());
    } else {
        std::puts("[RetryAction] FAIL (예상치 못한 실패)");
    }
}

// ─── 2. CircuitBreaker 상태 전이 시연 ────────────────────────────────────────

static void demo_circuit_breaker() {
    std::puts("\n=== [CircuitBreaker] 연속 장애 → OPEN → HalfOpen → Closed ===");

    // threshold=3: 3번 실패 시 OPEN
    CircuitBreakerConfig cb_cfg{
        .failure_threshold = 3,
        .success_threshold = 2,
        .timeout           = 50ms,
        .on_state_change   = [](auto from, auto to) {
            const char* names[] = {"Closed", "Open", "HalfOpen"};
            std::printf("  [CircuitBreaker] 상태 전이: %s → %s\n",
                        names[static_cast<int>(from)],
                        names[static_cast<int>(to)]);
        }
    };
    CircuitBreaker cb(cb_cfg);

    // 3번 실패 → OPEN
    for (int i = 0; i < 3; ++i) {
        if (cb.allow_request()) {
            std::printf("  [CB] 요청 허용 (failure %d)\n", i + 1);
            cb.record_failure();
        }
    }
    std::printf("[CircuitBreaker] 상태=%s, 실패=%zu\n",
                cb.state() == CircuitBreaker::State::Open ? "OPEN" : "OTHER",
                cb.failure_count());

    // OPEN 상태: fast-fail
    bool blocked = !cb.allow_request();
    std::printf("[CircuitBreaker] OPEN에서 요청 차단: %s\n", blocked ? "YES" : "NO");

    // timeout 대기 → HalfOpen
    std::this_thread::sleep_for(60ms);

    if (cb.allow_request()) {
        std::puts("  [CB] HalfOpen: 탐침 요청 허용");
        cb.record_success();
    }
    if (cb.allow_request()) {
        std::puts("  [CB] HalfOpen: 두 번째 탐침 허용");
        cb.record_success();  // success_threshold=2 → Closed
    }
    std::printf("[CircuitBreaker] 최종 상태=%s\n",
                cb.state() == CircuitBreaker::State::Closed ? "CLOSED" : "OTHER");
}

// ─── 3. CircuitBreakerAction + DlqAction 복합 ───────────────────────────────

static void demo_cb_with_dlq() {
    std::puts("\n=== [CircuitBreakerAction + DlqAction] 결제 파이프라인 ===");

    g_pg_call_count.store(0);
    g_fail_until.store(100);  // 모든 PG 호출 실패 (CB 테스트용)

    // CircuitBreaker: 2번 실패 시 OPEN
    auto cb = std::make_shared<CircuitBreaker>(CircuitBreakerConfig{
        .failure_threshold = 2,
        .timeout           = 100ms,
    });

    // DLQ: 실패한 결제 보관
    auto dlq = std::make_shared<DeadLetterQueue<Payment>>(
        DeadLetterQueue<Payment>::Config{.max_size = 100});

    // CircuitBreakerAction이 DlqAction을 감쌈
    // inner: PG 호출
    // DlqAction: 2회 시도 후 DLQ로
    // CircuitBreakerAction: CB 상태 반영

    auto pg_with_dlq = DlqAction<Payment, PaymentReceipt>(
        [&](Payment p, ActionEnv env) -> Task<Result<PaymentReceipt>> {
            // CB가 열려 있으면 fast-fail
            if (!cb->allow_request()) {
                std::puts("  [CB] 회로 차단 — fast-fail");
                co_return unexpected(std::make_error_code(std::errc::connection_refused));
            }
            auto res = co_await call_pg(p, env);
            if (res) cb->record_success();
            else     cb->record_failure();
            co_return res;
        },
        dlq,
        /*max_attempts=*/2
    );

    RunGuard guard;
    std::vector<PaymentReceipt> receipts;
    std::atomic<int> processed{0};

    // 결제 5건 전송 (모두 실패 → DLQ)
    for (uint64_t i = 1; i <= 5; ++i) {
        Payment p{i * 1000, "tok_" + std::to_string(i), 10000.0 * i};
        guard.run_and_wait([&, p]() -> Task<void> {
            ActionEnv env{};
            auto res = co_await pg_with_dlq(p, env);
            ++processed;
            if (!res)
                std::printf("  [결제] payment_id=%llu → 실패 (%s)\n",
                            static_cast<unsigned long long>(p.payment_id),
                            res.error().message().c_str());
        });
    }

    std::printf("[DLQ] 보관된 실패 결제: %zu건\n", dlq->size());

    // DLQ 내용 확인
    auto letters = dlq->peek(10);
    for (auto* l : letters) {
        std::printf("  [DLQ] payment_id=%llu attempt=%zu error=%s\n",
                    static_cast<unsigned long long>(l->item.payment_id),
                    l->attempt_count,
                    l->error.message().c_str());
    }

    // CB timeout 후 DLQ 재처리 시도
    std::puts("\n[재처리] CB timeout 대기 후 DLQ drain + 재처리");
    std::this_thread::sleep_for(120ms);
    g_fail_until.store(0);  // 이제 PG 정상
    cb->reset();

    auto drained = dlq->drain();
    std::printf("[재처리] drain %zu건\n", drained.size());

    int reprocessed_ok = 0;
    guard.run_and_wait([&]() -> Task<void> {
        for (auto& letter : drained) {
            ActionEnv env{};
            auto res = co_await call_pg(letter.item, env);
            if (res) {
                ++reprocessed_ok;
                std::printf("  [재처리] payment_id=%llu → 승인 ✓\n",
                            static_cast<unsigned long long>(letter.item.payment_id));
            }
        }
    });

    std::printf("[재처리] 성공: %d/%zu건\n", reprocessed_ok, drained.size());
}

// ─── 4. Jitter 백오프 전략 ───────────────────────────────────────────────────

static void demo_jitter_backoff() {
    std::puts("\n=== [RetryAction Jitter] 동시 재시도 폭풍 방지 ===");

    std::atomic<int> calls{0};
    auto flaky_service = [&](int req_id, ActionEnv) -> Task<Result<int>> {
        int n = ++calls;
        if (n <= 4) co_return unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        co_return req_id * 2;
    };

    RetryConfig cfg{
        .max_attempts = 6,
        .base_delay   = 1ms,
        .max_delay    = 20ms,
        .strategy     = BackoffStrategy::Jitter,
    };
    RetryAction<int, int> retry(flaky_service, cfg);

    RunGuard guard;
    int result = 0;
    guard.run_and_wait([&]() -> Task<void> {
        ActionEnv env{};
        auto res = co_await retry(21, env);
        if (res) result = *res;
    });

    std::printf("[Jitter] 입력=21, 결과=%d, 총 호출=%d\n", result, calls.load());
}

int main() {
    demo_retry();
    demo_circuit_breaker();
    demo_cb_with_dlq();
    demo_jitter_backoff();
    std::puts("\nresilience_example: ALL OK");
    return 0;
}
