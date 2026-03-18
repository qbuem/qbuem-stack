/**
 * @file resilience_example.cpp
 * @brief Payment gateway — RetryAction + CircuitBreaker + DeadLetterQueue composite example.
 *
 * ## Scenario
 * A payment service calls an external PG (Payment Gateway).
 * - Transient failure (timeout) → RetryAction (max 3 attempts, exponential backoff) retries
 * - Consecutive PG failures → CircuitBreaker transitions to OPEN state, fast-fail
 * - Retry limit exceeded or circuit breaker open → DeadLetterQueue stores failed payments
 * - DLQ items reprocessed later (after human review)
 *
 * ## Coverage
 * - RetryConfig: max_attempts / base_delay / strategy(Exponential/Fixed/Jitter)
 * - RetryAction<In, Out>: operator() retry logic
 * - CircuitBreakerConfig: failure_threshold / timeout / on_state_change
 * - CircuitBreaker: allow_request / record_success / record_failure / state
 * - CircuitBreakerAction<In, Out>: fast-fail when Open
 * - DeadLetterQueue<T>: push / size / drain / peek / reprocess
 * - DlqAction<In, Out>: auto DLQ storage on inner fn failure
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/circuit_breaker.hpp>
#include <qbuem/pipeline/dead_letter.hpp>
#include <qbuem/pipeline/retry_policy.hpp>
#include <qbuem/compat/print.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Domain types ─────────────────────────────────────────────────────────────

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

// ─── Mock PG call (failure counter simulates transient outage) ───────────────

static std::atomic<int> g_pg_call_count{0};
static std::atomic<int> g_fail_until{3};  // first N calls fail

static Task<Result<PaymentReceipt>> call_pg(Payment p, ActionEnv /*env*/) {
    int n = ++g_pg_call_count;
    if (n <= g_fail_until.load()) {
        std::println("  [PG] call #{} → transient failure (timeout)", n);
        co_return unexpected(std::make_error_code(std::errc::timed_out));
    }
    std::println("  [PG] call #{} → approved (payment_id={})",
                n, p.payment_id);
    co_return PaymentReceipt{p.payment_id, "PG-TXN-" + std::to_string(n), true};
}

// ─── RunGuard ─────────────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher  dispatcher;
    std::jthread thread;
    explicit RunGuard(size_t threads = 1) : dispatcher(threads) {
        thread = std::jthread([this] { dispatcher.run(); });
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

// ─── 1. RetryAction standalone demo ──────────────────────────────────────────

static void demo_retry() {
    std::println("\n=== [RetryAction] Exponential backoff retry ===");

    g_pg_call_count.store(0);
    g_fail_until.store(2);  // first 2 calls fail → 3rd succeeds

    RetryConfig cfg{
        .max_attempts = 5,
        .base_delay   = 1ms,   // short delay for testing
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
        std::println("[RetryAction] final result: txn={} approved={}",
                    receipt.pg_txn_id, receipt.approved);
        std::println("[RetryAction] total PG calls: {} (2 retries + 1 success)",
                    g_pg_call_count.load());
    } else {
        std::println("[RetryAction] FAIL (unexpected failure)");
    }
}

// ─── 2. CircuitBreaker state transition demo ──────────────────────────────────

static void demo_circuit_breaker() {
    std::println("\n=== [CircuitBreaker] Consecutive failures → OPEN → HalfOpen → Closed ===");

    // threshold=3: opens after 3 failures
    CircuitBreakerConfig cb_cfg{
        .failure_threshold = 3,
        .success_threshold = 2,
        .timeout           = 50ms,
        .on_state_change   = [](auto from, auto to) {
            const char* names[] = {"Closed", "Open", "HalfOpen"};
            std::println("  [CircuitBreaker] state transition: {} → {}",
                        names[static_cast<int>(from)],
                        names[static_cast<int>(to)]);
        }
    };
    CircuitBreaker cb(cb_cfg);

    // 3 failures → OPEN
    for (int i = 0; i < 3; ++i) {
        if (cb.allow_request()) {
            std::println("  [CB] request allowed (failure {})", i + 1);
            cb.record_failure();
        }
    }
    std::println("[CircuitBreaker] state={}, failures={}",
                cb.state() == CircuitBreaker::State::Open ? "OPEN" : "OTHER",
                cb.failure_count());

    // OPEN state: fast-fail
    bool blocked = !cb.allow_request();
    std::println("[CircuitBreaker] request blocked in OPEN: {}",
                blocked ? "YES" : "NO");

    // Wait for timeout → HalfOpen
    std::this_thread::sleep_for(60ms);

    if (cb.allow_request()) {
        std::println("  [CB] HalfOpen: probe request allowed");
        cb.record_success();
    }
    if (cb.allow_request()) {
        std::println("  [CB] HalfOpen: second probe allowed");
        cb.record_success();  // success_threshold=2 → Closed
    }
    std::println("[CircuitBreaker] final state={}",
                cb.state() == CircuitBreaker::State::Closed ? "CLOSED" : "OTHER");
}

// ─── 3. CircuitBreakerAction + DlqAction composite ───────────────────────────

static void demo_cb_with_dlq() {
    std::println("\n=== [CircuitBreakerAction + DlqAction] Payment pipeline ===");

    g_pg_call_count.store(0);
    g_fail_until.store(100);  // all PG calls fail (for CB testing)

    // CircuitBreaker: opens after 2 failures
    auto cb = std::make_shared<CircuitBreaker>(CircuitBreakerConfig{
        .failure_threshold = 2,
        .timeout           = 100ms,
    });

    // DLQ: stores failed payments
    auto dlq = std::make_shared<DeadLetterQueue<Payment>>(
        DeadLetterQueue<Payment>::Config{.max_size = 100});

    // CircuitBreakerAction wraps DlqAction
    // inner: PG call
    // DlqAction: after 2 attempts, route to DLQ
    // CircuitBreakerAction: reflects CB state

    auto pg_with_dlq = DlqAction<Payment, PaymentReceipt>(
        [&](Payment p, ActionEnv env) -> Task<Result<PaymentReceipt>> {
            // Fast-fail if circuit is open
            if (!cb->allow_request()) {
                std::println("  [CB] circuit open — fast-fail");
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

    // Send 5 payments (all fail → DLQ)
    for (uint64_t i = 1; i <= 5; ++i) {
        Payment p{i * 1000, "tok_" + std::to_string(i), 10000.0 * i};
        guard.run_and_wait([&, p]() -> Task<void> {
            ActionEnv env{};
            auto res = co_await pg_with_dlq(p, env);
            ++processed;
            if (!res)
                std::println("  [payment] payment_id={} → failed ({})",
                            p.payment_id, res.error().message());
        });
    }

    std::println("[DLQ] failed payments stored: {}", dlq->size());

    // Review DLQ contents
    auto letters = dlq->peek(10);
    for (auto* l : letters) {
        std::println("  [DLQ] payment_id={} attempt={} error={}",
                    l->item.payment_id, l->attempt_count,
                    l->error.message());
    }

    // Reprocess DLQ after CB timeout
    std::println("\n[reprocess] Waiting for CB timeout then draining DLQ");
    std::this_thread::sleep_for(120ms);
    g_fail_until.store(0);  // PG is now healthy
    cb->reset();

    auto drained = dlq->drain();
    std::println("[reprocess] drained {} items", drained.size());

    int reprocessed_ok = 0;
    guard.run_and_wait([&]() -> Task<void> {
        for (auto& letter : drained) {
            ActionEnv env{};
            auto res = co_await call_pg(letter.item, env);
            if (res) {
                ++reprocessed_ok;
                std::println("  [reprocess] payment_id={} → approved",
                            letter.item.payment_id);
            }
        }
    });

    std::println("[reprocess] success: {}/{}", reprocessed_ok, drained.size());
}

// ─── 4. Jitter backoff strategy ───────────────────────────────────────────────

static void demo_jitter_backoff() {
    std::println("\n=== [RetryAction Jitter] Prevent simultaneous retry storms ===");

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

    std::println("[Jitter] input=21, result={}, total_calls={}", result, calls.load());
}

int main() {
    demo_retry();
    demo_circuit_breaker();
    demo_cb_with_dlq();
    demo_jitter_backoff();
    std::println("\nresilience_example: ALL OK");
    return 0;
}
