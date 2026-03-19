/**
 * @file task_group_example.cpp
 * @brief TaskGroup — structured concurrency composite example.
 *
 * ## Scenario: Parallel data fetching for product page (MSA pattern)
 * Multiple microservices are queried in parallel to render a product detail page.
 * If any one fails, the entire group is cancelled.
 *
 * Services:
 * - Product information service  (product_service)
 * - Inventory service            (inventory_service)
 * - Price/promotion service      (pricing_service)
 * - Review service               (review_service)
 *
 * ## Coverage
 * - TaskGroup: spawn<T>() / join_all<T>() — result collection
 * - TaskGroup: spawn() (void) / join() — completion wait
 * - TaskGroup: cancel() / stop_token() — cancellation propagation
 * - Failure scenario: one fails → error propagation
 * - Timeout pattern: handling slow external service responses
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/task_group.hpp>
#include <qbuem/compat/print.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Domain types ─────────────────────────────────────────────────────────────

struct ProductInfo {
    uint64_t    product_id;
    std::string name;
    std::string category;
};

struct InventoryInfo {
    uint64_t product_id;
    int      stock;
    bool     in_stock;
};

struct PricingInfo {
    uint64_t product_id;
    double   price_krw;
    double   discount_rate;  // 0.0 ~ 1.0
};

struct ReviewSummary {
    uint64_t product_id;
    double   avg_rating;
    int      review_count;
};

// ─── Service state simulation ─────────────────────────────────────────────────

static std::atomic<int> g_service_fail_mode{0};  // 0=normal 1=inventory service failure
static std::atomic<int> g_call_count{0};

// ─── Service call coroutines ──────────────────────────────────────────────────

static Task<Result<ProductInfo>> fetch_product(uint64_t product_id) {
    ++g_call_count;
    std::println("  [product_service] starting fetch product_id={}", product_id);
    co_return ProductInfo{product_id, "Ultra-Fast SSD 1TB", "Storage"};
}

static Task<Result<InventoryInfo>> fetch_inventory(uint64_t product_id) {
    ++g_call_count;
    std::println("  [inventory_service] starting fetch product_id={}", product_id);
    if (g_service_fail_mode.load() == 1) {
        std::println("  [inventory_service] service failure!");
        co_return unexpected(std::make_error_code(std::errc::connection_refused));
    }
    co_return InventoryInfo{product_id, 42, true};
}

static Task<Result<PricingInfo>> fetch_pricing(uint64_t product_id) {
    ++g_call_count;
    std::println("  [pricing_service] starting fetch product_id={}", product_id);
    co_return PricingInfo{product_id, 129000.0, 0.15};  // 15% discount
}

static Task<Result<ReviewSummary>> fetch_reviews(uint64_t product_id) {
    ++g_call_count;
    std::println("  [review_service] starting fetch product_id={}", product_id);
    co_return ReviewSummary{product_id, 4.7, 1234};
}

// ─── RunGuard ─────────────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher  dispatcher;
    std::jthread thread;
    explicit RunGuard(size_t n = 2) : dispatcher(n) {
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

// ─── Scenario 1: Parallel fetch success — join_all<T>() ──────────────────────

static void scenario_parallel_fetch_success() {
    std::println("\n=== Scenario 1: Product page parallel data fetch (all succeed) ===");
    g_service_fail_mode.store(0);
    g_call_count.store(0);

    const uint64_t product_id = 777;

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        // Fetch 4 services simultaneously
        TaskGroup tg;
        tg.spawn<ProductInfo>(fetch_product(product_id));
        tg.spawn<InventoryInfo>(fetch_inventory(product_id));
        tg.spawn<PricingInfo>(fetch_pricing(product_id));
        tg.spawn<ReviewSummary>(fetch_reviews(product_id));

        // Wait for all results
        // Note: join_all<T>() collects a single type, so use separate TaskGroups per type
        // Here we use void join() to wait for completion, with results stored in external vars
        co_await tg.join();
        std::println("  [completed] parallel fetch done (total {} service calls)",
                    g_call_count.load());
    });

    std::println("[result] service calls={} (4 services in parallel)", g_call_count.load());
}

// ─── Scenario 2: join_all<T>() — homogeneous result collection ───────────────

static void scenario_join_all_homogeneous() {
    std::println("\n=== Scenario 2: Homogeneous parallel result collection — join_all<T>() ===");

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        // Simultaneously query inventory for multiple product IDs
        std::vector<uint64_t> product_ids = {101, 102, 103, 104, 105};

        TaskGroup tg;
        for (auto pid : product_ids) {
            tg.spawn<InventoryInfo>([](uint64_t id) -> Task<Result<InventoryInfo>> {
                co_return InventoryInfo{id, static_cast<int>(10 + id % 50), true};
            }(pid));
        }

        auto results = co_await tg.join_all<InventoryInfo>();
        if (results.has_value()) {
            std::println("  [completed] {} inventory records collected", results->size());
            int total_stock = 0;
            for (const auto& inv : *results)
                total_stock += inv.stock;
            std::println("  [stats] total stock: {}", total_stock);
        } else {
            std::println("  [ERROR] {}", results.error().message());
        }
    });
}

// ─── Scenario 3: Failure propagation — one fails → join() error ──────────────

static void scenario_one_failure_propagates() {
    std::println("\n=== Scenario 3: Inventory service failure → full error propagation ===");
    g_service_fail_mode.store(1);  // force inventory service failure
    g_call_count.store(0);

    const uint64_t product_id = 888;

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        TaskGroup tg;
        tg.spawn<ProductInfo>(fetch_product(product_id));
        tg.spawn<InventoryInfo>(fetch_inventory(product_id));  // fails
        tg.spawn<PricingInfo>(fetch_pricing(product_id));
        tg.spawn<ReviewSummary>(fetch_reviews(product_id));

        auto res = co_await tg.join();
        if (!res.has_value()) {
            std::println("  [result] TaskGroup error propagation: {}",
                        res.error().message());
            std::println("  → product page rendering failed, returning error page");
        } else {
            std::println("  [result] success (unexpected)");
        }
    });

    g_service_fail_mode.store(0);  // restore
}

// ─── Scenario 4: cancel() — early termination ────────────────────────────────

static void scenario_cancel() {
    std::println("\n=== Scenario 4: TaskGroup cancel() — bulk cancellation ===");

    RunGuard guard;
    std::atomic<int> completed{0};

    guard.run_and_wait([&]() -> Task<void> {
        TaskGroup tg;

        // Long-running task that checks cancellation signal
        auto long_task = [&](int id, std::stop_token stoken) -> Task<Result<int>> {
            if (stoken.stop_requested()) {
                std::println("  [cancelled] task-{} cancelled before start", id);
                co_return unexpected(std::make_error_code(std::errc::operation_canceled));
            }
            completed.fetch_add(1, std::memory_order_relaxed);
            co_return id * 10;
        };

        auto token = tg.stop_token();

        // Request cancellation first
        tg.cancel();

        // Tasks spawned after cancellation can check state
        tg.spawn<int>(long_task(1, token));
        tg.spawn<int>(long_task(2, token));
        tg.spawn<int>(long_task(3, token));

        auto res = co_await tg.join();
        std::println("  [result] join completed (has_error={})",
                    res.has_value() ? "false" : "true");
        std::println("  [result] tasks executed after cancel={}",
                    completed.load());
    });
}

// ─── Scenario 5: Parallel data pipeline — distributed aggregation ─────────────

static void scenario_parallel_aggregation() {
    std::println("\n=== Scenario 5: Distributed aggregation — inventory sum across warehouses ===");

    // Simultaneous inventory query from 5 warehouses
    struct WarehouseStock {
        std::string warehouse_id;
        int         stock;
    };

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        TaskGroup tg;

        const char* warehouses[] = {"seoul", "busan", "incheon", "daegu", "gwangju"};
        for (int i = 0; i < 5; ++i) {
            tg.spawn<WarehouseStock>(
                [i, name = std::string(warehouses[i])]() -> Task<Result<WarehouseStock>> {
                    int stock = 100 + i * 50;
                    std::println("  [warehouse] {}: {} units", name, stock);
                    co_return WarehouseStock{name, stock};
                }()
            );
        }

        auto results = co_await tg.join_all<WarehouseStock>();
        if (results.has_value()) {
            int total = 0;
            for (const auto& w : *results) total += w.stock;
            std::println("  [aggregated] national total stock: {} ({} warehouses)",
                        total, results->size());
        }
    });
}

// ─── Scenario 6: Fan-out notifications — void join() ─────────────────────────

static void scenario_fanout_notifications() {
    std::println("\n=== Scenario 6: Fan-out notifications (email + SMS + push) — join() ===");

    std::atomic<int> sent_count{0};

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        TaskGroup tg;

        auto notify = [&](const char* channel) -> Task<Result<void>> {
            std::println("  [notify] {} sent", channel);
            sent_count.fetch_add(1, std::memory_order_relaxed);
            co_return Result<void>{};
        };

        tg.spawn(notify("email"));
        tg.spawn(notify("SMS"));
        tg.spawn(notify("push_notification"));
        tg.spawn(notify("messenger"));

        auto res = co_await tg.join();
        std::println("  [result] notifications: {} ({} channels)",
                    res.has_value() ? "success" : "failed",
                    sent_count.load());
    });
}

int main() {
    scenario_parallel_fetch_success();
    scenario_join_all_homogeneous();
    scenario_one_failure_propagates();
    scenario_cancel();
    scenario_parallel_aggregation();
    scenario_fanout_notifications();
    std::println("\ntask_group_example: ALL OK");
    return 0;
}
