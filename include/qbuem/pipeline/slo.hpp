#pragma once

/**
 * @file qbuem/pipeline/slo.hpp
 * @brief SLO (Service Level Objective) tracking and error budget management
 * @defgroup qbuem_slo SLO Tracking
 * @ingroup qbuem_pipeline
 *
 * This header tracks latency and error-rate SLOs for pipeline actions:
 *
 * - `SloConfig`           : SLO policy settings (p99/p999 targets, error budget, violation callback)
 * - `LatencyHistogram`    : Rolling-window latency histogram (kWindow=1024 samples)
 * - `ErrorBudgetTracker`  : Latency + error-rate SLO tracker
 * - `SloObserver`         : `PipelineObserver` extension — SLO violation event hook
 * - `LoggingSloObserver`  : Default implementation that logs to standard error
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/observability.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <print>
#include <functional>
#include <string>
#include <string_view>

namespace qbuem {

using std::chrono::microseconds;

// ─── SloConfig ────────────────────────────────────────────────────────────────

/**
 * @brief SLO target configuration.
 *
 * Holds latency percentile targets, the permitted error ratio, and a
 * callback function invoked when a violation is detected.
 *
 * ### Defaults
 * - p99 target:  10 ms (10,000 µs)
 * - p999 target: 50 ms (50,000 µs)
 * - error budget: 0.1% (0.001)
 */
struct SloConfig {
  /** @brief p99 latency target (default: 10 ms). */
  microseconds p99_target{10'000};

  /** @brief p99.9 latency target (default: 50 ms). */
  microseconds p999_target{50'000};

  /**
   * @brief Permitted error ratio (0.0 – 1.0).
   *
   * When the error rate over the rolling window (1000 items) exceeds this
   * value the error budget is considered exhausted. Default is 0.1% (0.001).
   */
  double error_budget{0.001};

  /**
   * @brief Callback invoked on an SLO violation.
   *
   * @param action_name Name of the action where the violation was detected.
   *
   * The callback may perform metric collection, alert dispatch,
   * circuit-breaker triggering, etc.
   */
  std::function<void(std::string_view action_name)> on_violation;
};

// ─── LatencyHistogram ─────────────────────────────────────────────────────────

/**
 * @brief Rolling-window latency histogram.
 *
 * Retains up to `kWindow` (1024) recent latency samples in a ring buffer
 * and computes p99 and p99.9 percentiles.
 *
 * ### Four-bucket fast classification
 * - Bucket 0: < 1 ms  (< 1,000 µs)
 * - Bucket 1: < 10 ms (< 10,000 µs)
 * - Bucket 2: < 100 ms (< 100,000 µs)
 * - Bucket 3: >= 100 ms (>= 100,000 µs)
 *
 * ### Thread safety
 * `head_` and `count_` are protected by atomic operations.
 * Most accurate in a single-producer environment; in multi-producer
 * environments minor statistically harmless sample loss may occur.
 */
class LatencyHistogram {
public:
  /** @brief Rolling window size (maximum number of retained samples). */
  static constexpr size_t kWindow = 1024;

  /**
   * @brief Record a single latency sample (O(1)).
   *
   * Writes to the ring buffer in a circular fashion, overwriting the oldest
   * sample when the buffer is full. Also updates the four-bucket counters.
   *
   * @param latency Measured latency.
   */
  void record(microseconds latency) noexcept {
    size_t idx = head_.fetch_add(1, std::memory_order_relaxed) % kWindow;
    auto raw = latency.count();
    samples_[idx] = static_cast<uint32_t>(
        raw > static_cast<decltype(raw)>(UINT32_MAX)
            ? UINT32_MAX
            : static_cast<uint32_t>(raw));

    size_t c = count_.load(std::memory_order_relaxed);
    if (c < kWindow)
      count_.fetch_add(1, std::memory_order_relaxed);

    // Update four-bucket fast classification counters
    uint64_t us = static_cast<uint64_t>(latency.count());
    if      (us <   1'000u) bucket_[0].fetch_add(1, std::memory_order_relaxed);
    else if (us <  10'000u) bucket_[1].fetch_add(1, std::memory_order_relaxed);
    else if (us < 100'000u) bucket_[2].fetch_add(1, std::memory_order_relaxed);
    else                    bucket_[3].fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief Return the p99 latency.
   *
   * Sorts the currently retained samples and returns the 99th percentile.
   * Returns 0 if no samples are available.
   *
   * @returns p99 latency.
   */
  [[nodiscard]] microseconds p99() const { return percentile(990); }

  /**
   * @brief Return the p99.9 latency.
   *
   * Sorts the currently retained samples and returns the 99.9th percentile.
   * Returns 0 if no samples are available.
   *
   * @returns p99.9 latency.
   */
  [[nodiscard]] microseconds p999() const { return percentile(999); }

  /**
   * @brief Return the four-bucket counters.
   *
   * @returns Array of counters in order {<1ms, <10ms, <100ms, >=100ms}.
   */
  [[nodiscard]] std::array<uint64_t, 4> bucket_counts() const noexcept {
    return {bucket_[0].load(std::memory_order_relaxed),
            bucket_[1].load(std::memory_order_relaxed),
            bucket_[2].load(std::memory_order_relaxed),
            bucket_[3].load(std::memory_order_relaxed)};
  }

  /**
   * @brief Reset the histogram.
   *
   * Clears all samples and counters to zero.
   */
  void reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    count_.store(0, std::memory_order_relaxed);
    samples_.fill(0);
    for (auto& b : bucket_)
      b.store(0, std::memory_order_relaxed);
  }

private:
  /**
   * @brief Compute the given per-mille percentile.
   *
   * @param pmille Per-mille value in the range 0–1000 (990 = p99, 999 = p99.9).
   * @returns Latency at the requested percentile. Returns 0 if no samples.
   */
  [[nodiscard]] microseconds percentile(int pmille) const {
    size_t n = count_.load(std::memory_order_relaxed);
    if (n == 0) return microseconds{0};

    // Copy valid samples into a sort buffer
    std::array<uint32_t, kWindow> buf{};
    size_t head = head_.load(std::memory_order_relaxed) % kWindow;
    for (size_t i = 0; i < n; ++i)
      buf[i] = samples_[(head + kWindow - n + i) % kWindow];

    std::sort(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(n));

    size_t idx = static_cast<size_t>(
        std::ceil(static_cast<double>(n) * static_cast<double>(pmille) / 1000.0));
    if (idx > 0) --idx;
    if (idx >= n) idx = n - 1;
    return microseconds{buf[idx]};
  }

  alignas(64) std::atomic<size_t>  head_{0};
  alignas(64) std::atomic<size_t>  count_{0};
  std::array<uint32_t, kWindow>    samples_{};
  std::array<std::atomic<uint64_t>, 4> bucket_{};
};

// ─── ErrorBudgetTracker ───────────────────────────────────────────────────────

/**
 * @brief Latency and error-rate SLO tracker.
 *
 * Records each item processing result (success + latency / error) and
 * compares it against the targets in `SloConfig` to determine violations.
 *
 * ### Error rate calculation
 * Approximates the error rate based on a rolling 1000-item window.
 * Uses cumulative counters (`total_`, `errors_`) for the calculation.
 *
 * ### Thread safety
 * Atomic counters (`total_`, `errors_`) are thread-safe.
 * `LatencyHistogram` may have minor inaccuracies under multiple producers.
 */
class ErrorBudgetTracker {
public:
  /** @brief Rolling window size used for error rate calculation. */
  static constexpr size_t kRollingWindow = 1000;

  /**
   * @brief Construct an ErrorBudgetTracker.
   *
   * @param cfg         SLO policy settings.
   * @param action_name Name of the action this tracker monitors.
   */
  ErrorBudgetTracker(SloConfig cfg, std::string_view action_name)
      : cfg_(std::move(cfg)), action_name_(action_name) {}

  // ─── Recording ───────────────────────────────────────────────────────────

  /**
   * @brief Record a successful item processing event.
   *
   * Adds the latency to the histogram and increments the total counter.
   *
   * @param latency Processing latency.
   */
  void record_success(microseconds latency) noexcept {
    histogram_.record(latency);
    total_.fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief Record an error.
   *
   * Increments both the total counter and the error counter.
   */
  void record_error() noexcept {
    total_.fetch_add(1, std::memory_order_relaxed);
    errors_.fetch_add(1, std::memory_order_relaxed);
  }

  // ─── SLO check ───────────────────────────────────────────────────────────

  /**
   * @brief Check for SLO violations and invoke `SloConfig::on_violation` if any.
   *
   * A violation is reported when any of the following conditions hold:
   * 1. p99 latency > `SloConfig::p99_target`
   * 2. p99.9 latency > `SloConfig::p999_target`
   * 3. error rate > `SloConfig::error_budget`
   *
   * Does nothing if `on_violation` is not set.
   */
  void check_slo() {
    if (!cfg_.on_violation) return;

    bool violated = false;

    if (histogram_.p99()  > cfg_.p99_target)  violated = true;
    if (histogram_.p999() > cfg_.p999_target) violated = true;
    if (budget_exhausted())                    violated = true;

    if (violated) cfg_.on_violation(action_name_);
  }

  // ─── Queries ─────────────────────────────────────────────────────────────

  /**
   * @brief Check whether the error budget is exhausted.
   *
   * Returns true when the current error rate exceeds `SloConfig::error_budget`.
   *
   * @returns true if the error budget is exhausted.
   */
  [[nodiscard]] bool budget_exhausted() const noexcept {
    return error_rate() > cfg_.error_budget;
  }

  /**
   * @brief Return the rolling-window error rate.
   *
   * Approximates the error rate within the rolling 1000-sample window from
   * cumulative counters. Returns 0.0 when the total count is zero.
   *
   * @returns Error rate in the range 0.0 – 1.0.
   */
  [[nodiscard]] double error_rate() const noexcept {
    uint64_t total  = total_.load(std::memory_order_relaxed);
    uint64_t errors = errors_.load(std::memory_order_relaxed);
    if (total == 0) return 0.0;
    uint64_t window = std::min(total, static_cast<uint64_t>(kRollingWindow));
    return static_cast<double>(errors) / static_cast<double>(window);
  }

  /**
   * @brief Return a const reference to the internal latency histogram.
   * @returns Const reference to the latency histogram.
   */
  [[nodiscard]] const LatencyHistogram& histogram() const noexcept {
    return histogram_;
  }

  /**
   * @brief Return the total number of processed items (successes + errors).
   * @returns Cumulative processed count.
   */
  [[nodiscard]] uint64_t total_count() const noexcept {
    return total_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Return the total number of errors.
   * @returns Cumulative error count.
   */
  [[nodiscard]] uint64_t error_count() const noexcept {
    return errors_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Reset all counters and the histogram.
   */
  void reset() noexcept {
    total_.store(0, std::memory_order_relaxed);
    errors_.store(0, std::memory_order_relaxed);
    histogram_.reset();
  }

private:
  SloConfig             cfg_;
  std::string           action_name_;
  LatencyHistogram      histogram_;

  /** @brief Total processed item count (successes + errors). */
  std::atomic<uint64_t> total_{0};

  /** @brief Total error count. */
  std::atomic<uint64_t> errors_{0};
};

// ─── SloObserver ──────────────────────────────────────────────────────────────

/**
 * @brief `PipelineObserver` extension that supports SLO violation events.
 *
 * Inherits from `PipelineObserver` to retain all existing pipeline event
 * hooks, and adds an `on_slo_violation()` hook called on SLO violations.
 *
 * Implementations override `on_slo_violation()` to send alerts, record
 * metrics, trigger circuit breakers, etc.
 *
 * ### Thread safety
 * `on_slo_violation()` may be called concurrently from multiple worker
 * threads. Implementations must protect their internal state appropriately.
 *
 * ### Usage example
 * ```cpp
 * class MyObserver : public SloObserver {
 * public:
 *   void on_slo_violation(std::string_view action_name,
 *                          std::string_view metric_name,
 *                          double measured, double target) override {
 *     std::print(stderr, "[SLO violation] {}: {} {:.2f} > {:.2f}\n",
 *       action_name, metric_name, measured, target);
 *   }
 * };
 * ```
 */
class SloObserver : public PipelineObserver {
public:
  virtual ~SloObserver() = default;

  /**
   * @brief Called when an SLO violation is detected.
   *
   * @param action_name Name of the action where the violation was detected.
   * @param metric_name Name of the violated metric.
   *                    Examples: `"p99_latency"`, `"p999_latency"`, `"error_rate"`.
   * @param measured    Measured value.
   *                    Microseconds (µs) for latency metrics,
   *                    ratio 0.0–1.0 for error rate metrics.
   * @param target      Target threshold (same unit as `measured`).
   */
  virtual void on_slo_violation(std::string_view /*action_name*/,
                                  std::string_view /*metric_name*/,
                                  double           /*measured*/,
                                  double           /*target*/) {}
};

// ─── LoggingSloObserver ───────────────────────────────────────────────────────

/**
 * @brief Default implementation that logs SLO violations to standard error.
 *
 * Intended for development and debugging.
 * Output format: `[qbuem/slo] violation: action=<name> metric=<metric> measured=<value> target=<target>`
 */
class LoggingSloObserver : public SloObserver {
public:
  /**
   * @brief Print the SLO violation to standard error (`stderr`).
   *
   * @param action_name Name of the action where the violation was detected.
   * @param metric_name Name of the violated metric.
   * @param measured    Measured value.
   * @param target      Target threshold.
   */
  void on_slo_violation(std::string_view action_name,
                         std::string_view metric_name,
                         double           measured,
                         double           target) override {
    std::print(stderr,
               "[qbuem/slo] violation: action={} metric={}"
               " measured={:.3f} target={:.3f}\n",
               action_name, metric_name, measured, target);
  }
};

} // namespace qbuem

/** @} */ // end of qbuem_slo
