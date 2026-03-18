/**
 * @file examples/timer_wheel_example.cpp
 * @brief TimerWheel example — schedule / cancel / tick (O(1))
 */
#include <qbuem/core/timer_wheel.hpp>
#include <qbuem/compat/print.hpp>

#include <atomic>
#include <string>
#include <vector>

using namespace qbuem;

int main() {
    std::println("=== TimerWheel example ===");

    TimerWheel wheel;

    // ── Basic timer scheduling ────────────────────────────────────────────────
    std::vector<std::string> fired;

    auto id1 = wheel.schedule(100, [&fired] {
        fired.push_back("T1: 100ms");
        std::println("[timer] T1 fired at 100ms");
    });

    auto id2 = wheel.schedule(50, [&fired] {
        fired.push_back("T2: 50ms");
        std::println("[timer] T2 fired at 50ms");
    });

    auto id3 = wheel.schedule(200, [&fired] {
        fired.push_back("T3: 200ms");
        std::println("[timer] T3 fired at 200ms");
    });

    auto id_cancel = wheel.schedule(150, [&fired] {
        fired.push_back("TC: 150ms (should not fire)");
        std::println("[timer] TC fired (ERROR: should have been cancelled)");
    });

    // ── Timer cancellation ────────────────────────────────────────────────────
    bool cancelled = wheel.cancel(id_cancel);
    std::println("[timer] cancel(id_cancel)={}", cancelled);

    // ── Tick simulation ───────────────────────────────────────────────────────
    // 50ms elapsed
    size_t f1 = wheel.tick(50);
    std::println("[timer] After 50ms tick: {} callbacks fired", f1);

    // another 50ms elapsed (total=100ms)
    size_t f2 = wheel.tick(50);
    std::println("[timer] After 100ms tick: {} callbacks fired", f2);

    // another 100ms elapsed (total=200ms)
    size_t f3 = wheel.tick(100);
    std::println("[timer] After 200ms tick: {} callbacks fired", f3);

    // Cancelled timer must not fire
    wheel.tick(50); // 250ms — cancelled TC does not fire

    std::println("\n[timer] Fired timers:");
    for (const auto& s : fired)
        std::println("  - {}", s);

    // ── next_expiry_ms ────────────────────────────────────────────────────────
    TimerWheel wheel2;
    auto next_id = wheel2.schedule(300, [] {});
    uint64_t next = wheel2.next_expiry_ms();
    std::println("\n[timer] next_expiry_ms={} (should be ~300)", next);

    // ── Immediate execution (delay_ms=0) ──────────────────────────────────────
    int immediate_count = 0;
    wheel2.schedule(0, [&] { ++immediate_count; });
    wheel2.tick(0);
    std::println("[timer] Immediate (delay=0) fired: {}", immediate_count);

    // ── Many timers (O(1) schedule verification) ──────────────────────────────
    TimerWheel stress_wheel;
    std::atomic<size_t> stress_count{0};
    for (size_t i = 1; i <= 100; ++i) {
        stress_wheel.schedule(i, [&stress_count] {
            stress_count.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (uint64_t ms = 1; ms <= 100; ++ms)
        stress_wheel.tick(1);

    std::println("[timer] Stress: {}/100 timers fired", stress_count.load());

    return 0;
}
