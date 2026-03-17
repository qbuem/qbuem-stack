# tracing

**Category:** Observability
**File:** `tracing_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates distributed tracing with `TraceContext` (W3C traceparent format), `PipelineTracer`, `Span`, `SpanExporter`, and all four `Sampler` types. Provides end-to-end trace propagation compatible with OpenTelemetry collectors (Jaeger, Zipkin, OTLP).

## Scenario

A multi-stage order processing pipeline propagates W3C trace context across service calls. Each pipeline action creates a child span, records attributes (order_id, user_id, error_reason), and exports completed spans to a backend. A sampling policy controls which traces are recorded.

## Architecture Diagram

```
  Trace Context Propagation
  ──────────────────────────────────────────────────────────
  Incoming HTTP request
  "traceparent: 00-<trace_id>-<span_id>-01"
       │
       ▼
  TraceContext::from_traceparent(header)
  ├─ trace_id:       128-bit unique trace identifier
  ├─ parent_span_id: 64-bit parent span
  └─ flags:          0x01 = sampled

  New child span:
  TraceContext::child_span()
  └─ same trace_id, new span_id

  PipelineTracer + SpanExporter
  ──────────────────────────────────────────────────────────
  Pipeline action starts
       │
       ▼
  pt.start_span(name, pipeline, action, trace_ctx)
  ├─ set_attribute("order_id", "ORD-12345")
  ├─ set_attribute("user_id",  "usr-42")
  ├─ set_status(SpanStatus::Ok | Error)
  └─ RAII destructor → export_span(SpanData)
       │
       ▼
  PrintExporter (custom)  or  LoggingSpanExporter (built-in)
  └─ prints to stdout / log

  Samplers
  ──────────────────────────────────────────────────────────
  AlwaysSampler     → RECORD_AND_SAMPLE (100%)
  NeverSampler      → DROP (0%)
  ProbabilitySampler(0.5) → ~50% sampled
  ParentBasedSampler → follow parent's sampling decision
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `TraceContext::generate()` | Create a new root trace context |
| `TraceContext::from_traceparent(str)` | Parse W3C traceparent header |
| `ctx.to_traceparent()` | Serialize to W3C format |
| `ctx.child_span()` | Create child context (same trace, new span) |
| `PipelineTracer` | Global tracer managing span lifecycle |
| `PipelineTracer::set_global_tracer(tracer)` | Install as global tracer |
| `PipelineTracer::global()` | Access global tracer |
| `tracer.start_span(name, pipeline, action)` | Open a span (RAII) |
| `span.set_attribute(key, value)` | Attach key-value metadata |
| `span.set_status(SpanStatus)` | Mark span OK or Error |
| `SpanExporter::export_span(SpanData)` | Receive completed span |
| `AlwaysSampler` / `NeverSampler` | Constant sampling policies |
| `ProbabilitySampler(rate)` | Probabilistic sampling |
| `ParentBasedSampler` | Follow parent's sampling flag |

## Input / Output

### TraceContext

| Input | Output |
|-------|--------|
| `generate()` | `"00-<32hex>-<16hex>-00"` traceparent |
| `from_traceparent("00-4bf9...0e4736-...")` | Parsed `TraceContext` |
| `root.child_span()` | Same trace_id, new span_id |

### Span Export (PrintExporter)

```
[trace] span='process_order' action='validate' status=OK dur=1234us
        order_id=ORD-12345
        user_id=usr-42
[trace] span='charge_payment' action='payment' status=ERR dur=567us
        order_id=ORD-12345
        error_reason=insufficient_funds
```

### Samplers

| Sampler | Decision |
|---------|---------|
| `AlwaysSampler` | SAMPLE |
| `NeverSampler` | DROP |
| `ProbabilitySampler(0.5)` | ~500/1000 SAMPLE |
| `ParentBasedSampler` (parent sampled) | SAMPLE |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tracing_example
./build/examples/08-observability/tracing/tracing_example
```

## Notes

- Spans are exported on RAII destruction — ensure the span object goes out of scope before checking exported data.
- The `PipelineTracer` is opt-in; zero overhead when disabled (sampled=false or NeverSampler).
- For production use, replace `PrintExporter` with `OtlpGrpcExporter` or `JaegerExporter`.
