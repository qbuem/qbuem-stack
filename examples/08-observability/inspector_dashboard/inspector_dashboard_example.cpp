/**
 * @file inspector_dashboard_example.cpp
 * @brief Demonstrates qbuem-cli TUI dashboard and qbuem-inspector embedding.
 *
 * Shows how to:
 * 1. Implement ICliDataSource to expose custom metrics.
 * 2. Render a live ANSI TUI dashboard with tui_render().
 * 3. Export an offline HTML snapshot with html_export().
 * 4. Embed InspectorServer to serve Full Journey timelines.
 * 5. Use CliServer with multiple registered data sources.
 */

#include <qbuem/tools/qbuem_cli.hpp>
#include <qbuem/tools/qbuem_inspector.hpp>
#include <qbuem/tracing/lifecycle_tracer.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <qbuem/compat/print.hpp>
#include <thread>

using namespace qbuem::tools;
using namespace qbuem::tracing;

// ── §1 Custom data sources ────────────────────────────────────────────────────

/**
 * @brief Simulated pipeline metrics source.
 */
class PipelineMetricsSource final : public ICliDataSource {
public:
    explicit PipelineMetricsSource() {
        // Seed some initial counter values
        rps_.store(12345, std::memory_order_relaxed);
        p99_ns_.store(850'000, std::memory_order_relaxed);
        errors_.store(3, std::memory_order_relaxed);
    }

    void collect(MetricGroup& out) const noexcept override {
        out.source = std::string(source_name());
        out.metrics.push_back({.name = "pipeline.rps",
                               .value = static_cast<double>(rps_.load()),
                               .unit  = "rps"});
        out.metrics.push_back({.name = "pipeline.p99_latency",
                               .value = static_cast<double>(p99_ns_.load()) / 1000.0,
                               .unit  = "µs"});
        out.metrics.push_back({.name = "pipeline.error_rate",
                               .value = static_cast<double>(errors_.load()),
                               .unit  = "err/s"});
        out.metrics.push_back({.name = "pipeline.queue_depth",
                               .value = static_cast<double>(queue_depth_.load()),
                               .unit  = "msgs"});
    }

    [[nodiscard]] std::string_view source_name() const noexcept override {
        return "pipeline";
    }

    // Simulate metric updates
    void tick() noexcept {
        rps_.fetch_add(7, std::memory_order_relaxed);
        p99_ns_.store(700'000 + (std::rand() % 400'000), std::memory_order_relaxed); // NOLINT
        queue_depth_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    mutable std::atomic<uint64_t> rps_{0};
    mutable std::atomic<uint64_t> p99_ns_{0};
    mutable std::atomic<uint64_t> errors_{0};
    mutable std::atomic<uint64_t> queue_depth_{0};
};

/**
 * @brief Simulated SHM channel metrics source.
 */
class ShmMetricsSource final : public ICliDataSource {
public:
    void collect(MetricGroup& out) const noexcept override {
        out.source = std::string(source_name());
        out.metrics.push_back({.name  = "shm.throughput",
                               .value = 9842.5,
                               .unit  = "MB/s"});
        out.metrics.push_back({.name  = "shm.queue_depth",
                               .value = static_cast<double>(queue_.load()),
                               .unit  = "msgs"});
        out.metrics.push_back({.name  = "shm.dropped",
                               .value = 0.0,
                               .unit  = "msgs"});
    }

    [[nodiscard]] std::string_view source_name() const noexcept override {
        return "shm";
    }

    void tick() noexcept { queue_.fetch_add(1, std::memory_order_relaxed); }

private:
    mutable std::atomic<uint64_t> queue_{64};
};

// ── §2 TUI render demo ────────────────────────────────────────────────────────

void show_tui_render() {
    std::println("\n=== §2 ANSI TUI Dashboard Render ===");

    PipelineMetricsSource pipeline_src;
    ShmMetricsSource shm_src;
    pipeline_src.tick();
    shm_src.tick();

    CliServer server;
    server.register_source(&pipeline_src);
    server.register_source(&shm_src);

    auto groups = server.collect();

    // Render to stdout (would normally go to a terminal that supports ANSI)
    tui_render(groups, stdout);
}

// ── §3 HTML export demo ───────────────────────────────────────────────────────

void show_html_export() {
    std::println("\n=== §3 HTML Snapshot Export ===");

    PipelineMetricsSource pipeline_src;
    ShmMetricsSource shm_src;
    pipeline_src.tick(); pipeline_src.tick();

    CliServer server;
    server.register_source(&pipeline_src);
    server.register_source(&shm_src);

    auto groups = server.collect();
    auto html = html_export(groups, "qbuem System Snapshot — 2026-03-19");

    std::println("  Generated HTML: {} bytes", html.size());
    std::println("  First 120 chars: {}", html.substr(0, 120));
    std::println("  (In production: write to snapshot.html for offline sharing)");
}

// ── §4 InspectorServer embedding demo ─────────────────────────────────────────

void show_inspector_server_demo() {
    std::println("\n=== §4 InspectorServer Embedding ===");

    LifecycleTracer tracer("demo-app");

    // Generate some span data
    for (int i = 0; i < 5; ++i) {
        auto root = tracer.start_lifecycle("http_request");
        auto ctx  = root.context();
        {
            auto parse = tracer.start_span("parse_headers", ctx);
            std::this_thread::sleep_for(std::chrono::microseconds{10});
            parse.end(SpanStatus::Ok);
        }
        {
            auto db = tracer.start_span("db_query", ctx);
            std::this_thread::sleep_for(std::chrono::microseconds{50 + i * 10});
            db.end(i == 4 ? SpanStatus::Error : SpanStatus::Ok);
        }
        root.end(i == 4 ? SpanStatus::Error : SpanStatus::Ok);
    }

    // Embed in application — the InspectorServer drains from the tracer
    InspectorServer inspector(&tracer, nullptr, 9091);

    // Manually drain the collector to demonstrate it works without the HTTP loop
    tracer.drain([&](const SpanRecord& rec) {
        inspector.collector().ingest(rec);
        return true;
    });
    inspector.collector().evict_old(0); // evict nothing at t=0

    std::println("  InspectorServer would serve at http://localhost:9091");
    std::println("  Active journeys tracked:    {}", inspector.collector().in_flight());
    std::println("  Completed journeys tracked: {}", inspector.collector().completed());
    std::println("  Total journeys in window:   {}", inspector.collector().journeys().size());

    // Show the inspector HTML size (embedded UI)
    auto html = inspector_html();
    std::println("  Embedded inspector HTML:    {} bytes", html.size());
}

// ── §5 Multiple sources + port configuration ──────────────────────────────────

void show_multi_source_server() {
    std::println("\n=== §5 CliServer with Multiple Sources ===");

    PipelineMetricsSource pipeline_src;
    ShmMetricsSource shm_src;

    CliServer server;
    server.set_port(9090);
    server.set_refresh(std::chrono::milliseconds{500});
    server.register_source(&pipeline_src);
    server.register_source(&shm_src);

    std::println("  CliServer configured on port {}", server.port());

    // Simulate a few ticks
    for (int i = 0; i < 3; ++i) {
        pipeline_src.tick();
        shm_src.tick();
    }

    auto groups = server.collect();
    std::println("  Sources registered: {}", groups.size());
    for (const auto& g : groups) {
        std::println("  Source '{}': {} metrics", g.source, g.metrics.size());
        for (const auto& m : g.metrics) {
            std::println("    {:<35} {:>10.2f} {}", m.name, m.value, m.unit);
        }
    }

    std::println("  (In production: co_await server.listen(st) serves HTTP)");
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::println("╔══════════════════════════════════════════════════════╗");
    std::println("║   qbuem-stack v3.1.0 — Inspector Dashboard Example  ║");
    std::println("╚══════════════════════════════════════════════════════╝");

    show_tui_render();
    show_html_export();
    show_inspector_server_demo();
    show_multi_source_server();

    std::println("\n✓ Inspector dashboard demonstration complete.");
    std::println("  To use the full inspector UI:");
    std::println("  1. Embed InspectorServer in your application");
    std::println("  2. Run: open http://localhost:9091");
    std::println("  3. See real-time Full Journey Gantt timelines in your browser");
    return 0;
}
