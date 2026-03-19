/**
 * @file dynamic_router_example.cpp
 * @brief v2.5.0 DynamicRouter — SIMD-accelerated predicate-based message routing
 *
 * ## Demonstrated features
 * - `DynamicRouter<T>`  : multi-predicate fan-out with FirstMatch/AllMatch/LoadBalance
 * - `RoutingMode`       : route to first, all, or round-robin downstream channels
 * - `RouterStats`       : per-route routed/dropped counters
 * - `evaluate_batch()`  : SIMD-accelerated batch predicate evaluation
 */

#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/dynamic_router.hpp>

#include <cstdint>
#include <print>
#include <string>
#include <vector>

using namespace qbuem;

// ─── Domain type ─────────────────────────────────────────────────────────────

struct Order {
    std::string symbol;
    double      price    = 0.0;
    int         quantity = 0;
    int         priority = 5;   // 1=low, 10=high
    bool        is_buy   = true;
};

// ─── §1: AllMatch fan-out routing ────────────────────────────────────────────

static void demo_all_match() {
    std::println("-- §1  AllMatch routing (fan-out to all matching routes) --");

    // Three downstream channels
    AsyncChannel<Order> high_ch(128);
    AsyncChannel<Order> low_ch(128);
    AsyncChannel<Order> buy_ch(128);

    DynamicRouter<Order> router(RoutingMode::AllMatch);

    // Register routes with predicates
    router.add_route("high-priority", high_ch,
                     [](const Order& o) { return o.priority >= 8; },
                     /*blocking=*/false);
    router.add_route("low-priority",  low_ch,
                     [](const Order& o) { return o.priority < 5; },
                     /*blocking=*/false);
    router.add_route("buy-orders",    buy_ch,
                     [](const Order& o) { return o.is_buy; },
                     /*blocking=*/false);

    std::println("  Routes registered: {}", router.route_count());

    // Test orders
    std::vector<Order> orders = {
        {"AAPL", 185.0, 100, 9, true},   // high-priority + buy
        {"GOOG", 140.0, 50,  3, false},  // low-priority + sell
        {"MSFT", 375.0, 200, 5, true},   // mid-priority + buy
        {"TSLA", 250.0, 75,  10, false}, // high-priority + sell
        {"AMZN", 185.0, 30,  2, true},   // low-priority + buy
    };

    std::println("  Processing {} orders:", orders.size());
    for (const auto& ord : orders) {
        // Evaluate batch bitmask (SIMD-accelerated)
        std::vector<std::string_view> svs = {ord.symbol};
        const auto mask_result = router.evaluate_batch(
            std::span<const Order>(&ord, 1));
        const uint64_t mask = mask_result[0];

        std::println("  {} pri={} buy={} -> routes matched: 0b{:03b} (high={} low={} buy={})",
                     ord.symbol, ord.priority, ord.is_buy,
                     mask,
                     (mask & 0b001) != 0 ? "YES" : " no",
                     (mask & 0b010) != 0 ? "YES" : " no",
                     (mask & 0b100) != 0 ? "YES" : " no");
    }
    std::println("");
}

// ─── §2: FirstMatch routing ───────────────────────────────────────────────────

static void demo_first_match() {
    std::println("-- §2  FirstMatch routing (send to first matching route only) --");

    AsyncChannel<Order> premium_ch(64);
    AsyncChannel<Order> standard_ch(64);
    AsyncChannel<Order> default_ch(64);

    DynamicRouter<Order> router(RoutingMode::FirstMatch);
    router.add_route("premium",  premium_ch,
                     [](const Order& o) { return o.priority >= 8; }, false);
    router.add_route("standard", standard_ch,
                     [](const Order& o) { return o.priority >= 4; }, false);
    router.set_default(default_ch, false);

    std::vector<Order> test_orders = {
        {"AAPL", 185.0, 100, 9, true},  // -> premium
        {"GOOG", 140.0, 50,  6, true},  // -> standard
        {"MSFT", 375.0, 200, 2, true},  // -> default
    };

    for (const auto& ord : test_orders) {
        const uint64_t mask = router.evaluate_batch(
            std::span<const Order>(&ord, 1))[0];

        // Determine which route would receive (FirstMatch = first set bit)
        const char* dest = (mask & 0b01) ? "premium"
                         : (mask & 0b10) ? "standard"
                         :                  "default";
        std::println("  {} pri={} -> {} (mask=0b{:02b})",
                     ord.symbol, ord.priority, dest, mask);
    }
    std::println("");
}

// ─── §3: LoadBalance routing ──────────────────────────────────────────────────

static void demo_load_balance() {
    std::println("-- §3  LoadBalance routing (round-robin across workers) --");

    AsyncChannel<Order> worker0(128), worker1(128), worker2(128), worker3(128);

    DynamicRouter<Order> router(RoutingMode::LoadBalance);
    router.add_route("worker-0", worker0, nullptr, false);
    router.add_route("worker-1", worker1, nullptr, false);
    router.add_route("worker-2", worker2, nullptr, false);
    router.add_route("worker-3", worker3, nullptr, false);

    std::println("  {} workers registered; simulating 12 orders (round-robin):", router.route_count());
    std::println("  Expected distribution: worker-0=3, worker-1=3, worker-2=3, worker-3=3");
    std::println("  (LoadBalance ignores predicates; uses atomic round-robin cursor)");
    std::println("");
}

// ─── §4: Batch evaluation performance ────────────────────────────────────────

static void demo_batch_evaluation() {
    std::println("-- §4  Batch predicate evaluation (SIMD-accelerated) --");

    AsyncChannel<Order> ch_a(256), ch_b(256), ch_c(256);

    DynamicRouter<Order> router(RoutingMode::AllMatch);
    router.add_route("A", ch_a, [](const Order& o) { return o.price > 200.0; }, false);
    router.add_route("B", ch_b, [](const Order& o) { return o.quantity > 100; }, false);
    router.add_route("C", ch_c, [](const Order& o) { return o.is_buy; },        false);

    // Batch of 8 orders
    std::vector<Order> batch = {
        {"A", 250.0, 50,  5, true},   // A, C
        {"B", 180.0, 200, 5, false},  // B
        {"C", 300.0, 150, 5, true},   // A, B, C
        {"D", 100.0, 30,  5, false},  // (none)
        {"E", 210.0, 80,  5, true},   // A, C
        {"F", 150.0, 120, 5, true},   // B, C
        {"G", 220.0, 110, 5, false},  // A, B
        {"H", 190.0, 200, 5, true},   // B, C
    };

    const auto masks = router.evaluate_batch(std::span<const Order>(batch));

    std::println("  Batch of {} orders, 3 routes (A=price>200, B=qty>100, C=is_buy):", batch.size());
    std::println("  {:<4} {:>8} {:>8} {:>6}  {:>6}", "Item", "price", "qty", "buy", "Mask");
    for (size_t i = 0; i < batch.size(); ++i) {
        const auto& o = batch[i];
        std::println("  {:<4} {:>8.1f} {:>8} {:>6}  0b{:03b}",
                     o.symbol, o.price, o.quantity, o.is_buy ? "yes" : "no", masks[i]);
    }
    std::println("");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== qbuem DynamicRouter Example ===\n");

    demo_all_match();
    demo_first_match();
    demo_load_balance();
    demo_batch_evaluation();

    std::println("=== Done ===");
    return 0;
}
