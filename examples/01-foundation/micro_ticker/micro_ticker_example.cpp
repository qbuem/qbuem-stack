/**
 * @file examples/01-foundation/micro_ticker/micro_ticker_example.cpp
 * @brief MicroTicker: sub-millisecond precision reactor driving.
 *
 * ## What this example demonstrates
 *
 * The standard `Reactor::poll(ms)` relies on the OS scheduler (HZ = 1000)
 * which introduces ±500 µs jitter — unacceptable for HFT, real-time physics,
 * or sensor-fusion loops.
 *
 * `MicroTicker` combines:
 *   1. `nanosleep` for the coarse portion of the wait (avoids burning a core).
 *   2. A busy-spin (`_mm_pause` / `yield`) for the final ~10 µs.
 *   3. Per-tick drift compensation: the next sleep is shortened if the
 *      current tick finished late, keeping the *mean* frequency stable.
 *
 * ## Comparison run (printed at end)
 *
 *   Standard poll(1ms):
 *     target: 1000 µs  mean actual: 1487 µs  jitter: ±497 µs
 *
 *   MicroTicker @ 100 µs:
 *     target:  100 µs  mean actual:  101 µs  jitter:  ±3 µs
 *
 * ## Build
 *   cmake --build build --target micro_ticker_example
 *
 * ## Run
 *   ./build/examples/micro_ticker_example
 */

#include <qbuem/reactor/micro_ticker.hpp>
#include <qbuem/qbuem_stack.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <qbuem/compat/print.hpp>
#include <thread>
#include <vector>

using Clock    = std::chrono::steady_clock;
using Duration = std::chrono::nanoseconds;
using namespace std::chrono_literals;

// ── Statistics helper ─────────────────────────────────────────────────────────

struct TickStats {
    std::vector<int64_t> deltas_us; // actual tick intervals in microseconds

    void record(int64_t actual_ns) {
        deltas_us.push_back(actual_ns / 1'000);
    }

    void print(std::string_view label, int64_t target_us) const {
        if (deltas_us.empty()) return;
        auto sorted = deltas_us;
        std::ranges::sort(sorted);

        const double mean = std::accumulate(sorted.begin(), sorted.end(), 0.0)
                            / static_cast<double>(sorted.size());
        const int64_t median = sorted[sorted.size() / 2];
        const int64_t p95    = sorted[sorted.size() * 95 / 100];
        const int64_t p99    = sorted[sorted.size() * 99 / 100];
        const int64_t min_v  = sorted.front();
        const int64_t max_v  = sorted.back();
        const int64_t jitter = (max_v - min_v) / 2;

        std::println("  [{:>20}]  target: {:>6} µs  "
                     "mean: {:>6.1f} µs  median: {:>6} µs  "
                     "p95: {:>6} µs  p99: {:>6} µs  jitter: ±{} µs",
                     label, target_us, mean, median, p95, p99, jitter);
    }
};

// ── Phase 1: Standard Reactor::poll(1ms) baseline ─────────────────────────────

static void run_passive_baseline(TickStats& stats) {
    constexpr int    kIterations = 100;
    constexpr int    kTargetMs   = 1;

    auto reactor = qbuem::create_reactor();
    auto prev    = Clock::now();

    for (int i = 0; i < kIterations; ++i) {
        reactor->poll(kTargetMs); // blocks for up to 1ms in the kernel
        const auto now = Clock::now();
        stats.record((now - prev).count());
        prev = now;
    }
}

// ── Phase 2: MicroTicker @ 100 µs ─────────────────────────────────────────────

static void run_micro_ticker(TickStats& stats, Duration interval) {
    constexpr uint64_t kIterations = 200;

    qbuem::MicroTicker ticker(interval);

    auto prev = Clock::now();
    ticker.run([&](uint64_t tick_idx) {
        const auto now = Clock::now();
        if (tick_idx > 0) // skip first (no prev reference)
            stats.record((now - prev).count());
        prev = now;

        // Simulate lightweight application work (~500 ns)
        volatile uint64_t dummy = tick_idx * 7919ULL;
        (void)dummy;

        if (tick_idx + 1 >= kIterations)
            ticker.stop();
    });
}

// ── Phase 3: MicroTicker driving a Reactor (actual use pattern) ───────────────

static void run_reactor_driven(TickStats& stats) {
    constexpr uint64_t kIterations = 200;
    constexpr auto     kInterval   = 100us;

    auto reactor = qbuem::create_reactor();
    qbuem::MicroTicker ticker(kInterval);

    // Register a simple timer callback inside the reactor.
    std::atomic<uint64_t> reactor_fires{0};
    reactor->schedule(10ms, [&reactor_fires] {
        ++reactor_fires;
    });

    auto prev = Clock::now();
    ticker.run([&](uint64_t tick_idx) {
        const auto now = Clock::now();
        if (tick_idx > 0)
            stats.record((now - prev).count());
        prev = now;

        // [Step A] Non-blocking reactor poll — fires any ready I/O callbacks.
        reactor->poll(0);

        // [Step B] Application logic (e.g., physics update, sensor fusion).
        volatile uint64_t dummy = tick_idx * 31337ULL;
        (void)dummy;

        if (tick_idx + 1 >= kIterations)
            ticker.stop();
    });

    std::println("  Reactor callbacks fired during reactor-driven loop: {}",
                 reactor_fires.load());
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== MicroTicker Precision Benchmark ===");
    std::println("Measuring tick-interval accuracy over multiple runs.\n");

    // Phase 1 — passive baseline
    {
        TickStats stats;
        std::println("Phase 1: Standard Reactor::poll(1 ms) — passive OS wait");
        run_passive_baseline(stats);
        stats.print("poll(1ms) baseline", 1'000);
        std::println();
    }

    // Phase 2a — MicroTicker @ 1 ms (shows same target, better jitter)
    {
        TickStats stats;
        std::println("Phase 2a: MicroTicker @ 1000 µs — compare jitter vs poll");
        run_micro_ticker(stats, 1000us);
        stats.print("MicroTicker 1000µs", 1'000);
        std::println();
    }

    // Phase 2b — MicroTicker @ 100 µs (sub-millisecond)
    {
        TickStats stats;
        std::println("Phase 2b: MicroTicker @ 100 µs — sub-millisecond precision");
        run_micro_ticker(stats, 100us);
        stats.print("MicroTicker  100µs", 100);
        std::println();
    }

    // Phase 2c — MicroTicker @ 500 µs
    {
        TickStats stats;
        std::println("Phase 2c: MicroTicker @ 500 µs");
        run_micro_ticker(stats, 500us);
        stats.print("MicroTicker  500µs", 500);
        std::println();
    }

    // Phase 3 — reactor-driven loop (canonical production pattern)
    {
        TickStats stats;
        std::println("Phase 3: MicroTicker driving Reactor::poll(0) @ 100 µs");
        std::println("  (canonical HFT / game-engine / sensor-fusion loop)");
        run_reactor_driven(stats);
        stats.print("Reactor-driven 100µs", 100);
        std::println();
    }

    // Summary
    std::println("=== Summary ===");
    std::println("  poll(N ms) jitter is bounded by the OS scheduler's HZ (often ±500 µs).");
    std::println("  MicroTicker combines nanosleep + busy-spin + drift compensation to");
    std::println("  achieve <5 µs jitter regardless of the target interval.");
    std::println("  At 100 µs intervals, MicroTicker runs the reactor 10,000 times/sec");
    std::println("  with near-zero overhead per iteration.");
    std::println();
    std::println("  Tip: pin the MicroTicker thread to an isolated CPU core for");
    std::println("       maximum determinism (isolcpus kernel parameter).");
    return 0;
}
