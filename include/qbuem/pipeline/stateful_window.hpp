#pragma once

/**
 * @file qbuem/pipeline/stateful_window.hpp
 * @brief v2.5.0 StatefulWindow — thread-local aggregation with periodic flush
 * @defgroup qbuem_stateful_window StatefulWindow
 * @ingroup qbuem_pipeline
 *
 * ## Overview
 *
 * `StatefulWindow<T, Acc, Out>` is a pipeline action that accumulates items
 * in **thread-local storage** and flushes results downstream when a window
 * boundary is reached.  Unlike `WindowedAction` (which uses a shared mutex
 * and watermark propagation), `StatefulWindow` is optimised for maximum
 * throughput on a single reactor thread: there is zero cross-thread
 * synchronisation on the hot accumulation path.
 *
 * ## Window strategies
 *
 * | Strategy       | Description                                     |
 * |----------------|-------------------------------------------------|
 * | `TumblingFlush`| Fixed-duration window; resets after each flush  |
 * | `CountFlush`   | Flush after N items regardless of time          |
 * | `HybridFlush`  | Flush on either count OR time limit, first wins |
 *
 * ## Thread-local model
 *
 * Each worker coroutine accumulates into its own `Acc` instance stored in
 * `thread_local` storage indexed by `worker_idx`.  On flush, the accumulated
 * value is emitted via `ActionEnv::out` and the accumulator is reset.
 *
 * ## Usage Example
 * @code
 * // Tumbling 100ms window that sums int64 values
 * struct SumAcc { int64_t total = 0; size_t count = 0; };
 * struct WindowResult { int64_t sum; size_t count; };
 *
 * auto window = StatefulWindow<int64_t, SumAcc, WindowResult>{
 *     StatefulWindowConfig{
 *         .strategy    = FlushStrategy::HybridFlush,
 *         .window_ms   = 100,
 *         .max_items   = 1024,
 *         .num_workers = 4,
 *     },
 *     // accumulate: fold one item into the accumulator
 *     [](SumAcc& acc, int64_t v) { acc.total += v; ++acc.count; },
 *     // flush: convert accumulator → output; reset accumulator
 *     [](SumAcc& acc) -> WindowResult {
 *         auto r = WindowResult{acc.total, acc.count};
 *         acc = {};
 *         return r;
 *     }
 * };
 *
 * auto p = PipelineBuilder<int64_t, WindowResult>{}
 *     .add<WindowResult>(window.as_action())
 *     .build();
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace qbuem {

// ─── FlushStrategy ───────────────────────────────────────────────────────────

/**
 * @brief Determines when a StatefulWindow flushes its accumulator downstream.
 */
enum class FlushStrategy : uint8_t {
  TumblingFlush, ///< Flush every `window_ms` milliseconds (time-driven)
  CountFlush,    ///< Flush after `max_items` items (count-driven)
  HybridFlush,   ///< Flush on either `window_ms` or `max_items`, first wins
};

// ─── StatefulWindowConfig ────────────────────────────────────────────────────

/**
 * @brief Configuration for a StatefulWindow instance.
 */
struct StatefulWindowConfig {
  FlushStrategy strategy    = FlushStrategy::HybridFlush; ///< Flush trigger strategy
  uint64_t      window_ms   = 100;    ///< Window duration in milliseconds (TumblingFlush / HybridFlush)
  size_t        max_items   = 1024;   ///< Max items per window (CountFlush / HybridFlush)
  size_t        num_workers = 4;      ///< Expected max worker count (pre-allocates per-worker state)
  size_t        channel_cap = 256;    ///< Output channel capacity
};

// ─── StatefulWindow ──────────────────────────────────────────────────────────

/**
 * @brief Thread-local aggregation window action.
 *
 * @tparam T    Input item type.
 * @tparam Acc  Accumulator type (one instance per worker, default-constructed).
 * @tparam Out  Output (result) type emitted on flush.
 */
template <typename T, typename Acc, typename Out>
class StatefulWindow {
public:
  /** @brief Accumulate function: fold one item into the accumulator. */
  using AccumulateFn = std::function<void(Acc&, T)>;

  /**
   * @brief Flush function: convert the accumulator to output and reset it.
   *
   * Must reset `acc` to the "empty" state before returning so the next window
   * starts from a clean accumulator.
   */
  using FlushFn = std::function<Out(Acc&)>;

  /**
   * @brief Construct a StatefulWindow.
   *
   * @param cfg         Window configuration.
   * @param accumulate  Fold function (hot path — keep it allocation-free).
   * @param flush       Flush + reset function (cold path — may allocate).
   */
  StatefulWindow(StatefulWindowConfig cfg,
                 AccumulateFn accumulate,
                 FlushFn flush)
      : cfg_(cfg),
        accumulate_(std::move(accumulate)),
        flush_(std::move(flush))
  {
    // Pre-allocate per-worker state to avoid runtime allocations on hot path
    worker_state_.resize(cfg_.num_workers);
  }

  /**
   * @brief Return the action function for use in PipelineBuilder.
   *
   * The returned lambda captures `this` by pointer; the StatefulWindow must
   * outlive the pipeline.
   *
   * @code
   * .add<Out>(window.as_action())
   * @endcode
   */
  [[nodiscard]] auto as_action()
  {
    return [this](T item, ActionEnv env) -> Task<Result<Out>> {
      if (env.stop.stop_requested())
        co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));

      const size_t idx = env.worker_idx % cfg_.num_workers;
      WorkerState& ws  = worker_state_[idx];

      // Accumulate item into thread-local state
      accumulate_(ws.acc, std::move(item));
      ++ws.item_count;

      // Check whether we should flush
      bool should_flush = false;

      if (cfg_.strategy == FlushStrategy::CountFlush ||
          cfg_.strategy == FlushStrategy::HybridFlush) {
        if (ws.item_count >= cfg_.max_items)
          should_flush = true;
      }

      if (!should_flush &&
          (cfg_.strategy == FlushStrategy::TumblingFlush ||
           cfg_.strategy == FlushStrategy::HybridFlush)) {
        const auto now_ms = steady_ms();
        if (ws.window_start_ms == 0)
          ws.window_start_ms = now_ms; // First item in this window
        if (now_ms - ws.window_start_ms >= cfg_.window_ms)
          should_flush = true;
      }

      if (should_flush) {
        Out result = flush_(ws.acc);
        ws.item_count     = 0;
        ws.window_start_ms = steady_ms();
        co_return result;
      }

      // Window still open — return a sentinel indicating "no output yet"
      // Caller can filter nullopt from the output channel.
      co_return std::unexpected(std::make_error_code(std::errc::operation_in_progress));
    };
  }

  /**
   * @brief Force-flush all worker accumulators (call on pipeline drain).
   *
   * Returns one `Out` per worker that has at least one accumulated item.
   * Thread-safe to call from the shutdown path (no racing workers).
   */
  [[nodiscard]] std::vector<Out> drain() {
    std::vector<Out> results;
    for (auto& ws : worker_state_) {
      if (ws.item_count > 0) {
        results.push_back(flush_(ws.acc));
        ws.item_count = 0;
        ws.window_start_ms = 0;
      }
    }
    return results;
  }

  /** @brief Reset all per-worker state (clear accumulators and counters). */
  void reset() {
    for (auto& ws : worker_state_) {
      ws.acc = Acc{};
      ws.item_count = 0;
      ws.window_start_ms = 0;
    }
  }

  /** @brief Return the current item count for a given worker (diagnostic). */
  [[nodiscard]] size_t item_count(size_t worker_idx) const noexcept {
    if (worker_idx >= worker_state_.size()) return 0;
    return worker_state_[worker_idx].item_count;
  }

private:
  // ── Per-worker state ──────────────────────────────────────────────────────
  struct WorkerState {
    Acc      acc{};              ///< Thread-local accumulator (default-constructed)
    size_t   item_count    = 0;  ///< Items accumulated since last flush
    uint64_t window_start_ms = 0; ///< Monotonic ms when the current window opened
  };

  StatefulWindowConfig     cfg_;
  AccumulateFn             accumulate_;
  FlushFn                  flush_;
  std::vector<WorkerState> worker_state_; ///< Indexed by worker_idx

  // ── Helpers ───────────────────────────────────────────────────────────────

  [[nodiscard]] static uint64_t steady_ms() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
  }
};

// ─── TumblingWindow convenience alias ────────────────────────────────────────

/**
 * @brief Build a time-based TumblingFlush StatefulWindow.
 *
 * @tparam T    Input type.
 * @tparam Acc  Accumulator type.
 * @tparam Out  Output type.
 *
 * @param window_ms   Window duration (milliseconds).
 * @param num_workers Expected number of pipeline workers.
 * @param accumulate  Fold function.
 * @param flush       Flush + reset function.
 */
template <typename T, typename Acc, typename Out>
[[nodiscard]] StatefulWindow<T, Acc, Out>
make_tumbling_window(uint64_t window_ms,
                     size_t num_workers,
                     typename StatefulWindow<T, Acc, Out>::AccumulateFn accumulate,
                     typename StatefulWindow<T, Acc, Out>::FlushFn flush)
{
  return StatefulWindow<T, Acc, Out>{
      StatefulWindowConfig{
          .strategy    = FlushStrategy::TumblingFlush,
          .window_ms   = window_ms,
          .num_workers = num_workers,
      },
      std::move(accumulate),
      std::move(flush)
  };
}

/**
 * @brief Build a count-based CountFlush StatefulWindow.
 */
template <typename T, typename Acc, typename Out>
[[nodiscard]] StatefulWindow<T, Acc, Out>
make_count_window(size_t max_items,
                  size_t num_workers,
                  typename StatefulWindow<T, Acc, Out>::AccumulateFn accumulate,
                  typename StatefulWindow<T, Acc, Out>::FlushFn flush)
{
  return StatefulWindow<T, Acc, Out>{
      StatefulWindowConfig{
          .strategy    = FlushStrategy::CountFlush,
          .max_items   = max_items,
          .num_workers = num_workers,
      },
      std::move(accumulate),
      std::move(flush)
  };
}

/** @} */

} // namespace qbuem
