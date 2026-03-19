/**
 * @file lifecycle_tracing_example.cpp
 * @brief Demonstrates LifecycleTracer, TraceLogger, and CoroExplorer.
 *
 * This example shows how to:
 * 1. Instrument request lifecycles with zero-allocation span recording.
 * 2. Correlate structured log events with W3C trace context.
 * 3. Track coroutine hierarchy using CoroGuard RAII handles.
 * 4. Export trace data to the qbuem-inspector via JourneyCollector.
 */

#include <qbuem/tracing/lifecycle_tracer.hpp>
#include <qbuem/tracing/trace_logger.hpp>
#include <qbuem/tools/coro_explorer.hpp>
#include <qbuem/tools/qbuem_inspector.hpp>

#include <chrono>
#include <qbuem/compat/print.hpp>
#include <thread>

using namespace qbuem::tracing;
using namespace qbuem::tools;

// ── Demo: W3C Traceparent propagation ─────────────────────────────────────────

void show_traceparent_format() {
    std::println("\n=== §1 W3C Traceparent Format ===");

    TraceContext ctx;
    ctx.trace_id   = 0xDEADBEEFCAFEBABEULL;
    ctx.parent_id  = 0x0102030405060708ULL;
    ctx.is_sampled = true;

    std::println("  trace_id  : {:016x}", ctx.trace_id);
    std::println("  parent_id : {:016x}", ctx.parent_id);
    std::println("  sampled   : {}", ctx.is_sampled ? "true" : "false");

    // W3C traceparent header format:
    // 00-<trace_id_128_hex>-<span_id_hex>-01
    std::println("  traceparent: 00-{:016x}0000000000000000-{:016x}-01",
                 ctx.trace_id, ctx.parent_id);
}

// ── Demo: LifecycleTracer span tree ──────────────────────────────────────────

void show_lifecycle_trace() {
    std::println("\n=== §2 Lifecycle Tracer — Span Tree ===");

    LifecycleTracer tracer("order-service");

    // Root span: process_order
    auto root = tracer.start_lifecycle("process_order");
    auto ctx0 = root.context();
    std::println("  [root] trace_id={:016x} span_id={:016x}",
                 ctx0.trace_id, ctx0.parent_id);

    // Child span: validate
    {
        auto validate_span = tracer.start_span("validate", ctx0);
        auto ctx1 = validate_span.context();
        std::println("  [child:validate] span_id={:016x} parent={:016x}",
                     ctx1.parent_id, ctx0.parent_id);
        std::this_thread::sleep_for(std::chrono::microseconds{50});
        validate_span.end(SpanStatus::Ok);
    }

    // Child span: db_query
    {
        auto db_span = tracer.start_span("db_query", ctx0);
        std::this_thread::sleep_for(std::chrono::microseconds{120});
        db_span.end(SpanStatus::Ok);
        std::println("  [child:db_query] completed");
    }

    root.end(SpanStatus::Ok);

    std::println("  Total spans emitted: {}", tracer.total_spans());
    std::println("  Dropped spans: {}", tracer.dropped_spans());
}

// ── Demo: TraceLogger correlated logs ─────────────────────────────────────────

void show_trace_logger() {
    std::println("\n=== §3 TraceLogger — W3C-Correlated Log Lines ===");

    // Use a custom sink that prints to stdout instead of stderr for this demo
    class StdoutSink final : public ILogSink {
    public:
        void write(const TraceLogRecord& rec) noexcept override {
            // Convert ns to readable time
            time_t sec = static_cast<time_t>(rec.timestamp_ns / 1'000'000'000ULL);
            uint32_t ns = static_cast<uint32_t>(rec.timestamp_ns % 1'000'000'000ULL);
            struct tm tm_buf{};
            ::gmtime_r(&sec, &tm_buf);
            char ts[32]{};
            ::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);
            std::println("  [{}.{:09d}Z] [{}] trace={:016x} [{}] {}",
                         ts, ns,
                         level_str(rec.level),
                         rec.trace_id,
                         rec.service,
                         rec.msg);
        }
    } sink;

    TraceLogger<512> logger("order-service", &sink, LogLevel::Debug);
    logger.start();

    // Simulate request with context
    TraceContext ctx;
    ctx.trace_id  = 0xABCD1234ABCD5678ULL;
    ctx.parent_id = 0x1111222233334444ULL;

    logger.info(ctx,  "Received order request");
    logger.debug(ctx, "Validation started");
    logger.warn(ctx,  "Payment provider latency: 180 ms");
    logger.info(ctx,  "Order committed to DB");

    // Give flush thread time to drain
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    logger.stop();

    std::println("  Dropped records: {}", logger.dropped());
    std::println("  Ring size at stop: {}", logger.ring_size());
}

// ── Demo: CoroGuard + CoroExplorer ───────────────────────────────────────────

void show_coro_explorer() {
    std::println("\n=== §4 CoroExplorer — Coroutine Lifecycle Tracking ===");

    CoroExplorer explorer;

    // Simulate coroutine registrations
    {
        CoroGuard root("handle_connection", 0, 0);
        std::println("  Registered root coro #{} 'handle_connection'", root.id());

        {
            CoroGuard child("parse_http_request", root.id(), 1);
            std::println("  Registered child coro #{} 'parse_http_request'", child.id());
            child.suspend("TcpSocket::recv");
            std::this_thread::sleep_for(std::chrono::microseconds{100});
            child.resume();
        } // child destroyed → on_done()

        {
            CoroGuard child2("route_handler", root.id(), 1);
            child2.suspend("db_query");
            // Snapshot while child2 is suspended
            auto snap = explorer.snapshot();
            std::println("  Live coroutines during route_handler suspension:");
            CoroExplorer::render_tui(snap, stdout);
        }
    }

    std::println("  Active after all guards gone: {}", explorer.active());
    std::println("  Total created: {}", explorer.total());
}

// ── Demo: JourneyCollector groups spans ───────────────────────────────────────

void show_journey_collector() {
    std::println("\n=== §5 JourneyCollector — Full Journey Timeline ===");

    LifecycleTracer tracer("demo");
    JourneyCollector collector;

    // Simulate 3 requests (journeys)
    for (int req = 0; req < 3; ++req) {
        auto root = tracer.start_lifecycle("request");
        auto ctx  = root.context();

        // Drain the root span into the collector
        tracer.drain([&](const SpanRecord& rec) {
            collector.ingest(rec);
            return true;
        });

        // Child spans
        {
            auto child = tracer.start_span("stage_a", ctx);
            std::this_thread::sleep_for(std::chrono::microseconds{20 * (req + 1)});
            child.end(SpanStatus::Ok);
        }
        {
            auto child = tracer.start_span("stage_b", ctx);
            std::this_thread::sleep_for(std::chrono::microseconds{30});
            child.end(SpanStatus::Ok);
        }
        root.end(SpanStatus::Ok);

        tracer.drain([&](const SpanRecord& rec) {
            collector.ingest(rec);
            return true;
        });
    }

    std::println("  In-flight journeys:  {}", collector.in_flight());
    std::println("  Completed journeys:  {}", collector.completed());
    std::println("  Total journeys:      {}", collector.journeys().size());

    for (const auto& [id, j] : collector.journeys()) {
        std::println("  Journey {:016x}: {} spans  duration={:.3f} ms  complete={}",
                     id,
                     j.spans.size(),
                     static_cast<double>(j.duration_ns()) / 1e6,
                     j.complete() ? "yes" : "no");
    }
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::println("╔══════════════════════════════════════════════════════╗");
    std::println("║   qbuem-stack v3.1.0 — Lifecycle Tracing Example    ║");
    std::println("╚══════════════════════════════════════════════════════╝");

    show_traceparent_format();
    show_lifecycle_trace();
    show_trace_logger();
    show_coro_explorer();
    show_journey_collector();

    std::println("\n✓ All lifecycle tracing demonstrations complete.");
    return 0;
}
