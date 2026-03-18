#pragma once

/**
 * @file qbuem/tracing/span.hpp
 * @brief Span — represents a single unit of work. SpanData, Span (RAII wrapper).
 * @defgroup qbuem_tracing_span Span
 * @ingroup qbuem_tracing
 *
 * In distributed tracing, a Span represents a single operation unit.
 * `SpanData` stores the Span's metadata, and `Span` uses the RAII pattern
 * to automatically export to the exporter upon destruction.
 *
 * ## Features
 * - `SpanData::attributes` uses a fixed-size array without heap allocation.
 * - The `Span` destructor records `end_time` and exports to the tracer.
 * - If `Tracer` is nullptr, the export is skipped (noop).
 *
 * ## Usage example
 * @code
 * auto span = tracer.start_span("process_message", "ingest", "parse");
 * span.set_attribute("queue", "orders");
 * span.set_attribute("message_id", msg_id);
 * // ... perform work ...
 * span.set_status(SpanStatus::Ok);
 * // Automatically exported when the scope ends
 * @endcode
 * @{
 */

#include <qbuem/tracing/trace_context.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace qbuem::tracing {

// Forward declaration
class Tracer;

// ─── SpanStatus ───────────────────────────────────────────────────────────────

/**
 * @brief Completion status of a span.
 *
 * Follows the OpenTelemetry SpanStatus specification.
 * - `Unset`  : Not explicitly set (default value).
 * - `Ok`     : The operation completed successfully.
 * - `Error`  : An error occurred during the operation (details in `error_message`).
 */
enum class SpanStatus {
  Ok,     ///< Operation succeeded
  Error,  ///< Operation failed (see error_message)
  Unset,  ///< Status not set (default value)
};

// ─── SpanData ─────────────────────────────────────────────────────────────────

/**
 * @brief Span metadata container.
 *
 * Uses a fixed-size array for attributes to minimize heap allocation.
 * Attributes exceeding `kMaxAttributes` (16) are silently ignored.
 *
 * ### Lifetime
 * `SpanData` is a value type. When `Span` is destroyed, `end_time` is set
 * and a copy is passed to `Tracer::export_span()`.
 */
struct SpanData {
  TraceId  trace_id;       ///< 128-bit global trace identifier
  SpanId   span_id;        ///< 64-bit identifier for this span
  SpanId   parent_span_id; ///< 64-bit identifier of the parent span (invalid if root)

  std::string name;          ///< Operation name (e.g. "process_message")
  std::string pipeline_name; ///< Name of the pipeline where the span was created
  std::string action_name;   ///< Name of the action where the span was created

  /** @brief Span start time (UTC). */
  std::chrono::system_clock::time_point start_time;
  /** @brief Span end time (UTC). Set when Span is destroyed. */
  std::chrono::system_clock::time_point end_time;

  SpanStatus  status        = SpanStatus::Unset; ///< Completion status
  std::string error_message;                     ///< Error message (when status == Error)

  // ── Attributes (key-value) ─────────────────────────────────────────────────

  /** @brief Maximum number of attributes. set_attribute() is ignored when exceeded. */
  static constexpr size_t kMaxAttributes = 16;

  /**
   * @brief A single key-value attribute.
   *
   * Both members use std::string.
   * An empty key represents an invalid (unused) slot.
   */
  struct Attribute {
    std::string key;   ///< Attribute key
    std::string value; ///< Attribute value
  };

  /** @brief Attribute array (fixed-size, no heap allocation). */
  Attribute attributes[kMaxAttributes];

  /** @brief Number of attributes currently stored. */
  size_t attribute_count = 0;

  /**
   * @brief Add an attribute.
   *
   * If `kMaxAttributes` is reached, additional attributes are silently ignored.
   * If the same key already exists, the value is overwritten.
   *
   * @param key   Attribute key (ignored if empty).
   * @param value Attribute value.
   */
  void set_attribute(std::string_view key, std::string_view value) {
    if (key.empty()) return;

    // Search for existing key — overwrite
    for (size_t i = 0; i < attribute_count; ++i) {
      if (attributes[i].key == key) {
        attributes[i].value = std::string(value);
        return;
      }
    }

    // New slot
    if (attribute_count >= kMaxAttributes) return;
    attributes[attribute_count].key   = std::string(key);
    attributes[attribute_count].value = std::string(value);
    ++attribute_count;
  }
};

// ─── Span ─────────────────────────────────────────────────────────────────────

/**
 * @brief Active span — RAII wrapper that automatically exports upon destruction.
 *
 * `Span` is a move-only type. Copy constructor and copy assignment operator are deleted.
 * When the destructor is called, it records `end_time` and calls `tracer_->export_span(data_)`.
 *
 * If `tracer_` is nullptr, the export is skipped (noop behavior).
 *
 * ### Double-export prevention
 * The `ended_` flag prevents duplicate exports in the destructor.
 */
class Span {
public:
  /**
   * @brief Constructs a Span from SpanData and a Tracer pointer.
   *
   * @param data   Span metadata. Moved in.
   * @param tracer Tracer pointer responsible for exporting (non-owning, nullable).
   */
  Span(SpanData data, Tracer* tracer)
      : data_(std::move(data)), tracer_(tracer), ended_(false) {}

  /**
   * @brief Ends and exports the span upon destruction.
   *
   * If `ended_` is false, sets `end_time` to the current time and calls
   * `tracer_->export_span(data_)`.
   */
  ~Span();

  // Move-only, not copyable
  Span(Span&& other) noexcept
      : data_(std::move(other.data_)), tracer_(other.tracer_), ended_(other.ended_) {
    other.ended_ = true; // Prevent double-export when the original is destroyed
  }
  Span& operator=(Span&& other) noexcept {
    if (this != &other) {
      if (!ended_ && tracer_) {
        data_.end_time = std::chrono::system_clock::now();
        // Explicit flush before move-assignment is the caller's responsibility if needed
      }
      data_        = std::move(other.data_);
      tracer_      = other.tracer_;
      ended_       = other.ended_;
      other.ended_ = true;
    }
    return *this;
  }

  Span(const Span&)            = delete;
  Span& operator=(const Span&) = delete;

  /**
   * @brief Set the completion status of the span.
   *
   * @param s   Completion status (`SpanStatus::Ok` or `SpanStatus::Error`).
   * @param msg Error message (meaningful when status == Error).
   */
  void set_status(SpanStatus s, std::string_view msg = {}) {
    data_.status = s;
    if (!msg.empty())
      data_.error_message = std::string(msg);
  }

  /**
   * @brief Add an attribute.
   *
   * Calls `SpanData::set_attribute()` internally.
   *
   * @param key   Attribute key.
   * @param value Attribute value.
   */
  void set_attribute(std::string_view key, std::string_view value) {
    data_.set_attribute(key, value);
  }

  /**
   * @brief Returns a const reference to the SpanData.
   * @returns Metadata for this span.
   */
  const SpanData& data() const noexcept { return data_; }

private:
  SpanData data_;    ///< Span metadata
  Tracer*  tracer_;  ///< Tracer responsible for exporting (non-owning)
  bool     ended_;   ///< Flag to prevent double-export
};

} // namespace qbuem::tracing

/** @} */
