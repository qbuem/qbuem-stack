/**
 * @file saga_example.cpp
 * @brief SagaOrchestrator — distributed transaction (e-commerce order processing) composite example.
 *
 * ## Scenario: Order processing saga
 * When a customer purchases a product, a transaction spanning multiple services is required.
 *
 * Step 1 [Stock reservation]:
 *   execute:    Deduct quantity from product inventory
 *   compensate: Restore inventory (on order cancellation)
 *
 * Step 2 [Payment processing]:
 *   execute:    Authorize credit card payment
 *   compensate: Cancel payment / issue refund
 *
 * Step 3 [Shipment request]:
 *   execute:    Request dispatch from shipping system
 *   compensate: Request dispatch cancellation
 *
 * ## Failure scenarios
 * - Payment failure → auto-execute stock restore compensation transaction
 * - Shipment failure → payment refund + stock restore in reverse order
 *
 * ## Coverage
 * - SagaStep<T, T>: name / execute lambda / compensate lambda
 * - SagaOrchestrator<T>: add_step / run (success/failure paths)
 * - compensation_failures: check compensation failure list
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/saga.hpp>
#include <qbuem/compat/print.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Domain types ─────────────────────────────────────────────────────────────

struct OrderContext {
    uint64_t    order_id;
    uint64_t    product_id;
    int         quantity;
    double      total_krw;
    std::string card_token;

    // Execution result state (filled per step)
    bool stock_reserved  = false;
    bool payment_charged = false;
    bool shipment_queued = false;
    std::string shipment_tracking_id;
    std::string pg_txn_id;
};

// ─── Service state simulation ─────────────────────────────────────────────────

static std::atomic<int> g_stock{10};              // inventory quantity
static std::atomic<int> g_payment_fail_mode{0};  // 0=normal 1=fail
static std::atomic<int> g_shipping_fail_mode{0};

// Compensation execution tracking
static std::vector<std::string> g_compensations;

// ─── Per-step execute/compensate functions ────────────────────────────────────

static Task<Result<OrderContext>> step_reserve_stock(OrderContext ord) {
    if (g_stock.load() < ord.quantity) {
        std::println("  [reserve_stock] insufficient stock (stock={}, requested={})",
                    g_stock.load(), ord.quantity);
        co_return unexpected(std::make_error_code(std::errc::no_space_on_device));
    }
    g_stock.fetch_sub(ord.quantity);
    ord.stock_reserved = true;
    std::println("  [reserve_stock] completed — remaining_stock={}", g_stock.load());
    co_return ord;
}

static Task<void> compensate_reserve_stock(OrderContext ord) {
    g_stock.fetch_add(ord.quantity);
    g_compensations.push_back("stock_restore");
    std::println("  [compensate-stock_restore] +{} restored → remaining={}",
                ord.quantity, g_stock.load());
    co_return;
}

static Task<Result<OrderContext>> step_charge_payment(OrderContext ord) {
    if (g_payment_fail_mode.load()) {
        std::println("  [charge_payment] payment declined (simulation)");
        co_return unexpected(std::make_error_code(std::errc::permission_denied));
    }
    ord.payment_charged = true;
    ord.pg_txn_id = "PG-" + std::to_string(ord.order_id);
    std::println("  [charge_payment] completed — txn={} amount={:.0f}",
                ord.pg_txn_id, ord.total_krw);
    co_return ord;
}

static Task<void> compensate_charge_payment(OrderContext ord) {
    g_compensations.push_back("payment_cancel");
    std::println("  [compensate-payment_cancel] txn={} refund processed",
                ord.pg_txn_id);
    co_return;
}

static Task<Result<OrderContext>> step_request_shipment(OrderContext ord) {
    if (g_shipping_fail_mode.load()) {
        std::println("  [request_shipment] shipping system error (simulation)");
        co_return unexpected(std::make_error_code(std::errc::connection_refused));
    }
    ord.shipment_queued     = true;
    ord.shipment_tracking_id = "SHIP-" + std::to_string(ord.order_id);
    std::println("  [request_shipment] completed — tracking={}",
                ord.shipment_tracking_id);
    co_return ord;
}

static Task<void> compensate_request_shipment(OrderContext ord) {
    g_compensations.push_back("shipment_cancel");
    std::println("  [compensate-shipment_cancel] tracking={} cancelled",
                ord.shipment_tracking_id);
    co_return;
}

// ─── SagaOrchestrator configuration ──────────────────────────────────────────

static SagaOrchestrator<OrderContext> build_order_saga() {
    SagaOrchestrator<OrderContext> saga;
    saga.add_step(SagaStep<OrderContext, OrderContext>{
        .name       = "reserve_stock",
        .execute    = step_reserve_stock,
        .compensate = compensate_reserve_stock,
    });
    saga.add_step(SagaStep<OrderContext, OrderContext>{
        .name       = "charge_payment",
        .execute    = step_charge_payment,
        .compensate = compensate_charge_payment,
    });
    saga.add_step(SagaStep<OrderContext, OrderContext>{
        .name       = "request_shipment",
        .execute    = step_request_shipment,
        .compensate = compensate_request_shipment,
    });
    return saga;
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
    void run_and_wait(F&& f, std::chrono::milliseconds timeout = 5s) {
        std::atomic<bool> done{false};
        dispatcher.spawn([&, f = std::forward<F>(f)]() mutable -> Task<void> {
            co_await f(); done.store(true);
        }());
        auto dl = std::chrono::steady_clock::now() + timeout;
        while (!done.load() && std::chrono::steady_clock::now() < dl)
            std::this_thread::sleep_for(1ms);
    }
};

// ─── Scenario 1: Happy path order ────────────────────────────────────────────

static void scenario_happy_path() {
    std::println("\n=== Scenario 1: Happy path order ===");
    g_stock.store(10);
    g_payment_fail_mode.store(0);
    g_shipping_fail_mode.store(0);
    g_compensations.clear();

    auto saga = build_order_saga();
    OrderContext ord{1001, 42, 2, 59800.0, "tok_visa_4242"};

    RunGuard guard;
    bool success = false;
    OrderContext result{};

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await saga.run(ord, {});
        success  = res.has_value();
        if (success) result = *res;
    });

    std::println("[result] success={} stock_reserved={} payment={} shipment={}",
                success ? "YES" : "NO",
                result.stock_reserved  ? "OK" : "FAIL",
                result.payment_charged ? "OK" : "FAIL",
                result.shipment_queued ? "OK" : "FAIL");
    std::println("[result] tracking={}, remaining_stock={}",
                result.shipment_tracking_id, g_stock.load());
    if (!g_compensations.empty()) std::println("[compensation] unexpected compensation executed!");
}

// ─── Scenario 2: Payment failure → stock compensation ────────────────────────

static void scenario_payment_failure() {
    std::println("\n=== Scenario 2: Payment failure → auto stock restore ===");
    g_stock.store(10);
    g_payment_fail_mode.store(1);  // force payment failure
    g_shipping_fail_mode.store(0);
    g_compensations.clear();

    auto saga = build_order_saga();
    OrderContext ord{1002, 42, 3, 89700.0, "tok_mastercard_5555"};

    RunGuard guard;
    bool failed = false;

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await saga.run(ord, {});
        failed   = !res.has_value();
        if (failed)
            std::println("[result] saga failed: {}", res.error().message());
    });

    std::print("[compensation] executed compensations:");
    for (const auto& c : g_compensations) std::print(" {}", c);
    std::println("");
    std::println("[compensation] stock restored: {} (original=10, deducted=3 before payment failed)",
                g_stock.load());
}

// ─── Scenario 3: Shipment failure → payment refund + stock restore ────────────

static void scenario_shipping_failure() {
    std::println("\n=== Scenario 3: Shipment failure → payment refund + stock restore ===");
    g_stock.store(10);
    g_payment_fail_mode.store(0);
    g_shipping_fail_mode.store(1);  // force shipment failure
    g_compensations.clear();

    auto saga = build_order_saga();
    OrderContext ord{1003, 99, 1, 150000.0, "tok_amex_3782"};

    RunGuard guard;
    bool failed = false;

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await saga.run(ord, {});
        failed   = !res.has_value();
    });

    std::print("[compensation] executed (reverse order):");
    for (const auto& c : g_compensations) std::print(" [{}]", c);
    std::println("");
    std::println("[result] stock={} (10 restored)", g_stock.load());
    // Check compensation failures
    const auto& cf = saga.compensation_failures();
    std::println("[compensation_failures] {} items", cf.size());
}

// ─── Scenario 4: Out of stock ─────────────────────────────────────────────────

static void scenario_out_of_stock() {
    std::println("\n=== Scenario 4: Out of stock (first step failure) ===");
    g_stock.store(1);  // only 1 item in stock
    g_payment_fail_mode.store(0);
    g_shipping_fail_mode.store(0);
    g_compensations.clear();

    auto saga = build_order_saga();
    OrderContext ord{1004, 77, 5, 250000.0, "tok_visa_1234"};  // requesting 5

    RunGuard guard;
    bool failed = false;

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await saga.run(ord, {});
        failed   = !res.has_value();
        if (failed)
            std::println("[result] saga failed: {}", res.error().message());
    });

    // Failed at first step → no compensation needed (nothing executed yet)
    std::println("[compensation] none executed: {}",
                g_compensations.empty() ? "YES" : "NO");
    std::println("[stock] unchanged={}", g_stock.load());
}

int main() {
    scenario_happy_path();
    scenario_payment_failure();
    scenario_shipping_failure();
    scenario_out_of_stock();
    std::println("\nsaga_example: ALL OK");
    return 0;
}
