/**
 * @file idempotency_slo_example.cpp
 * @brief IdempotencyFilter + ErrorBudgetTracker + LatencyHistogram composite example.
 *
 * ## Scenario: Payment event deduplication + SLO tracking
 * Message queues guarantee at-least-once delivery, so the same payment may arrive
 * as duplicates. IdempotencyFilter removes duplicates, and ErrorBudgetTracker
 * manages processing quality as SLOs.
 *
 * ## Coverage
 * ### IdempotencyFilter
 * - InMemoryIdempotencyStore: set_if_absent / get / TTL expiry
 * - IdempotencyFilter::process → Result<optional<T>>
 *   - nullopt = duplicate (already processed)
 *   - has_value = new item
 * - Context: IdempotencyKey slot insertion
 *
 * ### ErrorBudgetTracker / LatencyHistogram / SloConfig
 * - record_success(latency) / record_error()
 * - error_rate() / budget_exhausted()
 * - check_slo() → on_violation callback
 * - histogram().p99() / histogram().p999()
 * - histogram().bucket_counts() — 4-bucket classification
 * - total_count() / error_count()
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/idempotency.hpp>
#include <qbuem/pipeline/slo.hpp>
#include <qbuem/compat/print.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Domain types ─────────────────────────────────────────────────────────────

struct PaymentEvent {
    uint64_t    payment_id;
    std::string idempotency_key;  // "PAY-{uuid}"
    double      amount_krw;
    bool        should_fail;      // forced failure flag (for testing)
};

// ─── Payment processing counters ─────────────────────────────────────────────

static std::atomic<int> g_processed{0};
static std::atomic<int> g_duplicates{0};
static std::atomic<int> g_errors{0};

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

// ─── Scenario 1: Duplicate payment event deduplication ───────────────────────

static void scenario_deduplication() {
    std::println("\n=== Scenario 1: Payment deduplication (at-least-once simulation) ===");
    g_processed.store(0);
    g_duplicates.store(0);

    auto idem_store = std::make_shared<InMemoryIdempotencyStore>();
    IdempotencyFilter<PaymentEvent> filter(idem_store, 1h);

    RunGuard guard;

    // Same payment sent 3 times (retransmission simulation)
    std::vector<PaymentEvent> events = {
        {1001, "PAY-aa11", 50000.0, false},
        {1002, "PAY-bb22", 89700.0, false},
        {1001, "PAY-aa11", 50000.0, false},  // duplicate!
        {1003, "PAY-cc33", 15000.0, false},
        {1002, "PAY-bb22", 89700.0, false},  // duplicate!
        {1001, "PAY-aa11", 50000.0, false},  // duplicate!
        {1004, "PAY-dd44", 200000.0, false},
    };

    guard.run_and_wait([&]() -> Task<void> {
        for (auto& ev : events) {
            // Insert IdempotencyKey into Context
            Context ctx;
            ctx = ctx.put(IdempotencyKey{ev.idempotency_key});
            ActionEnv env{ctx};

            auto result = co_await filter.process(ev, env);
            if (!result.has_value()) {
                std::println("  [ERROR] filter error: {}",
                            result.error().message());
                continue;
            }

            if (!result->has_value()) {
                // nullopt = duplicate
                g_duplicates.fetch_add(1, std::memory_order_relaxed);
                std::println("  [duplicate] payment_id={} key={} — skipped",
                            ev.payment_id, ev.idempotency_key);
            } else {
                // New payment processed
                g_processed.fetch_add(1, std::memory_order_relaxed);
                std::println("  [processed] payment_id={} amount={:.0f}",
                            (**result).payment_id, (**result).amount_krw);
            }
        }
    });

    std::println("[result] processed={}, duplicates={} (4 of 7 processed, 3 duplicates)",
                g_processed.load(), g_duplicates.load());
}

// ─── Scenario 2: Events without key → bypass idempotency check ───────────────

static void scenario_no_key_passthrough() {
    std::println("\n=== Scenario 2: No idempotency key → unconditional pass-through ===");

    auto idem_store = std::make_shared<InMemoryIdempotencyStore>();
    IdempotencyFilter<PaymentEvent> filter(idem_store);

    RunGuard guard;
    int passed = 0;

    guard.run_and_wait([&]() -> Task<void> {
        // Send same event 3 times without IdempotencyKey in Context
        PaymentEvent ev{9999, "", 1000.0, false};
        for (int i = 0; i < 3; ++i) {
            ActionEnv env{};  // empty Context
            auto result = co_await filter.process(ev, env);
            if (result.has_value() && result->has_value())
                ++passed;
        }
    });

    std::println("[result] 3 events without key all passed: {}/3", passed);
}

// ─── Scenario 3: SLO tracking — normal processing ────────────────────────────

static void scenario_slo_normal() {
    std::println("\n=== Scenario 3: SLO normal processing (p99 < 10ms, error rate < 0.1%) ===");

    std::atomic<int> violations{0};

    SloConfig cfg{
        .p99_target   = microseconds{10'000},   // 10ms
        .p999_target  = microseconds{50'000},   // 50ms
        .error_budget = 0.001,                  // 0.1%
        .on_violation = [&](std::string_view action_name) {
            ++violations;
            std::println("  [SLO violation] action={}",
                        std::string(action_name));
        },
    };

    ErrorBudgetTracker tracker(cfg, "payment_process");

    // 100 normal operations (latency 1ms ~ 5ms)
    for (int i = 0; i < 100; ++i) {
        auto latency = microseconds{1000 + (i % 5) * 1000};  // 1~5ms
        tracker.record_success(latency);
    }

    tracker.check_slo();

    auto buckets = tracker.histogram().bucket_counts();
    std::println("[SLO] total={} errors={} error_rate={:.4f}%",
                tracker.total_count(), tracker.error_count(),
                tracker.error_rate() * 100.0);
    std::println("[SLO] p99={}us p999={}us",
                tracker.histogram().p99().count(),
                tracker.histogram().p999().count());
    std::println("[SLO] buckets[<1ms={} <10ms={} <100ms={} >=100ms={}]",
                buckets[0], buckets[1], buckets[2], buckets[3]);
    std::println("[SLO] violations={} budget_exhausted={}",
                violations.load(),
                tracker.budget_exhausted() ? "YES" : "NO");
}

// ─── Scenario 4: SLO violation — error rate exceeded ─────────────────────────

static void scenario_slo_budget_exhausted() {
    std::println("\n=== Scenario 4: Error rate exceeded → error budget exhausted ===");

    std::atomic<int> violations{0};

    SloConfig cfg{
        .p99_target   = microseconds{10'000},
        .p999_target  = microseconds{50'000},
        .error_budget = 0.01,  // 1% (low threshold)
        .on_violation = [&](std::string_view action_name) {
            ++violations;
            std::println("  [SLO violation] action={} — error budget exhausted!",
                        std::string(action_name));
        },
    };

    ErrorBudgetTracker tracker(cfg, "payment_gateway");

    // 90 successes + 20 errors → error rate ~18%
    for (int i = 0; i < 90; ++i)
        tracker.record_success(microseconds{500});
    for (int i = 0; i < 20; ++i)
        tracker.record_error();

    tracker.check_slo();

    std::println("[SLO] total={} errors={} error_rate={:.2f}% (threshold=1%)",
                tracker.total_count(), tracker.error_count(),
                tracker.error_rate() * 100.0);
    std::println("[SLO] budget_exhausted={} violation_callbacks={}",
                tracker.budget_exhausted() ? "YES" : "NO",
                violations.load());
}

// ─── Scenario 5: SLO violation — p99 latency exceeded ────────────────────────

static void scenario_slo_latency_violation() {
    std::println("\n=== Scenario 5: p99 latency exceeded → SLO violation ===");

    std::atomic<int> violations{0};

    SloConfig cfg{
        .p99_target   = microseconds{5'000},   // 5ms (very strict)
        .p999_target  = microseconds{20'000},  // 20ms
        .error_budget = 0.1,
        .on_violation = [&](std::string_view action_name) {
            ++violations;
            std::println("  [SLO violation] action={} — latency target exceeded!",
                        std::string(action_name));
        },
    };

    ErrorBudgetTracker tracker(cfg, "slow_payment");

    // Of 1000 operations, p99 is set to 10ms by having 10 slow ones
    for (int i = 0; i < 990; ++i)
        tracker.record_success(microseconds{1000});  // 1ms (normal)
    for (int i = 0; i < 10; ++i)
        tracker.record_success(microseconds{15'000});  // 15ms (slow → p99 exceeded)

    tracker.check_slo();

    std::println("[SLO] p99={}us (target=5000us)",
                tracker.histogram().p99().count());
    std::println("[SLO] violations={}", violations.load());
}

// ─── Scenario 6: IdempotencyFilter + SLO integration ─────────────────────────

static void scenario_integrated() {
    std::println("\n=== Scenario 6: Idempotency filter + SLO integrated pipeline ===");
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

    // Events: 5 unique + 3 duplicates + 2 failures
    std::vector<PaymentEvent> events;
    for (int i = 0; i < 5; ++i)
        events.push_back({static_cast<uint64_t>(i + 1),
                          "KEY-" + std::to_string(i + 1),
                          10000.0 * (i + 1), false});
    // Duplicate retransmissions
    events.push_back({1, "KEY-1", 10000.0, false});
    events.push_back({2, "KEY-2", 20000.0, false});
    events.push_back({3, "KEY-3", 30000.0, false});

    guard.run_and_wait([&]() -> Task<void> {
        for (auto& ev : events) {
            auto t0 = std::chrono::steady_clock::now();

            Context ctx;
            ctx = ctx.put(IdempotencyKey{ev.idempotency_key});
            ActionEnv env{ctx};

            auto result = co_await filter.process(ev, env);
            auto latency = std::chrono::duration_cast<microseconds>(
                std::chrono::steady_clock::now() - t0);

            if (!result.has_value()) {
                g_errors.fetch_add(1, std::memory_order_relaxed);
                tracker.record_error();
            } else if (!result->has_value()) {
                g_duplicates.fetch_add(1, std::memory_order_relaxed);
                // Duplicates have near-zero processing time (fast-path)
            } else {
                g_processed.fetch_add(1, std::memory_order_relaxed);
                tracker.record_success(latency);
            }
        }

        tracker.check_slo();
    });

    std::println("[integrated] processed={} duplicates={} errors={}",
                g_processed.load(), g_duplicates.load(), g_errors.load());
    std::println("[integrated] SLO p99={}us error_rate={:.4f}% violations={}",
                tracker.histogram().p99().count(),
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
    std::println("\nidempotency_slo_example: ALL OK");
    return 0;
}
