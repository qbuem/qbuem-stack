/**
 * @file pipeline_observer_health_example.cpp
 * @brief Pipeline observability + health monitoring example.
 *
 * ## Coverage — pipeline/observability.hpp
 * - ActionMetrics              — per-action processing metrics (items, errors, latency buckets)
 * - ActionMetrics::record_latency_us() — record latency bucket in µs
 * - PipelineMetrics            — aggregated pipeline metrics
 * - PipelineMetrics::total_processed() / total_errors() / error_rate()
 * - PipelineObserver           — event hook interface
 * - LoggingObserver            — default stderr logging implementation
 * - NoopObserver               — zero-overhead inactive implementation
 * - HistogramMetrics           — user-defined bucket latency histogram
 *
 * ## Coverage — pipeline/health.hpp
 * - PipelineVersion            — semantic versioning + compatible_with()
 * - PipelineVersionRegistry    — pipeline version registry
 * - ActionHealth               — per-action detailed health state (to_json)
 * - PipelineHealth             — pipeline-level health aggregation (recompute, to_json)
 * - HealthStatus               — HEALTHY / DEGRADED / UNHEALTHY
 * - HealthRegistry             — global health state registry
 * - GraphTopologyExporter      — export PipelineGraph topology as JSON
 *
 * ## Coverage — pipeline/slo.hpp
 * - LatencyHistogram           — rolling 1024-sample p99/p99.9 histogram
 * - LatencyHistogram::record() — record a latency sample
 * - LatencyHistogram::p99() / p999() / bucket_counts()
 * - ErrorBudgetTracker         — latency + error rate SLO tracker
 * - ErrorBudgetTracker::record_success() / record_error()
 * - ErrorBudgetTracker::error_rate() / budget_exhausted() / check_slo()
 */

#include <mutex>   // health.hpp uses std::unique_lock — include before health.hpp

#include <qbuem/pipeline/health.hpp>
#include <qbuem/pipeline/observability.hpp>
#include <qbuem/pipeline/slo.hpp>

#include <chrono>
#include <string>
#include <system_error>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;

// ─────────────────────────────────────────────────────────────────────────────
// §1  ActionMetrics & PipelineMetrics
// ─────────────────────────────────────────────────────────────────────────────

static void demo_action_metrics() {
    println("── §1  ActionMetrics & PipelineMetrics ──");

    ActionMetrics m;

    // Record item processing (atomic counters)
    m.items_processed.fetch_add(100, std::memory_order_relaxed);
    m.errors.fetch_add(3, std::memory_order_relaxed);
    m.retried.fetch_add(5, std::memory_order_relaxed);
    m.dlq_count.fetch_add(1, std::memory_order_relaxed);

    // Record latency buckets (in µs)
    m.record_latency_us(500);    // < 1ms → lat_buckets[0]
    m.record_latency_us(5000);   // < 10ms → lat_buckets[1]
    m.record_latency_us(50000);  // < 100ms → lat_buckets[2]
    m.record_latency_us(200000); // >= 100ms → lat_buckets[3]

    println("  processed={} errors={} retried={} dlq={}",
                static_cast<unsigned long long>(m.items_processed.load()),
                static_cast<unsigned long long>(m.errors.load()),
                static_cast<unsigned long long>(m.retried.load()),
                static_cast<unsigned long long>(m.dlq_count.load()));
    println("  latency buckets: [0]={} [1]={} [2]={} [3]={}",
                static_cast<unsigned long long>(m.lat_buckets[0].load()),
                static_cast<unsigned long long>(m.lat_buckets[1].load()),
                static_cast<unsigned long long>(m.lat_buckets[2].load()),
                static_cast<unsigned long long>(m.lat_buckets[3].load()));

    // HistogramMetrics — user-defined bucket histogram
    auto hist = std::make_shared<HistogramMetrics>(
        std::initializer_list<uint64_t>{1000, 5000, 10000, 50000});
    m.histogram = hist;

    m.record_latency_us(800);    // < 1000 → bucket 0
    m.record_latency_us(3000);   // < 5000 → bucket 1
    auto counts = hist->bucket_counts();
    println("  HistogramMetrics bucket count: {}", counts.size());

    // m.reset()
    m.reset();
    println("  after reset() processed={}\n",
                static_cast<unsigned long long>(m.items_processed.load()));

    // PipelineMetrics — ActionMetrics cannot be moved due to atomic fields
    // actions vector is not directly populated here; verify interface methods only
    PipelineMetrics pm;
    pm.name = "order-pipeline";
    // Verify aggregate methods on empty state
    println("  PipelineMetrics '{}': total_processed={} error_rate={:.4f}\n",
                pm.name,
                static_cast<unsigned long long>(pm.total_processed()),
                pm.error_rate());
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  PipelineObserver — event hooks
// ─────────────────────────────────────────────────────────────────────────────

static void demo_observer() {
    println("── §2  PipelineObserver ──");

    // LoggingObserver — default logging implementation
    LoggingObserver logging_obs;
    logging_obs.on_item_done("validate", 42, 1200);
    logging_obs.on_error("enrich",
        std::make_error_code(std::errc::connection_refused));
    logging_obs.on_scale_event("publish", 2, 4);
    logging_obs.on_state_change("order-pipeline", "idle", "running");
    logging_obs.on_circuit_open("payment");
    logging_obs.on_circuit_close("payment");

    // NoopObserver — zero-overhead inactive implementation
    NoopObserver noop;
    noop.on_item_start("validate", 1);
    noop.on_dlq_item("enrich", {});
    println("  LoggingObserver & NoopObserver done\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  PipelineVersion & PipelineVersionRegistry
// ─────────────────────────────────────────────────────────────────────────────

static void demo_version() {
    println("── §3  PipelineVersion & PipelineVersionRegistry ──");

    PipelineVersion v1{1, 0, 0};
    PipelineVersion v2{1, 2, 3};
    PipelineVersion v3{2, 0, 0};

    println("  v1={} v2={} v3={}",
                v1.to_string(), v2.to_string(), v3.to_string());
    println("  v1.compatible_with(v2)={} (same major)",
                v1.compatible_with(v2) ? "true" : "false");
    println("  v1.compatible_with(v3)={} (different major)",
                v1.compatible_with(v3) ? "true" : "false");

    // PipelineVersionRegistry
    auto& vreg = PipelineVersionRegistry::global();
    vreg.set_version("order-pipeline", v1);
    vreg.set_version("payment-pipeline", v3);

    auto stored = vreg.get("order-pipeline");
    if (stored)
        println("  registered version: {}", stored->to_string());

    bool compat = vreg.compatible_with("order-pipeline", PipelineVersion{1, 5, 0});
    println("  compatible_with(1.5.0): {}\n", compat ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  PipelineHealth & HealthRegistry
// ─────────────────────────────────────────────────────────────────────────────

static void demo_health() {
    println("── §4  PipelineHealth & HealthRegistry ──");

    // Construct ActionHealth directly
    ActionHealth ah_validate;
    ah_validate.name             = "validate";
    ah_validate.circuit_state    = "CLOSED";
    ah_validate.error_rate_1m    = 0.001;
    ah_validate.p99_us           = 800;
    ah_validate.queue_depth      = 0;
    ah_validate.items_processed  = 1000;

    ActionHealth ah_publish;
    ah_publish.name             = "publish";
    ah_publish.circuit_state    = "OPEN";   // circuit open → DEGRADED
    ah_publish.error_rate_1m    = 0.15;
    ah_publish.p99_us           = 150000;
    ah_publish.queue_depth      = 42;
    ah_publish.items_processed  = 850;

    println("  ActionHealth JSON: {}", ah_validate.to_json());

    // Build PipelineHealth
    PipelineHealth health;
    health.name    = "order-pipeline";
    health.actions = {ah_validate, ah_publish};
    health.recompute();  // auto-compute status

    println("  overall status: {}",
                std::string(to_string(health.status)));

    auto json = health.to_json();
    println("  JSON (first 80 chars): {:.80}", json);
    if (json.size() > 80) println("  ...");

    // HealthRegistry
    auto& hreg = HealthRegistry::global();
    hreg.update(health);

    auto retrieved = hreg.get("order-pipeline");
    if (retrieved)
        println("  HealthRegistry lookup: status={}",
                    std::string(to_string(retrieved->status)));

    println("  all_json length: {}\n", hreg.all_json().size());
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  LatencyHistogram & ErrorBudgetTracker
// ─────────────────────────────────────────────────────────────────────────────

static void demo_slo() {
    println("── §5  LatencyHistogram & ErrorBudgetTracker ──");

    // LatencyHistogram — rolling 1024 samples
    LatencyHistogram hist;
    for (int i = 0; i < 900; ++i) hist.record(500us);    // 0.5ms
    for (int i = 0; i < 80;  ++i) hist.record(5000us);   // 5ms
    for (int i = 0; i < 15;  ++i) hist.record(50000us);  // 50ms
    for (int i = 0; i < 5;   ++i) hist.record(200000us); // 200ms

    println("  p99={}µs p99.9={}µs",
                static_cast<long long>(hist.p99().count()),
                static_cast<long long>(hist.p999().count()));

    auto buckets = hist.bucket_counts();
    println("  buckets [<1ms={} <10ms={} <100ms={} >=100ms={}]",
                static_cast<unsigned long long>(buckets[0]),
                static_cast<unsigned long long>(buckets[1]),
                static_cast<unsigned long long>(buckets[2]),
                static_cast<unsigned long long>(buckets[3]));

    hist.reset();
    println("  after reset() p99={}µs",
                static_cast<long long>(hist.p99().count()));

    // ErrorBudgetTracker — SLO tracker
    bool violated = false;
    SloConfig cfg;
    cfg.p99_target    = 10000us;    // 10ms
    cfg.p999_target   = 50000us;    // 50ms
    cfg.error_budget  = 0.01;       // 1% error allowance
    cfg.on_violation  = [&](std::string_view name) {
        println("  SLO violation detected: {}", name);
        violated = true;
    };

    ErrorBudgetTracker tracker(std::move(cfg), "payment");

    for (int i = 0; i < 980; ++i) tracker.record_success(800us);
    for (int i = 0; i < 20;  ++i) tracker.record_error();   // 2% error rate

    println("  total={} errors={} error_rate={:.4f}",
                static_cast<unsigned long long>(tracker.total_count()),
                static_cast<unsigned long long>(tracker.error_count()),
                tracker.error_rate());
    println("  budget_exhausted={}",
                tracker.budget_exhausted() ? "yes" : "no");

    tracker.check_slo();

    // histogram() method
    auto& inner_hist = tracker.histogram();
    println("  inner histogram p99={}µs\n",
                static_cast<long long>(inner_hist.p99().count()));
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("=== qbuem Pipeline Observability + SLO Example ===\n");

    demo_action_metrics();
    demo_observer();
    demo_version();
    demo_health();
    demo_slo();

    println("=== Done ===");
    return 0;
}
