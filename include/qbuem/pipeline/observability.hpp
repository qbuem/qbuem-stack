#pragma once

/**
 * @file qbuem/pipeline/observability.hpp
 * @brief Pipeline observability — ActionMetrics, PipelineMetrics, PipelineObserver
 * @defgroup qbuem_observability Pipeline Observability
 * @ingroup qbuem_pipeline
 *
 * This header provides metric collection and event hook infrastructure for pipelines:
 *
 * - `ActionMetrics`    : Cache-line-aligned atomic counters + latency histogram
 * - `PipelineMetrics`  : Aggregate metrics for an entire pipeline
 * - `PipelineObserver` : Event hook interface
 * - `LoggingObserver`  : Standard output logging implementation
 * - `NoopObserver`     : Zero-overhead inactive implementation
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <qbuem/compat/print.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace qbuem {

// ─── HistogramMetrics ─────────────────────────────────────────────────────────

/**
 * @brief Latency histogram with user-defined bucket boundaries.
 *
 * Can be used instead of the fixed 4-bucket histogram in `ActionMetrics`,
 * and is compatible with the Prometheus-style cumulative histogram structure.
 *
 * ### Usage example
 * @code
 * // 5-bucket histogram optimized for p50/p90/p99/p999
 * HistogramMetrics hist({500, 1000, 5000, 10000, 50000}); // in µs
 * hist.observe(latency_us);
 * auto counts = hist.bucket_counts(); // cumulative count per bucket
 * @endcode
 *
 * @note Thread-safe: `observe()` is implemented with `std::atomic` relaxed operations.
 */
class HistogramMetrics {
public:
  /**
   * @brief Constructs a histogram with user-defined upper boundaries (in µs).
   *
   * @param upper_bounds List of bucket upper bounds in µs (sorted in ascending order).
   *                     The last bucket captures all observations >= the maximum value.
   *
   * Example: `{1000, 10000, 100000}` → buckets: [0,1ms), [1ms,10ms), [10ms,100ms), [100ms,∞)
   */
  explicit HistogramMetrics(std::initializer_list<uint64_t> upper_bounds)
      : bounds_(upper_bounds), buckets_(upper_bounds.size() + 1) {
    for (auto &b : buckets_) b.val.store(0, std::memory_order_relaxed);
  }

  explicit HistogramMetrics(std::vector<uint64_t> upper_bounds)
      : bounds_(std::move(upper_bounds)), buckets_(bounds_.size() + 1) {
    for (auto &b : buckets_) b.val.store(0, std::memory_order_relaxed);
  }

  /**
   * @brief Records an observation into the histogram.
   *
   * @param us Latency in microseconds.
   */
  void observe(uint64_t us) noexcept {
    // Binary search for the first bucket whose upper bound >= us.
    auto it = std::lower_bound(bounds_.begin(), bounds_.end(), us);
    size_t idx = static_cast<size_t>(it - bounds_.begin());
    buckets_[idx].val.fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief Returns a snapshot of each bucket's count.
   *
   * @returns Vector of bucket counts. Size = `upper_bounds.size() + 1`.
   */
  [[nodiscard]] std::vector<uint64_t> bucket_counts() const noexcept {
    std::vector<uint64_t> counts(buckets_.size());
    for (size_t i = 0; i < buckets_.size(); ++i)
      counts[i] = buckets_[i].val.load(std::memory_order_relaxed);
    return counts;
  }

  /**
   * @brief Writes bucket counts directly into a caller-provided array (no heap allocation).
   *
   * Use this to read bucket values on a hot path (e.g., snapshot generation) without
   * vector allocation.
   *
   * @param out Pointer to the output array. Must have size >= bucket_count().
   * @param n   Array size (excess entries are ignored).
   * @returns The number of buckets actually written.
   */
  size_t fill_bucket_counts(uint64_t* out, size_t n) const noexcept {
    size_t cnt = std::min(n, buckets_.size());
    for (size_t i = 0; i < cnt; ++i)
      out[i] = buckets_[i].val.load(std::memory_order_relaxed);
    return cnt;
  }

  /** @brief Number of buckets = upper_bounds.size() + 1. */
  [[nodiscard]] size_t bucket_count() const noexcept { return buckets_.size(); }

  /** @brief Returns the list of bucket upper boundaries. */
  [[nodiscard]] const std::vector<uint64_t> &upper_bounds() const noexcept {
    return bounds_;
  }

  /** @brief Resets all counters to zero. */
  void reset() noexcept {
    for (auto &b : buckets_) b.val.store(0, std::memory_order_relaxed);
  }

private:
  /// @brief Cache-line-aligned bucket — prevents false sharing between adjacent buckets.
  struct alignas(64) Bucket {
    std::atomic<uint64_t> val{0};
  };

  std::vector<uint64_t> bounds_;  ///< Bucket upper boundaries (µs)
  std::vector<Bucket>   buckets_; ///< Per-bucket counters (64-byte padding)
};

// ─── ActionMetrics ────────────────────────────────────────────────────────────

/**
 * @brief Performance metrics for a single action (cache-line aligned).
 *
 * `alignas(64)` prevents false sharing.
 *
 * ### Latency buckets
 * - Bucket 0: < 1ms
 * - Bucket 1: 1ms ~ < 10ms
 * - Bucket 2: 10ms ~ < 100ms
 * - Bucket 3: >= 100ms
 */
struct alignas(64) ActionMetrics {
  /** @brief Number of items successfully processed. */
  std::atomic<uint64_t> items_processed{0};
  /** @brief Number of errors encountered during processing. */
  std::atomic<uint64_t> errors{0};
  /** @brief Number of retries performed. */
  std::atomic<uint64_t> retried{0};
  /** @brief Number of items moved to the dead-letter queue (DLQ). */
  std::atomic<uint64_t> dlq_count{0};

  /**
   * @brief Latency histogram buckets (4 levels).
   *
   * - [0]: < 1,000 µs  (< 1ms)
   * - [1]: < 10,000 µs (< 10ms)
   * - [2]: < 100,000 µs (< 100ms)
   * - [3]: >= 100,000 µs (>= 100ms)
   */
  std::atomic<uint64_t> lat_buckets[4] = {}; // NOLINT(modernize-avoid-c-arrays)

  /**
   * @brief Optional user-defined bucket histogram.
   *
   * Can be used instead of or in addition to the default 4-bucket histogram.
   * Not used when `nullptr`.
   */
  std::shared_ptr<HistogramMetrics> histogram;

  /**
   * @brief Records latency into both the fixed buckets and the user-defined histogram.
   *
   * @param us Measured latency in microseconds.
   */
  void record_latency_us(uint64_t us) noexcept {
    // Fixed 4-bucket histogram (backward compatible).
    if (us < 1000u)
      lat_buckets[0].fetch_add(1, std::memory_order_relaxed);
    else if (us < 10000u)
      lat_buckets[1].fetch_add(1, std::memory_order_relaxed);
    else if (us < 100000u)
      lat_buckets[2].fetch_add(1, std::memory_order_relaxed);
    else
      lat_buckets[3].fetch_add(1, std::memory_order_relaxed);
    // User-configurable histogram (if attached).
    if (histogram) histogram->observe(us);
  }

  /**
   * @brief Resets all counters to zero.
   */
  void reset() noexcept {
    items_processed.store(0, std::memory_order_relaxed);
    errors.store(0, std::memory_order_relaxed);
    retried.store(0, std::memory_order_relaxed);
    dlq_count.store(0, std::memory_order_relaxed);
    for (auto &b : lat_buckets)
      b.store(0, std::memory_order_relaxed);
    if (histogram) histogram->reset();
  }
};

// ─── PipelineMetrics ──────────────────────────────────────────────────────────

/**
 * @brief Aggregate metrics for an entire pipeline.
 *
 * Holds the `ActionMetrics` for each action along with its name.
 */
struct PipelineMetrics {
  /** @brief Pipeline name. */
  std::string name;
  /** @brief Per-action metric list (action name, metric reference). */
  std::vector<std::pair<std::string, ActionMetrics>> actions;

  /**
   * @brief Returns the total number of processed items across all actions.
   */
  [[nodiscard]] uint64_t total_processed() const noexcept {
    uint64_t total = 0;
    for (const auto &[name, m] : actions)
      total += m.items_processed.load(std::memory_order_relaxed);
    return total;
  }

  /**
   * @brief Returns the total error count across all actions.
   */
  [[nodiscard]] uint64_t total_errors() const noexcept {
    uint64_t total = 0;
    for (const auto &[name, m] : actions)
      total += m.errors.load(std::memory_order_relaxed);
    return total;
  }

  /**
   * @brief Returns the error rate (0.0 if no items have been processed).
   *
   * @returns `total_errors() / total_processed()`. Returns 0.0 when processed count is 0.
   */
  [[nodiscard]] double error_rate() const noexcept {
    uint64_t processed = total_processed();
    if (processed == 0) return 0.0;
    return static_cast<double>(total_errors()) /
           static_cast<double>(processed);
  }
};

// ─── PipelineObserver ─────────────────────────────────────────────────────────

/**
 * @brief Pipeline event hook interface.
 *
 * All methods provide a default no-op implementation, so only the events
 * of interest need to be overridden.
 *
 * ### Thread safety
 * Hooks may be called concurrently from multiple worker threads.
 * Implementations must protect internal state appropriately.
 */
class PipelineObserver {
public:
  virtual ~PipelineObserver() = default;

  /**
   * @brief Called when item processing begins.
   *
   * @param action_name Name of the action being processed.
   * @param item_id     Unique identifier of the item.
   */
  virtual void on_item_start(std::string_view /*action_name*/,
                              uint64_t /*item_id*/) {}

  /**
   * @brief Called when item processing completes.
   *
   * @param action_name Name of the completed action.
   * @param item_id     Unique identifier of the item.
   * @param latency_us  Processing time in microseconds.
   */
  virtual void on_item_done(std::string_view /*action_name*/,
                             uint64_t /*item_id*/,
                             uint64_t /*latency_us*/) {}

  /**
   * @brief Called when an error occurs during processing.
   *
   * @param action_name Name of the action where the error occurred.
   * @param ec          Error code.
   */
  virtual void on_error(std::string_view /*action_name*/,
                         std::error_code /*ec*/) {}

  /**
   * @brief Called when a worker scale event occurs.
   *
   * @param action_name  Name of the action whose scale changed.
   * @param old_workers  Worker count before the change.
   * @param new_workers  Worker count after the change.
   */
  virtual void on_scale_event(std::string_view /*action_name*/,
                               size_t /*old_workers*/,
                               size_t /*new_workers*/) {}

  /**
   * @brief Called when the pipeline transitions between states.
   *
   * @param pipeline_name Name of the pipeline.
   * @param old_state     Previous state name.
   * @param new_state     New state name.
   */
  virtual void on_state_change(std::string_view /*pipeline_name*/,
                                std::string_view /*old_state*/,
                                std::string_view /*new_state*/) {}

  /**
   * @brief Called when an item is moved to the DLQ.
   *
   * @param action_name Name of the action that triggered the DLQ transfer.
   * @param ec          Error code describing the failure cause.
   */
  virtual void on_dlq_item(std::string_view /*action_name*/,
                            std::error_code /*ec*/) {}

  /**
   * @brief Called when the circuit breaker opens.
   *
   * @param action_name Name of the action whose circuit opened.
   */
  virtual void on_circuit_open(std::string_view /*action_name*/) {}

  /**
   * @brief Called when the circuit breaker closes.
   *
   * @param action_name Name of the action whose circuit closed.
   */
  virtual void on_circuit_close(std::string_view /*action_name*/) {}
};

// ─── LoggingObserver ──────────────────────────────────────────────────────────

/**
 * @brief Default implementation that logs events to standard output.
 *
 * Intended for development and debugging rather than production use.
 * Output format: `[qbuem] <event>: <content>` (thread-safe, `std::print`-based).
 */
class LoggingObserver : public PipelineObserver {
public:
  /**
   * @brief Logs item processing completion.
   *
   * @param action_name Name of the completed action.
   * @param item_id     Item ID.
   * @param latency_us  Latency in µs.
   */
  void on_item_done(std::string_view action_name,
                    uint64_t item_id,
                    uint64_t latency_us) override {
    std::print(stderr,
               "[qbuem] item_done: action={} id={} latency={}us\n",
               action_name, item_id, latency_us);
  }

  /**
   * @brief Logs a processing error.
   *
   * @param action_name Name of the action where the error occurred.
   * @param ec          Error code.
   */
  void on_error(std::string_view action_name,
                std::error_code ec) override {
    std::print(stderr,
               "[qbuem] error: action={} code={} msg={}\n",
               action_name, ec.value(), ec.message());
  }

  /**
   * @brief Logs a scale event.
   *
   * @param action_name Name of the action whose scale changed.
   * @param old_workers Previous worker count.
   * @param new_workers New worker count.
   */
  void on_scale_event(std::string_view action_name,
                      size_t old_workers,
                      size_t new_workers) override {
    std::print(stderr,
               "[qbuem] scale: action={} {} -> {} workers\n",
               action_name, old_workers, new_workers);
  }

  /**
   * @brief Logs a pipeline state transition.
   *
   * @param pipeline_name Pipeline name.
   * @param old_state     Previous state.
   * @param new_state     New state.
   */
  void on_state_change(std::string_view pipeline_name,
                       std::string_view old_state,
                       std::string_view new_state) override {
    std::print(stderr,
               "[qbuem] state: pipeline={} {} -> {}\n",
               pipeline_name, old_state, new_state);
  }

  /**
   * @brief Logs a circuit breaker open event.
   *
   * @param action_name Name of the action whose circuit opened.
   */
  void on_circuit_open(std::string_view action_name) override {
    std::print(stderr, "[qbuem] circuit_open: action={}\n", action_name);
  }

  /**
   * @brief Logs a circuit breaker close event.
   *
   * @param action_name Name of the action whose circuit closed.
   */
  void on_circuit_close(std::string_view action_name) override {
    std::print(stderr, "[qbuem] circuit_close: action={}\n", action_name);
  }
};

// ─── NoopObserver ─────────────────────────────────────────────────────────────

/**
 * @brief Zero-overhead implementation used when observability is disabled.
 *
 * All methods are empty virtual functions, so the compiler can fully inline/eliminate them.
 * Using this class instead of leaving `PipelineObserver*` as nullptr allows safe
 * calls without null-pointer checks.
 */
class NoopObserver : public PipelineObserver {};

} // namespace qbuem

/** @} */ // end of qbuem_observability
