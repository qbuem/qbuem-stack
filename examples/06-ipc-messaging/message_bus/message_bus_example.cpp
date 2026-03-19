/**
 * @file examples/message_bus_example.cpp
 * @brief MessageBus example — Pub/Sub, ServerStream, ClientStream
 */
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/context.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;

// ─── Domain events ────────────────────────────────────────────────────────────

struct OrderEvent {
    int    order_id;
    double amount;
    std::string status;
};

struct InventoryUpdate {
    std::string sku;
    int         quantity_delta;
};

// ─── Unary Pub/Sub example ────────────────────────────────────────────────────

Task<Result<void>> pubsub_example(Dispatcher& dispatcher) {
    MessageBus bus;
    bus.start(dispatcher);

    std::atomic<int> received{0};

    // Subscription 1: order processing handler
    auto sub1 = bus.subscribe("orders", [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
        auto& ev = std::any_cast<const OrderEvent&>(msg);
        received.fetch_add(1, std::memory_order_relaxed);
        println("[bus] Sub1: order_id={} amount={} status={}",
                  ev.order_id, ev.amount, ev.status);
        co_return {};
    });

    // Subscription 2: same topic — round-robin distribution
    auto sub2 = bus.subscribe("orders", [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
        auto& ev = std::any_cast<const OrderEvent&>(msg);
        received.fetch_add(1, std::memory_order_relaxed);
        println("[bus] Sub2: order_id={} amount={}", ev.order_id, ev.amount);
        co_return {};
    });

    // Publish
    for (int i = 1; i <= 4; ++i) {
        co_await bus.publish("orders", OrderEvent{i, i * 99.9, "created"});
    }

    // Wait for receipt
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (received.load() < 4 && std::chrono::steady_clock::now() < deadline) {
        co_await std::suspend_never{};
    }

    println("[bus] Received {} events", received.load());

    // Unsubscribe (automatic when sub1, sub2 are destroyed)
    co_return {};
}

// ─── try_publish (non-blocking) example ──────────────────────────────────────

Task<Result<void>> try_publish_example(Dispatcher& dispatcher) {
    MessageBus bus;
    bus.start(dispatcher);

    std::atomic<int> inv_count{0};

    auto inv_sub = bus.subscribe("inventory",
        [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            auto& upd = std::any_cast<const InventoryUpdate&>(msg);
            inv_count.fetch_add(1, std::memory_order_relaxed);
            println("[bus] Inventory: sku={} delta={}", upd.sku, upd.quantity_delta);
            co_return {};
        });

    // Non-blocking publish
    bool ok = bus.try_publish("inventory", InventoryUpdate{"SKU-001", +10});
    println("[bus] try_publish SKU-001: {}", ok);

    ok = bus.try_publish("inventory", InventoryUpdate{"SKU-002", -5});
    println("[bus] try_publish SKU-002: {}", ok);

    // Query subscriber count
    println("[bus] inventory subscribers: {}",
              bus.subscriber_count("inventory"));

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (inv_count.load() < 2 && std::chrono::steady_clock::now() < deadline)
        co_await std::suspend_never{};

    println("[bus] Inventory events received: {}", inv_count.load());
    co_return {};
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    println("=== MessageBus Pub/Sub ===");

    Dispatcher dispatcher(2);
    std::jthread run_th([&] { dispatcher.run(); });

    std::atomic<bool> done1{false}, done2{false};

    dispatcher.spawn([&]() -> Task<Result<void>> {
        co_await pubsub_example(dispatcher);
        done1.store(true);
        co_return {};
    }());

    dispatcher.spawn([&]() -> Task<Result<void>> {
        co_await try_publish_example(dispatcher);
        done2.store(true);
        co_return {};
    }());

    auto deadline = std::chrono::steady_clock::now() + 10s;
    while ((!done1.load() || !done2.load()) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    dispatcher.stop();
    run_th.join();
    return 0;
}
