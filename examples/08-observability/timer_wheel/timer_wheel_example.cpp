/**
 * @file examples/timer_wheel_example.cpp
 * @brief TimerWheel 예시 — schedule / cancel / tick (O(1))
 */
#include <qbuem/core/timer_wheel.hpp>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace qbuem;

int main() {
    std::cout << "=== TimerWheel 예시 ===\n";

    TimerWheel wheel;

    // ── 기본 타이머 스케줄 ────────────────────────────────────────────────
    std::vector<std::string> fired;

    auto id1 = wheel.schedule(100, [&fired] {
        fired.push_back("T1: 100ms");
        std::cout << "[timer] T1 fired at 100ms\n";
    });

    auto id2 = wheel.schedule(50, [&fired] {
        fired.push_back("T2: 50ms");
        std::cout << "[timer] T2 fired at 50ms\n";
    });

    auto id3 = wheel.schedule(200, [&fired] {
        fired.push_back("T3: 200ms");
        std::cout << "[timer] T3 fired at 200ms\n";
    });

    auto id_cancel = wheel.schedule(150, [&fired] {
        fired.push_back("TC: 150ms (should not fire)");
        std::cout << "[timer] TC fired (ERROR: should have been cancelled)\n";
    });

    // ── 타이머 취소 ────────────────────────────────────────────────────────
    bool cancelled = wheel.cancel(id_cancel);
    std::cout << "[timer] cancel(id_cancel)=" << cancelled << "\n";

    // ── tick 시뮬레이션 ────────────────────────────────────────────────────
    // 50ms 경과
    size_t f1 = wheel.tick(50);
    std::cout << "[timer] After 50ms tick: " << f1 << " callbacks fired\n";

    // 다시 50ms 경과 (total=100ms)
    size_t f2 = wheel.tick(50);
    std::cout << "[timer] After 100ms tick: " << f2 << " callbacks fired\n";

    // 다시 100ms 경과 (total=200ms)
    size_t f3 = wheel.tick(100);
    std::cout << "[timer] After 200ms tick: " << f3 << " callbacks fired\n";

    // 취소된 타이머는 실행되지 않아야 함
    wheel.tick(50); // 250ms — 취소된 TC는 실행 안 됨

    std::cout << "\n[timer] Fired timers:\n";
    for (const auto& s : fired)
        std::cout << "  - " << s << "\n";

    // ── next_expiry_ms ─────────────────────────────────────────────────────
    TimerWheel wheel2;
    auto next_id = wheel2.schedule(300, [] {});
    uint64_t next = wheel2.next_expiry_ms();
    std::cout << "\n[timer] next_expiry_ms=" << next << " (should be ~300)\n";

    // ── 즉시 실행 (delay_ms=0) ─────────────────────────────────────────────
    int immediate_count = 0;
    wheel2.schedule(0, [&] { ++immediate_count; });
    wheel2.tick(0);
    std::cout << "[timer] Immediate (delay=0) fired: " << immediate_count << "\n";

    // ── 많은 타이머 (O(1) 스케줄 검증) ────────────────────────────────────
    TimerWheel stress_wheel;
    std::atomic<size_t> stress_count{0};
    for (size_t i = 1; i <= 100; ++i) {
        stress_wheel.schedule(i, [&stress_count] {
            stress_count.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (uint64_t ms = 1; ms <= 100; ++ms)
        stress_wheel.tick(1);

    std::cout << "[timer] Stress: " << stress_count.load() << "/100 timers fired\n";

    return 0;
}
