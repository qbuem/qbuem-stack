/**
 * @file examples/tracing_example.cpp
 * @brief 분산 추적 예시 — TraceContext, Span, PipelineTracer, Sampler
 */
#include <qbuem/tracing/trace_context.hpp>
#include <qbuem/tracing/span.hpp>
#include <qbuem/tracing/exporter.hpp>
#include <qbuem/tracing/sampler.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace qbuem::tracing;
using namespace std::chrono_literals;

// ─── 커스텀 SpanExporter ────────────────────────────────────────────────────

class PrintExporter final : public SpanExporter {
public:
    void export_span(const SpanData& s) override {
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            s.end_time - s.start_time);
        std::cout << "[trace] span='" << s.name
                  << "' action='" << s.action_name
                  << "' status=" << (s.status == SpanStatus::Ok ? "OK"
                                  : s.status == SpanStatus::Error ? "ERR" : "Unset")
                  << " dur=" << dur.count() << "us\n";
        for (size_t i = 0; i < s.attribute_count; ++i) {
            std::cout << "        " << s.attributes[i].key
                      << "=" << s.attributes[i].value << "\n";
        }
    }
    void shutdown() override {}
};

// ─── TraceContext (W3C traceparent) ─────────────────────────────────────────

void trace_context_example() {
    std::cout << "=== TraceContext ===\n";

    // 루트 컨텍스트 생성
    auto root = TraceContext::generate();
    std::cout << "[trace] Root traceparent: " << root.to_traceparent() << "\n";
    std::cout << "[trace] TraceId valid: " << root.trace_id.is_valid() << "\n";
    std::cout << "[trace] SpanId valid:  " << root.parent_span_id.is_valid() << "\n";

    // 자식 스팬 생성
    auto child = root.child_span();
    std::cout << "[trace] Child traceparent: " << child.to_traceparent() << "\n";

    // traceparent 헤더 파싱
    std::string tp = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    auto parsed = TraceContext::from_traceparent(tp);
    if (parsed) {
        std::cout << "[trace] Parsed traceparent valid: "
                  << parsed->trace_id.is_valid() << "\n";
        std::cout << "[trace] Sampled: " << static_cast<int>(parsed->flags) << "\n";
    }
}

// ─── PipelineTracer + SpanExporter ─────────────────────────────────────────

void tracer_example() {
    std::cout << "\n=== PipelineTracer + PrintExporter ===\n";

    // PipelineTracer 전역 설정
    auto tracer = std::make_unique<PipelineTracer>();
    tracer->set_exporter(std::make_shared<PrintExporter>());
    PipelineTracer::set_global_tracer(std::move(tracer));

    auto& pt = PipelineTracer::global();

    // 스팬 생성 — RAII로 소멸 시 자동 export
    {
        auto span = pt.start_span("process_order", "order-pipeline", "validate");
        span.set_attribute("order_id", "ORD-12345");
        span.set_attribute("user_id",  "usr-42");
        std::this_thread::sleep_for(1ms); // 처리 시뮬레이션
        span.set_status(SpanStatus::Ok);
    } // 소멸 → export

    // 에러 스팬
    {
        auto span = pt.start_span("charge_payment", "order-pipeline", "payment");
        span.set_attribute("order_id",     "ORD-12345");
        span.set_attribute("error_reason", "insufficient_funds");
        span.set_status(SpanStatus::Error);
    }

    // 자식 스팬 — 같은 TraceContext로 연결
    auto root_ctx = TraceContext::generate();
    {
        auto span = pt.start_span("serialize_response",
                                  "api-pipeline", "serialize",
                                  root_ctx);
        span.set_attribute("content_type", "application/json");
        span.set_status(SpanStatus::Ok);
    }
}

// ─── Sampler ────────────────────────────────────────────────────────────────

void sampler_example() {
    std::cout << "\n=== Sampler ===\n";

    // AlwaysSampler
    AlwaysSampler always;
    SamplingContext sctx{"my-pipeline", "my-action", "my-span", {}};
    auto d1 = always.should_sample(sctx);
    std::cout << "[sampler] AlwaysSampler: "
              << (d1 == SamplingDecision::RECORD_AND_SAMPLE ? "SAMPLE" : "DROP") << "\n";

    // NeverSampler
    NeverSampler never;
    auto d2 = never.should_sample(sctx);
    std::cout << "[sampler] NeverSampler: "
              << (d2 == SamplingDecision::DROP ? "DROP" : "SAMPLE") << "\n";

    // ProbabilitySampler (50%)
    ProbabilitySampler prob(0.5);
    size_t sampled = 0;
    for (int i = 0; i < 1000; ++i) {
        if (prob.should_sample(sctx) == SamplingDecision::RECORD_AND_SAMPLE)
            ++sampled;
    }
    std::cout << "[sampler] ProbabilitySampler(0.5): " << sampled << "/1000\n";

    // ParentBasedSampler — 부모 플래그 따름
    ParentBasedSampler parent;
    TraceContext sampled_ctx = TraceContext::generate();
    sampled_ctx.flags = 0x01; // sampled
    SamplingContext pctx{"pipe", "action", "span", &sampled_ctx};
    auto d3 = parent.should_sample(pctx);
    std::cout << "[sampler] ParentBasedSampler (parent sampled): "
              << (d3 == SamplingDecision::RECORD_AND_SAMPLE ? "SAMPLE" : "DROP") << "\n";

    // LoggingSpanExporter 직접 사용
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
