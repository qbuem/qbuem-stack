/**
 * @file examples/12-saas/otlp_export/otlp_export_example.cpp
 * @brief How to export spans to an OpenTelemetry collector (OTLP/JSON).
 *
 * `OtlpHttpExporter` implements the `SpanExporter` port: it batches completed
 * spans on a background thread and hands each batch to an INJECTED transport as
 * an OTLP/HTTP JSON body. You provide the transport (the core has no TLS and the
 * fetch client is coroutine-native, so byte delivery is deployment-specific):
 *
 *   // Inside your app's dispatcher, POST to a local collector:
 *   OtlpHttpExporter exporter([&](std::string_view json) {
 *       // co_await fetch("http://localhost:4318/v1/traces")
 *       //     .method(Method::Post).header("content-type","application/json")
 *       //     .body(std::string(json)).send(st);
 *   }, {.service_name = "orders"});
 *   PipelineTracer::instance().set_exporter(shared_exporter);
 *
 * This demo injects a transport that just prints the JSON, so it is
 * self-contained (no collector / network required).
 */

#include <qbuem/qbuem_stack.hpp> // exposes tracing/otlp_exporter.hpp via the umbrella

#include <qbuem/compat/print.hpp> // std::print/println shim (GCC 13 lacks <print>)

#include <chrono>
#include <string_view>

using namespace qbuem::tracing;

namespace {
SpanData make_span(std::string_view name, SpanStatus status) {
  SpanData s;
  s.trace_id.bytes = {0x4b, 0xf9, 0x2f, 0x35, 0x77, 0xb3, 0x4d, 0xa6,
                      0xa3, 0xce, 0x92, 0x9d, 0x0e, 0x0e, 0x47, 0x36};
  s.span_id.bytes  = {0x00, 0xf0, 0x67, 0xaa, 0x0b, 0xa9, 0x02, 0xb7};
  s.name           = name;
  s.pipeline_name  = "orders";
  s.action_name    = "handle";
  s.start_time     = std::chrono::system_clock::now();
  s.end_time       = s.start_time + std::chrono::milliseconds(7);
  s.status         = status;
  s.set_attribute("http.method", "POST");
  s.set_attribute("http.route", "/orders");
  return s;
}
} // namespace

int main() {
  std::println("=== OTLP/JSON span export ===\n");

  // 1. The pure encoder — render a span as an OTLP ExportTraceServiceRequest.
  std::println("--- encode_otlp_traces_json (one span) ---");
  SpanData s = make_span("POST /orders", SpanStatus::Ok);
  std::println("{}\n", encode_otlp_traces_json(std::span<const SpanData>(&s, 1),
                                               "orders-svc"));

  // 2. The exporter — batches on a background thread, calls the injected
  //    transport. Here the transport just prints; in production it POSTs to a
  //    collector. flush() forces a synchronous send for the demo.
  std::println("--- OtlpHttpExporter (injected transport) ---");
  int batches = 0;
  OtlpHttpExporter exporter(
      [&](std::string_view json) {
        ++batches;
        std::println("transport got a {}-byte OTLP batch", json.size());
      },
      {.service_name = "orders-svc"});

  exporter.export_span(make_span("GET /orders/1", SpanStatus::Ok));
  exporter.export_span(make_span("POST /orders", SpanStatus::Error));
  exporter.flush();    // synchronous drain + send
  exporter.shutdown(); // stop worker + final flush

  std::println("\nexported via {} transport call(s), {} dropped",
               batches, exporter.dropped());
  return 0;
}
