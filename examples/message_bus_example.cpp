/**
 * @file examples/message_bus_example.cpp
 * @brief MessageBus 예시 — Pub/Sub, ServerStream, ClientStream
 */
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/context.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 도메인 이벤트 ──────────────────────────────────────────────────────────

struct OrderEvent {
    int    order_id;
    double amount;
    std::string status;
};

struct InventoryUpdate {
    std::string sku;
    int         quantity_delta;
};

// ─── Unary Pub/Sub 예시 ─────────────────────────────────────────────────────

Task<Result<void>> pubsub_example(Dispatcher& dispatcher) {
    MessageBus bus;
    bus.start(dispatcher);

    std::atomic<int> received{0};

    // 구독 1: 주문 처리 핸들러
    auto sub1 = bus.subscribe("orders", [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
        auto& ev = std::any_cast<const OrderEvent&>(msg);
        received.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[bus] Sub1: order_id=" << ev.order_id
                  << " amount=" << ev.amount
                  << " status=" << ev.status << "\n";
        co_return {};
    });

    // 구독 2: 동일 토픽 — 라운드로빈 분산
    auto sub2 = bus.subscribe("orders", [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
        auto& ev = std::any_cast<const OrderEvent&>(msg);
        received.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[bus] Sub2: order_id=" << ev.order_id
                  << " amount=" << ev.amount << "\n";
        co_return {};
    });

    // 발행
    for (int i = 1; i <= 4; ++i) {
        co_await bus.publish("orders", OrderEvent{i, i * 99.9, "created"});
    }

    // 수신 대기
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (received.load() < 4 && std::chrono::steady_clock::now() < deadline) {
        co_await std::suspend_never{};
    }

    std::cout << "[bus] Received " << received.load() << " events\n";

    // 구독 해제 (sub1, sub2 소멸 시 자동)
    co_return {};
}

// ─── try_publish (논블로킹) 예시 ────────────────────────────────────────────

Task<Result<void>> try_publish_example(Dispatcher& dispatcher) {
    MessageBus bus;
    bus.start(dispatcher);

    std::atomic<int> inv_count{0};

    auto inv_sub = bus.subscribe("inventory",
        [&](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            auto& upd = std::any_cast<const InventoryUpdate&>(msg);
            inv_count.fetch_add(1, std::memory_order_relaxed);
            std::cout << "[bus] Inventory: sku=" << upd.sku
                      << " delta=" << upd.quantity_delta << "\n";
            co_return {};
        });

    // 논블로킹 발행
    bool ok = bus.try_publish("inventory", InventoryUpdate{"SKU-001", +10});
    std::cout << "[bus] try_publish SKU-001: " << ok << "\n";

    ok = bus.try_publish("inventory", InventoryUpdate{"SKU-002", -5});
    std::cout << "[bus] try_publish SKU-002: " << ok << "\n";

    // 구독자 수 조회
    std::cout << "[bus] inventory subscribers: "
              << bus.subscriber_count("inventory") << "\n";

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (inv_count.load() < 2 && std::chrono::steady_clock::now() < deadline)
        co_await std::suspend_never{};

    std::cout << "[bus] Inventory events received: " << inv_count.load() << "\n";
    co_return {};
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== MessageBus Pub/Sub ===\n";

    Dispatcher dispatcher(2);
    std::thread run_th([&] { dispatcher.run(); });

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
