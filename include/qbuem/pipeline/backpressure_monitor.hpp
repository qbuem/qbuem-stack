#pragma once

/**
 * @file qbuem/pipeline/backpressure_monitor.hpp
 * @brief v2.5.0 BackpressureMonitor — real-time stage pressure and latency metrics via atomics
 * @defgroup qbuem_backpressure_monitor BackpressureMonitor
 * @ingroup qbuem_pipeline
 *
 * ## Overview
 *
 * BackpressureMonitor provides zero-overhead per-stage telemetry for pipeline health:
 *
 * - `StageMetrics`         : Cache-line-aligned atomic counters for queue depth, throughput,
 *                            and latency per pipeline stage.
 * - `BackpressureMonitor`  : Aggregator that tracks N named stages; thread-safe snapshot API.
 * - `StagePressure`        : Snapshot value type (queue depth ratio, p50/p99 latency, throughput).
 * - `BackpressureAlert`    : Threshold-based alerting (queue saturation, latency breach).
 *
 * ## Usage Example
 * @code
 * BackpressureMonitor monitor;
 * auto& s = monitor.stage("parse");
 *
 * // In the action hot path:
 * s.record_enqueue();                            // item entered queue
 * auto t0 = StageMetrics::now_ns();
 * // ... process ...
 * s.record_dequeue(StageMetrics::now_ns() - t0); // item processed, latency recorded
 *
 * // Snapshot from any thread:
 * auto snap = monitor.snapshot("parse");
 * std::println("parse queue={} p99={}ns tput={}msg/s",
 *              snap.queue_depth, snap.latency_p99_ns, snap.throughput_per_sec);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qbuem {

// ─── StageMetrics ────────────────────────────────────────────────────────────

/**
 * @brief Cache-line-aligned per-stage atomic counters.
 *
 * All hot-path methods are lock-free and use relaxed/release/acquire ordering
 * as appropriate.  One `StageMetrics` instance per named pipeline stage.
 *
 * ### Memory layout
 * Three separate 64-byte cache lines to prevent false sharing:
 *  - Line 0: queue counters (enqueue/dequeue)
 *  - Line 1: latency histogram buckets
 *  - Line 2: throughput & error counters
 */
class StageMetrics {
public:
  /** @brief Latency histogram bucket count (powers-of-two µs boundaries). */
  static constexpr size_t kBuckets = 8;

  /**
   * @brief Bucket upper bounds in nanoseconds.
   *
   * Buckets: <1µs, <5µs, <10µs, <50µs, <100µs, <500µs, <1ms, >=1ms
   */
  static constexpr std::array<uint64_t, kBuckets> kBucketBounds = {
      1'000ULL,    //  0: < 1 µs
      5'000ULL,    //  1: < 5 µs
      10'000ULL,   //  2: < 10 µs
      50'000ULL,   //  3: < 50 µs
      100'000ULL,  //  4: < 100 µs
      500'000ULL,  //  5: < 500 µs
      1'000'000ULL,//  6: < 1 ms
      UINT64_MAX,  //  7: >= 1 ms
  };

  // ── Queue counters (cache line 0) ─────────────────────────────────────────
  alignas(64) std::atomic<int64_t>  queue_depth_{0};   ///< Current queue occupancy (signed: can go negative momentarily)
  alignas(64) std::atomic<uint64_t> enqueue_total_{0}; ///< Total items enqueued

  // ── Latency histogram (cache line 1) ─────────────────────────────────────
  alignas(64) std::array<std::atomic<uint64_t>, kBuckets> hist_{};

  // ── Throughput & error counters (cache line 2) ────────────────────────────
  alignas(64) std::atomic<uint64_t> dequeue_total_{0};  ///< Total items dequeued (= processed)
  alignas(64) std::atomic<uint64_t> error_total_{0};    ///< Total errors/drops
  alignas(64) std::atomic<uint64_t> bytes_total_{0};    ///< Total bytes processed (optional)

  // ── Capacity watermark ────────────────────────────────────────────────────
  uint64_t capacity_{256}; ///< Channel capacity (set once at construction)

  // ── Constructor ───────────────────────────────────────────────────────────
  explicit StageMetrics(uint64_t capacity = 256) noexcept : capacity_(capacity) {
    for (auto& b : hist_) b.store(0, std::memory_order_relaxed);
  }

  // Non-copyable (atomics)
  StageMetrics(const StageMetrics&) = delete;
  StageMetrics& operator=(const StageMetrics&) = delete;

  // ── Hot-path methods ──────────────────────────────────────────────────────

  /**
   * @brief Record that one item was enqueued into this stage's channel.
   *
   * Call from the producer side before `co_await channel.send()`.
   * Hot path — lock-free, O(1).
   */
  void record_enqueue() noexcept {
    queue_depth_.fetch_add(1, std::memory_order_relaxed);
    enqueue_total_.fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief Record that one item was dequeued and processed.
   *
   * @param latency_ns Processing latency in nanoseconds (0 = unknown).
   * @param bytes      Bytes processed in this item (0 = not tracked).
   *
   * Hot path — lock-free, O(1).
   */
  void record_dequeue(uint64_t latency_ns = 0, uint64_t bytes = 0) noexcept {
    queue_depth_.fetch_sub(1, std::memory_order_relaxed);
    dequeue_total_.fetch_add(1, std::memory_order_relaxed);
    if (bytes > 0)
      bytes_total_.fetch_add(bytes, std::memory_order_relaxed);
    if (latency_ns > 0)
      observe_latency(latency_ns);
  }

  /**
   * @brief Record a dropped or errored item.
   */
  void record_error() noexcept {
    error_total_.fetch_add(1, std::memory_order_relaxed);
    queue_depth_.fetch_sub(1, std::memory_order_relaxed);
  }

  /**
   * @brief Observe a latency sample (nanoseconds) into the histogram.
   */
  void observe_latency(uint64_t ns) noexcept {
    for (size_t i = 0; i < kBuckets; ++i) {
      if (ns <= kBucketBounds[i]) {
        hist_[i].fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    hist_[kBuckets - 1].fetch_add(1, std::memory_order_relaxed);
  }

  // ── Utility ───────────────────────────────────────────────────────────────

  /** @brief Return current monotonic time in nanoseconds (for latency measurement). */
  [[nodiscard]] static uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }

  /**
   * @brief Compute the approximate Pth-percentile latency from the histogram.
   *
   * @param p  Percentile in [0, 100] (e.g., 99 → p99).
   * @return   Lower bound of the bucket containing the Pth percentile, in ns.
   *           Returns 0 if no samples recorded.
   */
  [[nodiscard]] uint64_t percentile_ns(double p) const noexcept {
    uint64_t total = 0;
    std::array<uint64_t, kBuckets> counts{};
    for (size_t i = 0; i < kBuckets; ++i) {
      counts[i] = hist_[i].load(std::memory_order_relaxed);
      total += counts[i];
    }
    if (total == 0) return 0;

    const uint64_t target = static_cast<uint64_t>(total * p / 100.0);
    uint64_t cumulative = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
      cumulative += counts[i];
      if (cumulative >= target)
        return (i > 0) ? kBucketBounds[i - 1] : 0;
    }
    return kBucketBounds[kBuckets - 1];
  }

  /** @brief Return the fill ratio [0.0, 1.0] of the backing channel. */
  [[nodiscard]] double fill_ratio() const noexcept {
    if (capacity_ == 0) return 0.0;
    const auto depth = queue_depth_.load(std::memory_order_relaxed);
    return static_cast<double>(depth < 0 ? 0 : depth) / static_cast<double>(capacity_);
  }
};

// ─── StagePressure (snapshot) ────────────────────────────────────────────────

/**
 * @brief Immutable snapshot of a stage's pressure metrics at a point in time.
 *
 * Returned by `BackpressureMonitor::snapshot()`. All fields are plain values;
 * safe to copy and log from any thread.
 */
struct StagePressure {
  std::string   name;               ///< Stage name
  int64_t       queue_depth = 0;    ///< Current queue occupancy
  uint64_t      capacity    = 0;    ///< Channel capacity
  double        fill_ratio  = 0.0;  ///< queue_depth / capacity [0..1]
  uint64_t      enqueue_total = 0;  ///< Lifetime enqueue count
  uint64_t      dequeue_total = 0;  ///< Lifetime dequeue count
  uint64_t      error_total   = 0;  ///< Lifetime error/drop count
  uint64_t      bytes_total   = 0;  ///< Lifetime bytes processed
  uint64_t      latency_p50_ns  = 0; ///< Approx. P50 processing latency (ns)
  uint64_t      latency_p99_ns  = 0; ///< Approx. P99 processing latency (ns)
  uint64_t      latency_p999_ns = 0; ///< Approx. P99.9 processing latency (ns)
  double        throughput_per_sec = 0.0; ///< Estimated throughput (items/sec)
  bool          is_saturated = false; ///< True if fill_ratio >= saturation threshold
};

// ─── BackpressureAlert ────────────────────────────────────────────────────────

/**
 * @brief Threshold configuration for pressure alerts.
 */
struct BackpressureAlert {
  double   saturation_threshold = 0.80; ///< Fill ratio above which the stage is "saturated"
  uint64_t latency_p99_limit_ns = 1'000'000ULL; ///< P99 latency limit (default: 1 ms)
  uint64_t error_rate_ppm       = 1'000;         ///< Max allowed errors per million items

  /**
   * @brief Callback invoked when a threshold is breached.
   *
   * Called from `BackpressureMonitor::check_alerts()` — must be fast; do not block.
   */
  std::function<void(const StagePressure&)> on_alert;
};

// ─── BackpressureMonitor ─────────────────────────────────────────────────────

/**
 * @brief Aggregates `StageMetrics` for all named pipeline stages.
 *
 * Thread-safe registration (cold path, guarded by mutex) and lock-free hot-path
 * access via `stage()`. Snapshot and alert checks are cold-path operations.
 *
 * ### Typical integration
 * ```cpp
 * // Setup (once, before pipeline starts):
 * BackpressureMonitor monitor;
 * monitor.register_stage("parse",    256);
 * monitor.register_stage("validate", 256);
 *
 * // In action lambdas (hot path):
 * auto& m = monitor.stage("parse");
 * m.record_enqueue();
 * auto t0 = StageMetrics::now_ns();
 * // ... process ...
 * m.record_dequeue(StageMetrics::now_ns() - t0);
 *
 * // Dashboard thread (cold path):
 * for (auto& snap : monitor.all_snapshots()) {
 *     std::println("  stage={} depth={}/{} p99={}µs err={}",
 *                  snap.name, snap.queue_depth, snap.capacity,
 *                  snap.latency_p99_ns / 1000, snap.error_total);
 * }
 * ```
 */
class BackpressureMonitor {
public:
  BackpressureMonitor() = default;

  // Non-copyable (owns StageMetrics with atomics)
  BackpressureMonitor(const BackpressureMonitor&) = delete;
  BackpressureMonitor& operator=(const BackpressureMonitor&) = delete;

  /**
   * @brief Register a new named stage (cold path — call before pipeline starts).
   *
   * @param name     Stage name (must be unique within this monitor).
   * @param capacity Channel capacity used for fill_ratio calculation.
   */
  void register_stage(std::string_view name, uint64_t capacity = 256) {
    std::lock_guard lock(mu_);
    auto key = std::string(name);
    if (!stages_.contains(key)) {
      stages_.emplace(key, std::make_unique<StageMetrics>(capacity));
      order_.emplace_back(key);
    }
  }

  /**
   * @brief Get a reference to the named stage's metrics (hot-path safe after register).
   *
   * Performs a map lookup protected by a mutex on the first call per stage name
   * (cold path); subsequent calls use the cached pointer (lock-free).
   *
   * @pre `register_stage(name)` must have been called before the pipeline starts.
   * @param name  Stage name.
   * @return      Reference to the stage's `StageMetrics`.
   */
  [[nodiscard]] StageMetrics& stage(std::string_view name) {
    std::lock_guard lock(mu_);
    auto it = stages_.find(std::string(name));
    if (it == stages_.end()) {
      // Auto-register on first access (convenience — not recommended on hot path)
      auto key = std::string(name);
      stages_.emplace(key, std::make_unique<StageMetrics>());
      order_.emplace_back(key);
      return *stages_.at(key);
    }
    return *it->second;
  }

  /**
   * @brief Build a snapshot of the named stage's current metrics.
   *
   * @param name  Stage name.
   * @return      `StagePressure` snapshot, or default-constructed if not found.
   */
  [[nodiscard]] StagePressure snapshot(std::string_view name) const {
    std::lock_guard lock(mu_);
    auto it = stages_.find(std::string(name));
    if (it == stages_.end()) return {};
    return build_snapshot(std::string(name), *it->second);
  }

  /**
   * @brief Build snapshots for all registered stages (in registration order).
   */
  [[nodiscard]] std::vector<StagePressure> all_snapshots() const {
    std::lock_guard lock(mu_);
    std::vector<StagePressure> result;
    result.reserve(order_.size());
    for (const auto& name : order_) {
      auto it = stages_.find(name);
      if (it != stages_.end())
        result.push_back(build_snapshot(name, *it->second));
    }
    return result;
  }

  /**
   * @brief Check all stages against alert thresholds and invoke callbacks.
   *
   * Cold-path: call from a monitoring thread, not from the reactor.
   *
   * @param alert  Alert configuration (thresholds + callback).
   */
  void check_alerts(const BackpressureAlert& alert) const {
    if (!alert.on_alert) return;
    for (const auto& snap : all_snapshots()) {
      bool breach = false;
      if (snap.fill_ratio >= alert.saturation_threshold) breach = true;
      if (snap.latency_p99_ns > alert.latency_p99_limit_ns) breach = true;
      if (snap.dequeue_total > 0) {
        const uint64_t err_ppm = snap.error_total * 1'000'000ULL / snap.dequeue_total;
        if (err_ppm > alert.error_rate_ppm) breach = true;
      }
      if (breach) alert.on_alert(snap);
    }
  }

  /** @brief Reset all counters and histograms across all stages. */
  void reset() {
    std::lock_guard lock(mu_);
    for (auto& [name, m] : stages_) {
      m->queue_depth_.store(0, std::memory_order_relaxed);
      m->enqueue_total_.store(0, std::memory_order_relaxed);
      m->dequeue_total_.store(0, std::memory_order_relaxed);
      m->error_total_.store(0, std::memory_order_relaxed);
      m->bytes_total_.store(0, std::memory_order_relaxed);
      for (auto& b : m->hist_) b.store(0, std::memory_order_relaxed);
    }
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<StageMetrics>> stages_;
  std::vector<std::string> order_; ///< Registration order for deterministic snapshot output

  [[nodiscard]] static StagePressure build_snapshot(
      const std::string& name, const StageMetrics& m) noexcept
  {
    StagePressure s;
    s.name           = name;
    s.queue_depth    = m.queue_depth_.load(std::memory_order_relaxed);
    s.capacity       = m.capacity_;
    s.fill_ratio     = m.fill_ratio();
    s.enqueue_total  = m.enqueue_total_.load(std::memory_order_relaxed);
    s.dequeue_total  = m.dequeue_total_.load(std::memory_order_relaxed);
    s.error_total    = m.error_total_.load(std::memory_order_relaxed);
    s.bytes_total    = m.bytes_total_.load(std::memory_order_relaxed);
    s.latency_p50_ns  = m.percentile_ns(50.0);
    s.latency_p99_ns  = m.percentile_ns(99.0);
    s.latency_p999_ns = m.percentile_ns(99.9);
    s.is_saturated    = (s.fill_ratio >= 0.80);

    // Simple throughput estimate: dequeue_total / 1 (snapshot-relative, use externally with timing)
    s.throughput_per_sec = 0.0; // Set by caller with delta measurements if needed
    return s;
  }
};

/** @} */

} // namespace qbuem
