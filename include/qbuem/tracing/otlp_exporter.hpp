#pragma once

/**
 * @file qbuem/tracing/otlp_exporter.hpp
 * @brief OTLP/HTTP (JSON) span exporter — ships completed spans off-process to an
 *        OpenTelemetry collector. Implements the `SpanExporter` port.
 * @ingroup qbuem_tracing_exporter
 *
 * Two parts:
 *  - `encode_otlp_traces_json()` — a pure function rendering spans as an OTLP/HTTP
 *    `ExportTraceServiceRequest` JSON document (RFC: OTLP/JSON; trace/span IDs as
 *    lowercase hex, timestamps as decimal-string Unix nanos, status 0/1/2).
 *  - `OtlpHttpExporter` — buffers spans and flushes batches on a background
 *    thread via an **injected transport** callback, so the exporter stays
 *    zero-dependency and reactor-agnostic (ports & adapters). Supply a transport
 *    that delivers the JSON body to your collector — e.g. a `fetch()` POST to a
 *    local collector (`http://localhost:4318/v1/traces`) driven by your app's
 *    dispatcher, or a write to a sidecar agent. The encoding is the value here;
 *    the byte delivery is deployment-specific.
 *
 * Off the hot path: `export_span()` only enqueues (a span completes far less often
 * than a packet arrives); the blocking transport runs on a dedicated flush thread
 * so it never stalls a reactor (Pillar 1). The internal mutex/cv mirror
 * `LoggingSpanExporter`/`TraceLogger`, which are likewise cold-path.
 *
 * @code
 * auto exp = std::make_shared<qbuem::tracing::OtlpHttpExporter>(
 *     [](std::string_view json) { post_to_collector(json); },          // transport
 *     qbuem::tracing::OtlpHttpExporter::Config{.service_name = "orders"});
 * qbuem::tracing::PipelineTracer::instance().set_exporter(exp);
 * @endcode
 */

#include <qbuem/tracing/exporter.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace qbuem::tracing {

namespace detail::otlp {

inline void json_escape_to(std::string& out, std::string_view s) {
  constexpr std::string_view hex = "0123456789abcdef";
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "\\u00";
          out += hex[(static_cast<unsigned char>(c) >> 4) & 0x0F];
          out += hex[static_cast<unsigned char>(c) & 0x0F];
        } else {
          out += c;
        }
    }
  }
}

inline uint64_t to_unix_nano(std::chrono::system_clock::time_point tp) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch())
          .count());
}

// OTLP status code: 0 UNSET, 1 OK, 2 ERROR.
inline int status_code(SpanStatus s) {
  switch (s) {
    case SpanStatus::Ok:    return 1;
    case SpanStatus::Error: return 2;
    default:                return 0;
  }
}

} // namespace detail::otlp

/**
 * @brief Render spans as one OTLP/HTTP JSON `ExportTraceServiceRequest`.
 *
 * All spans go under a single resource (`service.name`) and instrumentation scope
 * (`qbuem-stack`). `pipeline_name`/`action_name` become `qbuem.pipeline`/
 * `qbuem.action` attributes; an Error status carries its message.
 */
[[nodiscard]] inline std::string encode_otlp_traces_json(
    std::span<const SpanData> spans, std::string_view service_name) {
  using namespace detail::otlp;
  std::string out;
  out.reserve(256 + spans.size() * 256);

  out += "{\"resourceSpans\":[{\"resource\":{\"attributes\":[{\"key\":\"service.name\","
         "\"value\":{\"stringValue\":\"";
  json_escape_to(out, service_name);
  out += "\"}}]},\"scopeSpans\":[{\"scope\":{\"name\":\"qbuem-stack\"},\"spans\":[";

  bool first_span = true;
  for (const auto& s : spans) {
    if (!first_span) out += ',';
    first_span = false;

    std::array<char, 33> tb{};
    std::array<char, 17> sb{};
    s.trace_id.to_chars(tb.data(), tb.size());
    s.span_id.to_chars(sb.data(), sb.size());

    out += "{\"traceId\":\"";
    out += tb.data();
    out += "\",\"spanId\":\"";
    out += sb.data();
    out += "\"";
    if (s.parent_span_id.is_valid()) {
      std::array<char, 17> pb{};
      s.parent_span_id.to_chars(pb.data(), pb.size());
      out += ",\"parentSpanId\":\"";
      out += pb.data();
      out += "\"";
    }
    out += ",\"name\":\"";
    json_escape_to(out, s.name);
    out += "\",\"kind\":1,\"startTimeUnixNano\":\"";
    out += std::to_string(to_unix_nano(s.start_time));
    out += "\",\"endTimeUnixNano\":\"";
    out += std::to_string(to_unix_nano(s.end_time));
    out += "\",\"attributes\":[";

    bool first_attr = true;
    auto emit_attr = [&](std::string_view k, std::string_view v) {
      if (!first_attr) out += ',';
      first_attr = false;
      out += "{\"key\":\"";
      json_escape_to(out, k);
      out += "\",\"value\":{\"stringValue\":\"";
      json_escape_to(out, v);
      out += "\"}}";
    };
    if (!s.pipeline_name.empty()) emit_attr("qbuem.pipeline", s.pipeline_name);
    if (!s.action_name.empty())   emit_attr("qbuem.action", s.action_name);
    for (size_t i = 0; i < s.attribute_count; ++i)
      emit_attr(s.attributes[i].key, s.attributes[i].value);
    out += "]";

    out += ",\"status\":{\"code\":";
    out += std::to_string(status_code(s.status));
    if (s.status == SpanStatus::Error && !s.error_message.empty()) {
      out += ",\"message\":\"";
      json_escape_to(out, s.error_message);
      out += "\"";
    }
    out += "}}";
  }

  out += "]}]}]}"; // close spans, scopeSpan, scopeSpans, resourceSpan, resourceSpans, root
  return out;
}

/**
 * @brief OTLP/HTTP span exporter with a background flush thread + injected transport.
 *
 * Thread-safe: `export_span()` may be called concurrently; transport calls are
 * serialized. `flush()` drains and sends synchronously on the caller; the worker
 * thread also flushes every `flush_interval`. `shutdown()` (also run by the
 * destructor) stops the worker and performs a final flush.
 */
class OtlpHttpExporter final : public SpanExporter {
public:
  /** @brief Delivers a finished OTLP/JSON body to the collector. */
  using Transport = std::function<void(std::string_view json_body)>;

  struct Config {
    std::string               service_name   = "qbuem-service";
    std::chrono::milliseconds flush_interval = std::chrono::milliseconds{1000};
    size_t                    max_queue      = 4096; ///< Bound; spans beyond are dropped.
    size_t                    max_batch      = 512;  ///< Spans per OTLP request.
  };

  explicit OtlpHttpExporter(Transport transport)
      : OtlpHttpExporter(std::move(transport), Config{}) {}

  OtlpHttpExporter(Transport transport, Config cfg)
      : transport_(std::move(transport)), cfg_(std::move(cfg)) {
    worker_ = std::jthread([this] { run_(); });
  }

  ~OtlpHttpExporter() override { shutdown(); }

  OtlpHttpExporter(const OtlpHttpExporter&) = delete;
  OtlpHttpExporter& operator=(const OtlpHttpExporter&) = delete;

  /** @brief Enqueue a completed span (drops if the queue is full). */
  void export_span(const SpanData& span) override {
    std::lock_guard lk(mtx_);
    if (queue_.size() < cfg_.max_queue) queue_.push_back(span);
    else dropped_.fetch_add(1, std::memory_order_relaxed);
  }

  /** @brief Drain the queue and send it now (synchronous, on the caller). */
  void flush() override { send_(drain_()); }

  /** @brief Stop the worker and perform a final flush. Idempotent. */
  void shutdown() override {
    bool was;
    {
      // Set the stop flag UNDER the mutex so the notify cannot be lost in the
      // window where the worker has checked the predicate but not yet entered
      // cv_.wait_for() — otherwise the worker could sleep the full flush_interval
      // and join() would hang (caught by CI ASan as a timeout).
      std::lock_guard lk(mtx_);
      was = stop_.exchange(true);
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (!was) flush(); // final drain once
  }

  /** @brief Number of spans dropped because the queue was full. */
  [[nodiscard]] uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

private:
  void run_() {
    while (!stop_.load()) {
      {
        std::unique_lock lk(mtx_);
        cv_.wait_for(lk, cfg_.flush_interval, [this] { return stop_.load(); });
      }
      flush();
    }
  }

  std::vector<SpanData> drain_() {
    std::lock_guard lk(mtx_);
    std::vector<SpanData> batch;
    batch.swap(queue_);
    return batch;
  }

  void send_(const std::vector<SpanData>& batch) {
    if (batch.empty() || !transport_) return;
    std::lock_guard lk(send_mtx_); // serialize transport calls
    const size_t step = cfg_.max_batch > 0 ? cfg_.max_batch : batch.size();
    for (size_t i = 0; i < batch.size(); i += step) {
      const size_t n = std::min(step, batch.size() - i);
      transport_(encode_otlp_traces_json(
          std::span<const SpanData>(batch.data() + i, n), cfg_.service_name));
    }
  }

  Transport                 transport_;
  Config                    cfg_;
  std::mutex                mtx_;       // guards queue_
  std::condition_variable   cv_;
  std::vector<SpanData>     queue_;
  std::mutex                send_mtx_;  // serializes transport_ calls
  std::atomic<bool>         stop_{false};
  std::atomic<uint64_t>     dropped_{0};
  std::jthread              worker_;
};

} // namespace qbuem::tracing
