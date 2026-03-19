#pragma once

/**
 * @file qbuem/tracing/exporter.hpp
 * @brief Span exporter, pipeline tracer, and metrics exporter definitions.
 * @defgroup qbuem_tracing_exporter Exporter
 * @ingroup qbuem_tracing
 *
 * This header completes the export pipeline for the distributed tracing system:
 *
 * - `SpanExporter`          : Pure virtual interface for consuming span data
 * - `NoopSpanExporter`      : Span exporter that does nothing
 * - `LoggingSpanExporter`   : Exporter that prints human-readable output to stderr
 * - `Tracer`                : Base tracer used by `Span` (implements the forward declaration in span.hpp)
 * - `PipelineTracer`        : Global singleton tracer with injectable `SpanExporter`
 * - `IMetricsExporter`      : Abstract interface for Prometheus push metrics
 * - `PrometheusTextExporter`: In-memory Prometheus text format generator
 * - `TraceContextSlot`      : Slot type for storing `TraceContext` in `qbuem::Context`
 *
 * ## Usage example
 * @code
 * // Configure a logging exporter on the global tracer
 * auto tracer = std::make_unique<qbuem::tracing::PipelineTracer>();
 * tracer->set_exporter(std::make_shared<qbuem::tracing::LoggingSpanExporter>());
 * qbuem::tracing::PipelineTracer::set_global_tracer(std::move(tracer));
 *
 * // Create a span
 * auto& pt = qbuem::tracing::PipelineTracer::global();
 * auto span = pt.start_span("process", "ingest", "parse");
 * span.set_status(qbuem::tracing::SpanStatus::Ok);
 * // Automatically exported when the scope ends
 * @endcode
 * @{
 */

#include <qbuem/tracing/span.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>

// ─── SpanExporter ─────────────────────────────────────────────────────────────

namespace qbuem::tracing {

/**
 * @brief Pure virtual interface for consuming span data.
 *
 * Implementations receive data via `export_span()` when a span completes.
 * By default, `flush()` and `shutdown()` are no-ops.
 *
 * ### Implementation contract
 * - `export_span()` may be called concurrently from multiple threads.
 *   Implementations must synchronize internally.
 * - `export_span()` must not be called after `shutdown()`.
 */
class SpanExporter {
public:
  virtual ~SpanExporter() = default;

  /**
   * @brief Export a completed span's data.
   *
   * @param span Span metadata to export.
   */
  virtual void export_span(const SpanData& span) = 0;

  /**
   * @brief Force-flush any buffered span data.
   *
   * Default implementation is a no-op.
   */
  virtual void flush() {}

  /**
   * @brief Shut down the exporter.
   *
   * Any remaining data should be processed before shutdown.
   * Default implementation is a no-op.
   */
  virtual void shutdown() {}
};

// ─── NoopSpanExporter ─────────────────────────────────────────────────────────

/**
 * @brief Span exporter that silently discards all data.
 *
 * Use in environments where tracing is not needed, or in tests.
 * All span data is immediately discarded.
 */
class NoopSpanExporter final : public SpanExporter {
public:
  /**
   * @brief Silently discard the span.
   * @param span Span data to ignore.
   */
  void export_span(const SpanData& /*span*/) override {}
};

// ─── LoggingSpanExporter ──────────────────────────────────────────────────────

/**
 * @brief Exporter that prints completed spans to stderr in a human-readable format.
 *
 * Intended for development and debugging. Each span is printed on a single line.
 *
 * ### Output format
 * ```
 * [SPAN] <name> pipeline=<pipeline> action=<action>
 *        trace=<trace_id> span=<span_id> parent=<parent_span_id>
 *        status=<Ok|Error|Unset> duration=<ms>ms
 *        [error: <message>]
 *        [attrs: key=value ...]
 * ```
 *
 * `export_span()` is thread-safe (uses an internal mutex).
 */
class LoggingSpanExporter final : public SpanExporter {
public:
  /**
   * @brief Print span data to stderr.
   *
   * @param span Span metadata to print.
   */
  void export_span(const SpanData& span) override {
    // TraceId → hex string
    char trace_buf[33];
    span.trace_id.to_chars(trace_buf, sizeof(trace_buf));

    // SpanId → hex string
    char span_buf[17];
    span.span_id.to_chars(span_buf, sizeof(span_buf));

    // parent SpanId → hex string
    char parent_buf[17];
    span.parent_span_id.to_chars(parent_buf, sizeof(parent_buf));

    // Calculate duration (milliseconds)
    const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
        span.end_time - span.start_time).count();
    const double duration_ms = static_cast<double>(duration_us) / 1000.0;

    // Status string
    const char* status_str = "Unset";
    switch (span.status) {
      case SpanStatus::Ok:    status_str = "Ok";    break;
      case SpanStatus::Error: status_str = "Error"; break;
      case SpanStatus::Unset: status_str = "Unset"; break;
    }

    // Assemble output string.
    std::string out = std::format(
        "[SPAN] {} pipeline={} action={}\n"
        "       trace={} span={} parent={}\n"
        "       status={} duration={:.3f}ms",
        span.name, span.pipeline_name, span.action_name,
        trace_buf, span_buf, parent_buf,
        status_str, duration_ms);

    if (span.status == SpanStatus::Error && !span.error_message.empty()) {
      out += std::format("\n       error: {}", span.error_message);
    }

    if (span.attribute_count > 0) {
      out += "\n       attrs:";
      for (size_t i = 0; i < span.attribute_count; ++i) {
        out += std::format(" {}={}", span.attributes[i].key, span.attributes[i].value);
      }
    }

    out += '\n';

    {
      std::lock_guard<std::mutex> lk(mtx_);
      std::print(stderr, "{}", out);
    }
  }

private:
  mutable std::mutex mtx_; ///< Mutex for synchronizing stderr output
};

// ─── Tracer ───────────────────────────────────────────────────────────────────

/**
 * @brief Base tracer called by the `Span` RAII destructor.
 *
 * Concrete implementation of the `Tracer` class forward-declared in `span.hpp`.
 * Forwards completed spans to the outside via `SpanExporter`.
 *
 * Prefer using `PipelineTracer` over using this class directly.
 */
class Tracer {
public:
  /**
   * @brief Default constructor — uses `NoopSpanExporter`.
   */
  Tracer() : exporter_(std::make_shared<NoopSpanExporter>()) {}

  /**
   * @brief Constructs a Tracer with a specific exporter.
   *
   * @param exporter Exporter to call when a span completes. Falls back to Noop if nullptr.
   */
  explicit Tracer(std::shared_ptr<SpanExporter> exporter)
      : exporter_(exporter ? std::move(exporter)
                           : std::make_shared<NoopSpanExporter>()) {}

  /**
   * @brief Forward a completed span to the exporter.
   *
   * Called automatically by the `Span` destructor.
   *
   * @param span Completed span data.
   */
  void export_span(SpanData span) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (exporter_) {
      exporter_->export_span(span);
    }
  }

  /**
   * @brief Start a new span.
   *
   * @param name          Operation name.
   * @param pipeline_name Pipeline name.
   * @param action_name   Action name.
   * @param parent        Parent TraceContext (default: empty context → creates root span).
   * @returns RAII span object. Automatically exported upon destruction.
   */
  Span start_span(std::string_view name,
                  std::string_view pipeline_name,
                  std::string_view action_name,
                  TraceContext parent = {}) {
    SpanData data;

    if (parent.trace_id.is_valid()) {
      // Valid parent context — create as child span
      data.trace_id       = parent.trace_id;
      data.parent_span_id = parent.parent_span_id;
      data.span_id        = SpanId::generate();
    } else {
      // Root span: generate new TraceId and SpanId
      data.trace_id       = TraceId::generate();
      data.span_id        = SpanId::generate();
      // parent_span_id remains at default (invalid) value
    }

    data.name          = std::string(name);
    data.pipeline_name = std::string(pipeline_name);
    data.action_name   = std::string(action_name);
    data.start_time    = std::chrono::system_clock::now();

    return Span(std::move(data), this);
  }

  /**
   * @brief Replace the current exporter.
   *
   * @param exporter New exporter. Falls back to Noop if nullptr.
   */
  void set_exporter(std::shared_ptr<SpanExporter> exporter) {
    std::lock_guard<std::mutex> lk(mtx_);
    exporter_ = exporter ? std::move(exporter)
                         : std::make_shared<NoopSpanExporter>();
  }

private:
  std::shared_ptr<SpanExporter> exporter_; ///< Exporter that consumes span data
  std::mutex                    mtx_;      ///< Mutex for export synchronization
};

// ─── Span destructor definition ───────────────────────────────────────────────
// Tracer is only forward-declared in span.hpp, so Span::~Span() is defined
// here where Tracer is fully defined.

/**
 * @brief Span RAII destructor — records end_time and exports to the Tracer.
 *
 * Runs only when `ended_` is false, preventing double exports.
 */
inline Span::~Span() {
  if (!ended_ && tracer_) {
    data_.end_time = std::chrono::system_clock::now();
    tracer_->export_span(std::move(data_));
    ended_ = true;
  }
}

// ─── PipelineTracer ───────────────────────────────────────────────────────────

/**
 * @brief Global singleton tracer with injectable `SpanExporter`.
 *
 * `PipelineTracer` wraps `Tracer` to provide a global singleton pattern.
 * Configure it at application startup with `set_global_tracer()`,
 * then access it from anywhere via `global()`.
 *
 * ### Thread safety
 * - `global()` : Thread-safe (guaranteed by atomic initialization).
 * - `set_global_tracer()` : Should be called only once at program startup.
 * - `start_span()` / `end_span()` : Thread-safe (uses internal mutex).
 * - `set_exporter()` : Thread-safe.
 *
 * ### Usage example
 * @code
 * // Application initialization
 * auto pt = std::make_unique<PipelineTracer>();
 * pt->set_exporter(std::make_shared<LoggingSpanExporter>());
 * PipelineTracer::set_global_tracer(std::move(pt));
 *
 * // Create a span
 * auto span = PipelineTracer::global().start_span("op", "pipeline", "action");
 * span.set_status(SpanStatus::Ok);
 * @endcode
 */
class PipelineTracer {
public:
  /**
   * @brief Default constructor — uses `NoopSpanExporter`.
   */
  PipelineTracer() : tracer_(std::make_shared<NoopSpanExporter>()) {}

  /**
   * @brief Access the global `PipelineTracer` instance.
   *
   * If the global tracer has not been set with `set_global_tracer()`,
   * a tracer using the default `NoopSpanExporter` is returned.
   *
   * @returns Reference to the global PipelineTracer.
   */
  static PipelineTracer& global() {
    static PipelineTracer default_instance;
    PipelineTracer* ptr = s_global_.load(std::memory_order_acquire);
    return ptr ? *ptr : default_instance;
  }

  /**
   * @brief Replace the global `PipelineTracer`.
   *
   * The previous global tracer is destroyed.
   * Recommended to call only once at program startup.
   *
   * @param tracer New global tracer. Restores the default instance if nullptr.
   */
  static void set_global_tracer(std::unique_ptr<PipelineTracer> tracer) {
    std::lock_guard<std::mutex> lk(s_global_mtx_);
    delete s_global_.exchange(tracer.release(), std::memory_order_acq_rel);
  }

  /**
   * @brief Start a new span.
   *
   * Creates a child span if a valid parent context is provided, otherwise creates a root span.
   *
   * @param name          Operation name.
   * @param pipeline_name Pipeline name.
   * @param action_name   Action name.
   * @param parent        Parent TraceContext (default: creates root span).
   * @returns RAII span object. Automatically calls `end_span()` upon destruction.
   */
  Span start_span(std::string_view name,
                  std::string_view pipeline_name,
                  std::string_view action_name,
                  TraceContext parent = {}) {
    std::lock_guard<std::mutex> lk(mtx_);
    return tracer_.start_span(name, pipeline_name, action_name, parent);
  }

  /**
   * @brief Forward completed span data to the exporter.
   *
   * Called internally by the `Span` RAII destructor.
   * Does not need to be called directly.
   *
   * @param span_data Completed span metadata.
   */
  void end_span(SpanData span_data) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::shared_ptr<SpanExporter> exp = exporter_;
    if (exp) {
      exp->export_span(span_data);
    }
  }

  /**
   * @brief Replace the exporter.
   *
   * The new exporter applies to all spans going forward.
   * Falls back to `NoopSpanExporter` if nullptr.
   *
   * @param exporter New span exporter.
   */
  void set_exporter(std::shared_ptr<SpanExporter> exporter) {
    auto new_exp = exporter ? std::move(exporter)
                            : std::make_shared<NoopSpanExporter>();
    {
      std::lock_guard<std::mutex> lk(mtx_);
      exporter_ = new_exp;
      tracer_.set_exporter(new_exp);
    }
  }

private:
  Tracer                        tracer_;   ///< Internal Tracer (handles span creation and export)
  std::shared_ptr<SpanExporter> exporter_; ///< Currently configured exporter (kept for reference)
  std::mutex                    mtx_;      ///< Mutex for thread-safe access

  static std::atomic<PipelineTracer*> s_global_;  ///< Global singleton pointer
  static std::mutex                   s_global_mtx_; ///< Mutex for replacing the global pointer
};

// Static member definitions
inline std::atomic<PipelineTracer*> PipelineTracer::s_global_{nullptr};
inline std::mutex                   PipelineTracer::s_global_mtx_{};

// ─── IMetricsExporter ─────────────────────────────────────────────────────────

/**
 * @brief Abstract interface for Prometheus push metrics.
 *
 * Supports three metric types: Gauge, Counter, and Histogram.
 * Prometheus labels are passed as strings.
 *
 * ### Label format
 * Prometheus label format: `key="value",key2="value2"`
 *
 * ### Implementation contract
 * - All methods must be thread-safe.
 * - After calling `flush()`, all metrics must be guaranteed to have been forwarded externally.
 */
class IMetricsExporter {
public:
  virtual ~IMetricsExporter() = default;

  /**
   * @brief Set a Gauge metric (absolute value).
   *
   * Used for metrics representing current state (e.g. current queue depth, memory usage).
   *
   * @param name   Metric name (Prometheus naming convention: snake_case).
   * @param value  Current gauge value.
   * @param labels Prometheus label string (e.g. `job="worker",env="prod"`).
   */
  virtual void gauge(std::string_view name,
                     double value,
                     std::string_view labels = "") = 0;

  /**
   * @brief Increment a Counter metric (delta).
   *
   * Used for monotonically increasing cumulative metrics (e.g. number of processed messages).
   *
   * @param name   Metric name.
   * @param delta  Increment amount (negative values are not recommended).
   * @param labels Prometheus label string.
   */
  virtual void counter(std::string_view name,
                       double delta,
                       std::string_view labels = "") = 0;

  /**
   * @brief Record an observation in a Histogram.
   *
   * Used for metrics tracking distributions (e.g. request latency, payload size).
   *
   * @param name   Metric name.
   * @param value  Observed value.
   * @param labels Prometheus label string.
   */
  virtual void histogram(std::string_view name,
                         double value,
                         std::string_view labels = "") = 0;

  /**
   * @brief Force-flush any buffered metrics.
   *
   * Default implementation is a no-op.
   */
  virtual void flush() {}
};

// ─── PrometheusTextExporter ───────────────────────────────────────────────────

/**
 * @brief In-memory Prometheus text format metrics generator.
 *
 * Accumulates metric data in memory and returns it in Prometheus
 * text exposition format via `export_text()`.
 *
 * ### Prometheus text format output example
 * ```
 * # TYPE queue_depth gauge
 * queue_depth{job="worker"} 42.000000
 * # TYPE messages_processed counter
 * messages_processed_total{env="prod"} 1024.000000
 * # TYPE request_latency_ms histogram
 * request_latency_ms_sum{} 12345.678000
 * request_latency_ms_count{} 100
 * ```
 *
 * ### Thread safety
 * All methods are protected by an internal mutex.
 */
class PrometheusTextExporter final : public IMetricsExporter {
public:
  /**
   * @brief Record a Gauge metric to the buffer.
   *
   * @param name   Metric name.
   * @param value  Current gauge value.
   * @param labels Prometheus label string.
   */
  void gauge(std::string_view name,
             double value,
             std::string_view labels = "") override {
    std::lock_guard<std::mutex> lk(mtx_);
    append_metric("gauge", name, value, labels);
  }

  /**
   * @brief Accumulate a Counter metric in the buffer.
   *
   * Counters with the same (name, labels) combination are accumulated.
   *
   * @param name   Metric name.
   * @param delta  Increment amount.
   * @param labels Prometheus label string.
   */
  void counter(std::string_view name,
               double delta,
               std::string_view labels = "") override {
    std::lock_guard<std::mutex> lk(mtx_);
    const std::string key = make_key(name, labels);
    counter_values_[key] += delta;
    counter_meta_[key]    = {std::string(name), std::string(labels)};
  }

  /**
   * @brief Record an observation in a Histogram.
   *
   * Accumulates sum and count.
   *
   * @param name   Metric name.
   * @param value  Observed value.
   * @param labels Prometheus label string.
   */
  void histogram(std::string_view name,
                 double value,
                 std::string_view labels = "") override {
    std::lock_guard<std::mutex> lk(mtx_);
    const std::string key = make_key(name, labels);
    histogram_sum_[key]   += value;
    histogram_count_[key] += 1;
    histogram_meta_[key]   = {std::string(name), std::string(labels)};
  }

  /**
   * @brief Return all accumulated metrics in Prometheus text format.
   *
   * The internal buffer is cleared after returning.
   *
   * @returns Prometheus exposition text format string.
   */
  [[nodiscard]] std::string export_text() {
    std::lock_guard<std::mutex> lk(mtx_);

    std::string out;
    out.reserve(raw_buffer_.size() + counter_values_.size() * 80
                                   + histogram_sum_.size() * 120);

    // Gauge / raw buffer
    out += raw_buffer_;
    raw_buffer_.clear();

    // Counter output
    for (const auto& [key, total] : counter_values_) {
      const auto& [metric_name, metric_labels] = counter_meta_.at(key);
      out += "# TYPE ";
      out += metric_name;
      out += "_total counter\n";
      out += metric_name;
      out += "_total";
      append_labels(out, metric_labels);
      out += ' ';
      out += std::to_string(total);
      out += '\n';
    }
    counter_values_.clear();
    counter_meta_.clear();

    // Histogram output
    for (const auto& [key, sum] : histogram_sum_) {
      const auto& [metric_name, metric_labels] = histogram_meta_.at(key);
      const double count = histogram_count_.at(key);

      out += "# TYPE ";
      out += metric_name;
      out += " histogram\n";

      out += metric_name;
      out += "_sum";
      append_labels(out, metric_labels);
      out += ' ';
      out += std::to_string(sum);
      out += '\n';

      out += metric_name;
      out += "_count";
      append_labels(out, metric_labels);
      out += ' ';
      out += std::to_string(static_cast<uint64_t>(count));
      out += '\n';
    }
    histogram_sum_.clear();
    histogram_count_.clear();
    histogram_meta_.clear();

    return out;
  }

  /**
   * @brief Clear the internal buffer (reset metrics).
   *
   * Overrides `IMetricsExporter::flush()`. Clears the buffer to free memory.
   */
  void flush() override {
    std::lock_guard<std::mutex> lk(mtx_);
    raw_buffer_.clear();
    counter_values_.clear();
    counter_meta_.clear();
    histogram_sum_.clear();
    histogram_count_.clear();
    histogram_meta_.clear();
  }

private:
  /**
   * @brief Immediately record a Gauge metric to the raw buffer (internal helper).
   *
   * @param type   Prometheus metric type string.
   * @param name   Metric name.
   * @param value  Value.
   * @param labels Label string.
   */
  void append_metric(std::string_view type,
                     std::string_view name,
                     double value,
                     std::string_view labels) {
    raw_buffer_ += "# TYPE ";
    raw_buffer_ += name;
    raw_buffer_ += ' ';
    raw_buffer_ += type;
    raw_buffer_ += '\n';
    raw_buffer_ += name;
    append_labels(raw_buffer_, labels);
    raw_buffer_ += ' ';
    raw_buffer_ += std::to_string(value);
    raw_buffer_ += '\n';
  }

  /**
   * @brief Append a label string in Prometheus format to the output string.
   *
   * Appends `{}` if labels is empty.
   *
   * @param out    Target string.
   * @param labels Label string (e.g. `job="worker"`).
   */
  static void append_labels(std::string& out, std::string_view labels) {
    out += '{';
    if (!labels.empty()) {
      out += labels;
    }
    out += '}';
  }

  /**
   * @brief Convert a (name, labels) combination to a unique key string.
   *
   * @param name   Metric name.
   * @param labels Label string.
   * @returns Composite key string.
   */
  static std::string make_key(std::string_view name, std::string_view labels) {
    std::string key;
    key.reserve(name.size() + 1 + labels.size());
    key += name;
    key += '\x1F'; // ASCII unit separator
    key += labels;
    return key;
  }

  std::mutex  mtx_;         ///< Mutex for thread-safe access

  /// @brief Buffer for Gauge and immediately serialized metrics
  std::string raw_buffer_;

  /// @brief Counter accumulated values (key → sum)
  std::unordered_map<std::string, double> counter_values_;

  /// @brief Counter metadata (key → {name, labels})
  std::unordered_map<std::string, std::pair<std::string, std::string>> counter_meta_;

  /// @brief Histogram sum accumulated values (key → sum)
  std::unordered_map<std::string, double> histogram_sum_;

  /// @brief Histogram count accumulated values (key → count)
  std::unordered_map<std::string, double> histogram_count_;

  /// @brief Histogram metadata (key → {name, labels})
  std::unordered_map<std::string, std::pair<std::string, std::string>> histogram_meta_;
};

} // namespace qbuem::tracing

// ─── TraceContextSlot ─────────────────────────────────────────────────────────

namespace qbuem {

/**
 * @brief Slot type for storing `TraceContext` in `qbuem::Context`.
 *
 * Store a TraceContext in a context with `qbuem::Context::put<TraceContextSlot>()`,
 * and retrieve it with `qbuem::Context::get<TraceContextSlot>()`.
 *
 * ### Usage example
 * @code
 * qbuem::Context ctx;
 * auto trace_ctx = qbuem::tracing::TraceContext::generate();
 * ctx = ctx.put(qbuem::TraceContextSlot{trace_ctx});
 *
 * if (auto slot = ctx.get<qbuem::TraceContextSlot>()) {
 *     auto child = slot->value.child_span();
 * }
 * @endcode
 *
 * @note `qbuem::TraceCtx` uses raw byte arrays, while
 *       `TraceContextSlot` directly holds a type-safe `tracing::TraceContext`.
 */
struct TraceContextSlot {
  tracing::TraceContext value; ///< Trace context (TraceId, SpanId, flags)
};

} // namespace qbuem

/** @} */
