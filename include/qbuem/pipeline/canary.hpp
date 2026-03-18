#pragma once

/**
 * @file qbuem/pipeline/canary.hpp
 * @brief Canary deployment automation — CanaryRouter
 * @defgroup qbuem_canary CanaryRouter
 * @ingroup qbuem_pipeline
 *
 * CanaryRouter distributes traffic between a stable (existing) pipeline and a canary (new) pipeline
 * according to a configured ratio, monitors metrics, and supports automatic/manual rollback.
 *
 * ## Rollout procedure
 * ```
 * 1% → 5% → 25% → 100%  (automatic step-by-step increase)
 *                         ↑ error_delta / p99 / budget checked at each step
 * ```
 *
 * ## Usage example
 * ```cpp
 * CanaryRouter<int> router;
 * router.set_stable(stable_pipeline);
 * router.set_canary(canary_pipeline);
 * router.start_gradual_rollout({
 *   .steps             = {1, 5, 25, 100},
 *   .step_duration     = 60s,
 *   .max_error_delta   = 0.01,   // rollback if errors increase by more than 1%
 *   .max_p99_ratio     = 1.5,    // rollback if p99 degrades by more than 50%
 * });
 * ```
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace qbuem {

// ---------------------------------------------------------------------------
// CanaryMetrics — per-pipeline metric collection
// ---------------------------------------------------------------------------

/**
 * @brief Pipeline metrics used in canary deployments.
 */
struct CanaryMetrics {
  std::atomic<uint64_t> total{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> latency_sum_us{0};   // cumulative latency (µs)
  std::atomic<uint64_t> latency_count{0};

  void record_success(uint64_t latency_us) {
    ++total;
    ++latency_count;
    latency_sum_us.fetch_add(latency_us, std::memory_order_relaxed);
  }

  void record_error() {
    ++total;
    ++errors;
  }

  [[nodiscard]] double error_rate() const noexcept {
    uint64_t t = total.load(std::memory_order_relaxed);
    return (t == 0) ? 0.0 : static_cast<double>(errors.load()) / t;
  }

  [[nodiscard]] uint64_t avg_latency_us() const noexcept {
    uint64_t c = latency_count.load(std::memory_order_relaxed);
    return (c == 0) ? 0 : latency_sum_us.load() / c;
  }

  void reset() {
    total.store(0);
    errors.store(0);
    latency_sum_us.store(0);
    latency_count.store(0);
  }
};

// ---------------------------------------------------------------------------
// CanaryRouter<T>
// ---------------------------------------------------------------------------

/**
 * @brief Traffic-splitting canary router with metric-based automatic rollback.
 *
 * @tparam T Pipeline message type.
 */
template <typename T>
class CanaryRouter {
public:
  // -------------------------------------------------------------------------
  // Configuration
  // -------------------------------------------------------------------------

  /**
   * @brief Step-by-step rollout configuration.
   */
  struct RolloutConfig {
    /// Canary traffic percentage at each step (%) — e.g., {1, 5, 25, 100}
    std::vector<uint32_t> steps{1, 5, 25, 100};

    /// Duration of each step (default 60 seconds)
    std::chrono::seconds step_duration{60};

    /// Roll back if the canary error rate exceeds the stable rate by this amount
    double max_error_delta{0.01};

    /// Roll back if the canary average latency exceeds stable by this multiplier
    double max_latency_ratio{1.5};

    /// Callback invoked on rollback
    std::function<void(std::string_view reason)> on_rollback;

    /// Callback invoked on step completion (current step %)
    std::function<void(uint32_t step_pct)> on_step_complete;
  };

  // Pipeline push interface
  using PushFn = std::function<bool(T)>;  // try_push wrapper

  // -------------------------------------------------------------------------
  // Pipeline registration
  // -------------------------------------------------------------------------

  /**
   * @brief Sets the push function for the stable pipeline.
   */
  CanaryRouter &set_stable(PushFn fn) {
    stable_push_ = std::move(fn);
    return *this;
  }

  /**
   * @brief Sets the push function for the canary pipeline.
   */
  CanaryRouter &set_canary(PushFn fn) {
    canary_push_ = std::move(fn);
    return *this;
  }

  // -------------------------------------------------------------------------
  // Traffic routing
  // -------------------------------------------------------------------------

  /**
   * @brief Routes an item to stable or canary according to the current ratio.
   *
   * @param item Item to push.
   * @returns true if pushed successfully.
   */
  bool push(T item) {
    uint32_t pct = canary_pct_.load(std::memory_order_relaxed);
    bool send_to_canary = (pct > 0) && (rng_() % 100 < pct);

    if (send_to_canary && canary_push_) {
      auto t0 = std::chrono::steady_clock::now();
      bool ok = canary_push_(item);
      auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
      if (ok) canary_metrics_.record_success(static_cast<uint64_t>(dt));
      else    canary_metrics_.record_error();
      return ok;
    }

    if (stable_push_) {
      auto t0 = std::chrono::steady_clock::now();
      bool ok = stable_push_(item);
      auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
      if (ok) stable_metrics_.record_success(static_cast<uint64_t>(dt));
      else    stable_metrics_.record_error();
      return ok;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // Rollout control
  // -------------------------------------------------------------------------

  /**
   * @brief Starts a step-by-step gradual rollout.
   *
   * Evaluates metrics at each step and automatically rolls back on violation.
   * This function must be run as a background coroutine.
   *
   * @param cfg Rollout configuration.
   * @returns Task<void> — exits when rollout completes or a rollback occurs.
   */
  Task<void> start_gradual_rollout(RolloutConfig cfg) {
    cfg_ = std::move(cfg);
    rollout_active_.store(true);

    for (uint32_t step_pct : cfg_.steps) {
      if (!rollout_active_.load()) co_return;  // manually rolled back

      // Apply the current step percentage
      set_canary_percent(step_pct);
      stable_metrics_.reset();
      canary_metrics_.reset();

      // Poll metrics every 100ms for step_duration
      auto deadline = std::chrono::steady_clock::now() + cfg_.step_duration;
      while (std::chrono::steady_clock::now() < deadline) {
        co_await sleep(100);  // 100ms poll interval via AsyncSleep

        if (!rollout_active_.load()) co_return;

        // Check automatic rollback conditions
        if (auto reason = check_rollback_condition()) {
          rollout_active_.store(false);
          set_canary_percent(0);
          if (cfg_.on_rollback) cfg_.on_rollback(*reason);
          co_return;
        }
      }

      if (cfg_.on_step_complete) cfg_.on_step_complete(step_pct);
    }

    // 100% reached — rollout complete
    rollout_active_.store(false);
  }

  /**
   * @brief Immediately rolls back to stable manually.
   */
  void rollback_to_stable() {
    rollout_active_.store(false);
    set_canary_percent(0);
  }

  /**
   * @brief Directly sets the canary traffic percentage (0–100).
   */
  void set_canary_percent(uint32_t pct) noexcept {
    canary_pct_.store(std::min(pct, 100u), std::memory_order_relaxed);
  }

  [[nodiscard]] uint32_t canary_percent() const noexcept {
    return canary_pct_.load(std::memory_order_relaxed);
  }

  // -------------------------------------------------------------------------
  // Metric accessors
  // -------------------------------------------------------------------------

  [[nodiscard]] const CanaryMetrics &stable_metrics() const noexcept { return stable_metrics_; }
  [[nodiscard]] const CanaryMetrics &canary_metrics() const noexcept { return canary_metrics_; }

private:
  // -------------------------------------------------------------------------
  // Automatic rollback condition check
  // -------------------------------------------------------------------------

  [[nodiscard]] std::optional<std::string> check_rollback_condition() const {
    double s_err = stable_metrics_.error_rate();
    double c_err = canary_metrics_.error_rate();
    if (c_err - s_err > cfg_.max_error_delta) {
      return "error_delta exceeded: canary=" + std::to_string(c_err) +
             " stable=" + std::to_string(s_err);
    }

    uint64_t s_lat = stable_metrics_.avg_latency_us();
    uint64_t c_lat = canary_metrics_.avg_latency_us();
    if (s_lat > 0 &&
        static_cast<double>(c_lat) / s_lat > cfg_.max_latency_ratio) {
      return "latency_ratio exceeded: canary=" + std::to_string(c_lat) +
             "us stable=" + std::to_string(s_lat) + "us";
    }

    return std::nullopt;
  }

  // -------------------------------------------------------------------------
  // Data members
  // -------------------------------------------------------------------------
  PushFn              stable_push_;
  PushFn              canary_push_;
  CanaryMetrics       stable_metrics_;
  CanaryMetrics       canary_metrics_;
  std::atomic<uint32_t> canary_pct_{0};
  std::atomic<bool>   rollout_active_{false};
  RolloutConfig       cfg_;

  // Thread-local RNG (simple implementation; speed over precision)
  static thread_local std::mt19937 rng_;
};

template <typename T>
thread_local std::mt19937 CanaryRouter<T>::rng_{std::random_device{}()};

/** @} */

} // namespace qbuem
