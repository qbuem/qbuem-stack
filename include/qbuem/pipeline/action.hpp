#pragma once

/**
 * @file qbuem/pipeline/action.hpp
 * @brief Pipeline action — Action<In, Out>
 * @defgroup qbuem_action Action
 * @ingroup qbuem_pipeline
 *
 * Action is a processing stage in a pipeline.
 * It manages a pool of worker coroutines, dequeues items from the input channel,
 * applies the processing function, and puts results into the output channel.
 *
 * ## Action function signatures
 * ```cpp
 * // Full: context + cancellation token + worker index
 * Task<Result<Out>>(In item, ActionEnv env)
 *
 * // Simple: cancellation token only
 * Task<Result<Out>>(In item, std::stop_token stop)
 *
 * // Plain: minimal
 * Task<Result<Out>>(In item)
 * ```
 *
 * ## State patterns
 * - **Stateless**: no lambda captures
 * - **Immutable**: `const shared_ptr<T>` capture
 * - **Mutable(WorkerLocal)**: `WorkerLocal<T>` + `env.worker_idx`
 * - **External**: accessed via `ServiceRegistry`
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/concepts.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/slo.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace qbuem {

/**
 * @brief Pipeline action — coroutine worker pool based processing stage.
 *
 * @tparam In  Input type.
 * @tparam Out Output type.
 */
template <typename In, typename Out>
class Action {
public:
  /** @brief Normalized Action processing function type (Full signature). */
  using Fn = std::function<Task<Result<Out>>(In, ActionEnv)>;

  /**
   * @brief Action configuration struct.
   */
  struct Config {
    size_t min_workers   = 1;    ///< Minimum number of workers
    size_t max_workers   = 4;    ///< Maximum number of workers
    size_t channel_cap   = 256;  ///< Input channel capacity
    bool   auto_scale    = true; ///< Load-based automatic scaling
    bool   keyed_ordering = false; ///< Guarantee ordering for the same key (when order matters)
    ServiceRegistry *registry = nullptr; ///< Pipeline ServiceRegistry
    /**
     * @brief SLO (Service Level Objective) configuration.
     *
     * When set, tracks latency and error rate SLOs for this action.
     * `std::nullopt` disables SLO tracking (default).
     *
     * Example:
     * @code
     * Action<int,int> a{fn, {.slo = qbuem::SloConfig{
     *     .p99_target_us  = 5000,   // 5ms p99
     *     .p999_target_us = 20000,  // 20ms p999
     *     .error_budget   = 0.001,  // 0.1% error budget
     * }}};
     * @endcode
     */
    std::optional<SloConfig> slo; ///< SLO configuration (disabled when absent)
  };

  /**
   * @brief Construct an Action.
   *
   * @tparam FnT Function type satisfying the ActionFn<FnT, In, Out> concept.
   * @param fn   Processing function (Full/Simple/Plain signatures all accepted).
   * @param cfg  Action configuration.
   */
  template <typename FnT>
    requires ActionFn<FnT, In, Out>
  Action(FnT fn, Config cfg = {})
      : fn_(to_full_action_fn<FnT, In, Out>(std::move(fn))),
        cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<In>>>(cfg_.channel_cap)) {}

  Action(const Action &) = delete;
  Action &operator=(const Action &) = delete;
  Action(Action &&) = default;
  Action &operator=(Action &&) = default;

  // -------------------------------------------------------------------------
  // Item submission
  // -------------------------------------------------------------------------

  /**
   * @brief Push an item into the Action's input channel (with backpressure).
   *
   * @param item Item to process.
   * @param ctx  Item context (default: empty Context).
   */
  Task<Result<void>> push(In item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Attempt to push an item without blocking.
   *
   * @returns `true` on success, `false` when the channel is full.
   */
  bool try_push(In item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  // -------------------------------------------------------------------------
  // Lifecycle
  // -------------------------------------------------------------------------

  /**
   * @brief Start the Action — spawn `min_workers` coroutines on the Dispatcher.
   *
   * @param dispatcher Dispatcher that runs the coroutines.
   * @param out        Output channel (nullptr discards results).
   */
  void start(Dispatcher &dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
    out_channel_ = out;
    stop_src_    = std::make_unique<std::stop_source>();

    for (size_t i = 0; i < cfg_.min_workers; ++i) {
      worker_count_.fetch_add(1, std::memory_order_relaxed);
      dispatcher.spawn(worker_loop(i, dispatcher));
    }
  }

  /**
   * @brief Close the input channel and wait for all workers to finish.
   *
   * The output channel is closed automatically after `drain()` completes.
   */
  Task<void> drain() {
    in_channel_->close();
    // Wait for all workers to exit
    while (worker_count_.load(std::memory_order_acquire) > 0) {
      struct Yield {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
          if (auto *r = Reactor::current())
            r->post([h]() mutable { h.resume(); });
          else
            h.resume();
        }
        void await_resume() noexcept {}
      };
      co_await Yield{};
    }
    if (out_channel_)
      out_channel_->close();
    co_return;
  }

  /**
   * @brief Immediately stop the Action (send cancellation signal).
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    in_channel_->close();
  }

  /**
   * @brief Adjust the worker count to `n`.
   *
   * @param n New target worker count.
   * @param dispatcher Dispatcher used to spawn new workers.
   */
  void scale_to(size_t n, Dispatcher &dispatcher) {
    size_t current = worker_count_.load(std::memory_order_relaxed);
    if (n > current) {
      for (size_t i = current; i < n; ++i) {
        worker_count_.fetch_add(1, std::memory_order_relaxed);
        dispatcher.spawn(worker_loop(i, dispatcher));
      }
    } else if (n < current) {
      target_workers_.store(n, std::memory_order_release);
    }
  }

  void scale_out(Dispatcher &dispatcher) {
    scale_to(worker_count_.load() + 1, dispatcher);
  }

  void scale_in() {
    size_t c = worker_count_.load(std::memory_order_relaxed);
    if (c > cfg_.min_workers)
      target_workers_.store(c - 1, std::memory_order_release);
  }

  /**
   * @brief Return the output channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
    return out_channel_;
  }

  /**
   * @brief Return the input channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<In>>> input() const {
    return in_channel_;
  }

private:
  // -------------------------------------------------------------------------
  // Worker loop
  // -------------------------------------------------------------------------
  Task<void> worker_loop(size_t worker_idx, Dispatcher &dispatcher) {
    auto stop_token = stop_src_ ? stop_src_->get_token() : std::stop_token{};

    for (;;) {
      // Scale-in check: if target_workers_ < our index, exit
      size_t target = target_workers_.load(std::memory_order_acquire);
      if (target > 0 && worker_idx >= target) {
        break;
      }

      // Receive next item from input channel
      auto citem = co_await in_channel_->recv();
      if (!citem) break; // EOS

      // Build ActionEnv
      ActionEnv env{
          .ctx        = citem->ctx,
          .stop       = stop_token,
          .worker_idx = worker_idx,
          .registry   = cfg_.registry ? cfg_.registry : &global_registry(),
      };

      // Execute the action
      auto result = co_await fn_(std::move(citem->value), env);

      // Forward result to output channel
      if (result.has_value() && out_channel_) {
        co_await out_channel_->send(
            ContextualItem<Out>{std::move(*result), env.ctx});
      }
      // On error: item is dropped (DLQ support comes in v0.8.0)
    }

    size_t remaining = worker_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0 && out_channel_)
      out_channel_->close();
    co_return;
  }

  // -------------------------------------------------------------------------
  // Data members
  // -------------------------------------------------------------------------
  Fn                                                    fn_;
  Config                                                cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<In>>>     in_channel_;
  std::shared_ptr<AsyncChannel<ContextualItem<Out>>>    out_channel_;
  std::unique_ptr<std::stop_source>                     stop_src_;
  std::atomic<size_t>                                   worker_count_{0};
  std::atomic<size_t>                                   target_workers_{0}; // 0 = unlimited
};

} // namespace qbuem

/** @} */
