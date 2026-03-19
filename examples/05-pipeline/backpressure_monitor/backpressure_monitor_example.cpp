/**
 * @file backpressure_monitor_example.cpp
 * @brief v2.5.0 BackpressureMonitor — real-time pipeline stage pressure telemetry
 *
 * ## Demonstrated features
 * - `BackpressureMonitor`   : per-stage registration and lock-free hot-path recording
 * - `StageMetrics`          : enqueue/dequeue counters, latency histogram, fill ratio
 * - `StagePressure`         : immutable snapshot with P50/P99/P99.9 latency
 * - `BackpressureAlert`     : threshold-based alerting on saturation or latency breach
 */

#include <qbuem/pipeline/backpressure_monitor.hpp>

#include <chrono>
#include <qbuem/compat/print.hpp>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Simulate pipeline stage work ───────────────────────────────────────────

static void simulate_stage(BackpressureMonitor& monitor,
                            std::string_view stage_name,
                            size_t n_items,
                            uint64_t min_latency_ns,
                            uint64_t max_latency_ns)
{
    auto& m = monitor.stage(stage_name);
    std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> lat_dist{min_latency_ns, max_latency_ns};

    for (size_t i = 0; i < n_items; ++i) {
        m.record_enqueue();

        // Simulate processing latency
        const uint64_t latency = lat_dist(rng);
        // (In a real pipeline this would be actual async work; here we just record)
        m.record_dequeue(latency, 512 /*bytes*/);
    }
}

// ─── Print snapshot table ────────────────────────────────────────────────────

static void print_snapshot(const BackpressureMonitor& monitor) {
    std::println("  {:<20} {:>8} {:>8} {:>10} {:>10} {:>10} {:>8}",
                 "Stage", "Depth", "Enqueue", "P50(µs)", "P99(µs)", "P999(µs)", "Errors");
    std::println("  {}", std::string(78, '-'));

    for (const auto& snap : monitor.all_snapshots()) {
        std::println("  {:<20} {:>8} {:>8} {:>10.1f} {:>10.1f} {:>10.1f} {:>8}",
                     snap.name,
                     snap.queue_depth,
                     snap.enqueue_total,
                     static_cast<double>(snap.latency_p50_ns)  / 1000.0,
                     static_cast<double>(snap.latency_p99_ns)  / 1000.0,
                     static_cast<double>(snap.latency_p999_ns) / 1000.0,
                     snap.error_total);
    }
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== qbuem BackpressureMonitor Example ===\n");

    BackpressureMonitor monitor;

    // Register pipeline stages (call before pipeline starts)
    monitor.register_stage("parse",    256);
    monitor.register_stage("validate", 256);
    monitor.register_stage("enrich",   512);
    monitor.register_stage("risk",     128);

    // §1 — Simulate normal load across all stages
    std::println("-- §1  Normal load simulation (10k items/stage) --");
    simulate_stage(monitor, "parse",    10'000, 1'000,    5'000);   // 1–5 µs
    simulate_stage(monitor, "validate", 10'000, 500,      2'000);   // 0.5–2 µs
    simulate_stage(monitor, "enrich",   10'000, 10'000,   50'000);  // 10–50 µs
    simulate_stage(monitor, "risk",     10'000, 50'000,   200'000); // 50–200 µs

    print_snapshot(monitor);
    std::println("");

    // §2 — Manually inject an error on the risk stage
    std::println("-- §2  Inject errors into 'risk' stage --");
    {
        auto& risk = monitor.stage("risk");
        for (int i = 0; i < 42; ++i) {
            risk.record_enqueue();
            risk.record_error(); // item dropped (e.g., circuit breaker tripped)
        }
    }
    {
        auto snap = monitor.snapshot("risk");
        std::println("  risk: errors={} fill_ratio={:.1f}% saturated={}",
                     snap.error_total,
                     snap.fill_ratio * 100.0,
                     snap.is_saturated ? "YES" : "no");
    }
    std::println("");

    // §3 — Alert threshold check
    std::println("-- §3  Alert threshold check --");
    BackpressureAlert alert{
        .saturation_threshold = 0.70,
        .latency_p99_limit_ns = 100'000, // 100 µs
        .on_alert = [](const StagePressure& sp) {
            std::println("  [ALERT] stage={} fill={:.0f}% p99={:.0f}µs errors={}",
                         sp.name,
                         sp.fill_ratio * 100.0,
                         static_cast<double>(sp.latency_p99_ns) / 1000.0,
                         sp.error_total);
        }
    };
    monitor.check_alerts(alert);
    std::println("");

    // §4 — High-pressure scenario: saturate the parse stage
    std::println("-- §4  High-pressure scenario (parse stage: 5000 queued, 100 processed) --");
    {
        auto& parse = monitor.stage("parse");
        // Simulate 5000 items enqueued but only 100 dequeued
        for (int i = 0; i < 5000; ++i) parse.record_enqueue();
        for (int i = 0; i < 100;  ++i) parse.record_dequeue(3'000 /*ns*/);

        auto snap = monitor.snapshot("parse");
        std::println("  parse: depth={} fill_ratio={:.1f}% saturated={}",
                     snap.queue_depth,
                     snap.fill_ratio * 100.0,
                     snap.is_saturated ? "YES" : "no");
    }
    std::println("");

    // §5 — Full snapshot after all scenarios
    std::println("-- §5  Final snapshot --");
    print_snapshot(monitor);

    // §6 — Reset and verify
    std::println("\n-- §6  Reset all counters --");
    monitor.reset();
    {
        auto snap = monitor.snapshot("parse");
        std::println("  parse after reset: depth={} enqueue={} errors={}",
                     snap.queue_depth, snap.enqueue_total, snap.error_total);
    }

    std::println("\n=== Done ===");
    return 0;
}
