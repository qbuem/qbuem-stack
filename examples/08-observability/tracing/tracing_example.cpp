/**
 * @file examples/tracing_example.cpp
 * @brief Distributed tracing example — TraceContext, Span, PipelineTracer, Sampler
 */
#include <qbuem/tracing/trace_context.hpp>
#include <qbuem/tracing/span.hpp>
#include <qbuem/tracing/exporter.hpp>
#include <qbuem/tracing/sampler.hpp>
#include <qbuem/compat/print.hpp>

#include <chrono>
#include <memory>
#include <thread>

using namespace qbuem::tracing;
using namespace std::chrono_literals;

// ─── Custom SpanExporter ──────────────────────────────────────────────────────

class PrintExporter final : public SpanExporter {
public:
    void export_span(const SpanData& s) override {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            s.end_time - s.start_time);
        std::println("[trace] span='{}' action='{}' status={} dur={}us",
            s.name, s.action_name,
            s.status == SpanStatus::Ok ? "OK"
            : s.status == SpanStatus::Error ? "ERR" : "Unset",
            dur.count());
        for (size_t i = 0; i < s.attribute_count; ++i) {
            std::println("        {}={}", s.attributes[i].key, s.attributes[i].value);
        }
    }
    void shutdown() override {}
};

// ─── TraceContext (W3C traceparent) ──────────────────────────────────────────

void trace_context_example() {
    std::println("=== TraceContext ===");

    // Generate root context
    auto root = TraceContext::generate();
    std::println("[trace] Root traceparent: {}", root.to_traceparent());
    std::println("[trace] TraceId valid: {}", root.trace_id.is_valid());
    std::println("[trace] SpanId valid:  {}", root.parent_span_id.is_valid());

    // Create child span
    auto child = root.child_span();
    std::println("[trace] Child traceparent: {}", child.to_traceparent());

    // Parse traceparent header
    std::string tp = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    auto parsed = TraceContext::from_traceparent(tp);
    if (parsed) {
        std::println("[trace] Parsed traceparent valid: {}", parsed->trace_id.is_valid());
        std::println("[trace] Sampled: {}", static_cast<int>(parsed->flags));
    }
}

// ─── PipelineTracer + SpanExporter ───────────────────────────────────────────

void tracer_example() {
    std::println("\n=== PipelineTracer + PrintExporter ===");

    // Configure global PipelineTracer
    auto tracer = std::make_unique<PipelineTracer>();
    tracer->set_exporter(std::make_shared<PrintExporter>());
    PipelineTracer::set_global_tracer(std::move(tracer));

    auto& pt = PipelineTracer::global();

    // Create span — automatically exported on RAII destruction
    {
        auto span = pt.start_span("process_order", "order-pipeline", "validate");
        span.set_attribute("order_id", "ORD-12345");
        span.set_attribute("user_id",  "usr-42");
        std::this_thread::sleep_for(1ms); // simulate processing
        span.set_status(SpanStatus::Ok);
    } // destruction → export

    // Error span
    {
        auto span = pt.start_span("charge_payment", "order-pipeline", "payment");
        span.set_attribute("order_id",     "ORD-12345");
        span.set_attribute("error_reason", "insufficient_funds");
        span.set_status(SpanStatus::Error);
    }

    // Child span — linked with the same TraceContext
    auto root_ctx = TraceContext::generate();
    {
        auto span = pt.start_span("serialize_response",
                                  "api-pipeline", "serialize",
                                  root_ctx);
        span.set_attribute("content_type", "application/json");
        span.set_status(SpanStatus::Ok);
    }
}

// ─── Sampler ─────────────────────────────────────────────────────────────────

void sampler_example() {
    std::println("\n=== Sampler ===");

    // AlwaysSampler
    AlwaysSampler always;
    SamplingContext sctx{"my-pipeline", "my-action", "my-span", {}};
    auto d1 = always.should_sample(sctx);
    std::println("[sampler] AlwaysSampler: {}",
        d1 == SamplingDecision::RECORD_AND_SAMPLE ? "SAMPLE" : "DROP");

    // NeverSampler
    NeverSampler never;
    auto d2 = never.should_sample(sctx);
    std::println("[sampler] NeverSampler: {}",
        d2 == SamplingDecision::DROP ? "DROP" : "SAMPLE");

    // ProbabilitySampler (50%)
    ProbabilitySampler prob(0.5);
    size_t sampled = 0;
    for (int i = 0; i < 1000; ++i) {
        if (prob.should_sample(sctx) == SamplingDecision::RECORD_AND_SAMPLE)
            ++sampled;
    }
    std::println("[sampler] ProbabilitySampler(0.5): {}/1000", sampled);

    // ParentBasedSampler — follows parent flags
    ParentBasedSampler parent;
    TraceContext sampled_ctx = TraceContext::generate();
    sampled_ctx.flags = 0x01; // sampled
    SamplingContext pctx{"pipe", "action", "span", &sampled_ctx};
    auto d3 = parent.should_sample(pctx);
    std::println("[sampler] ParentBasedSampler (parent sampled): {}",
        d3 == SamplingDecision::RECORD_AND_SAMPLE ? "SAMPLE" : "DROP");

    // LoggingSpanExporter direct usage
    LoggingSpanExporter log_exp;
    SpanData sd;
    sd.name          = "test_span";
    sd.pipeline_name = "test-pipeline";
    sd.action_name   = "test-action";
    sd.start_time    = std::chrono::system_clock::now();
    sd.end_time      = sd.start_time + 500us;
    sd.status        = SpanStatus::Ok;
    sd.set_attribute("key1", "val1");
    log_exp.export_span(sd);
}

int main() {
    trace_context_example();
    tracer_example();
    sampler_example();
    return 0;
}
