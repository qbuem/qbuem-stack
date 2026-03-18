#pragma once

/**
 * @file qbuem/pipeline/batch_action.hpp
 * @brief Batch processing action — BatchAction<In, Out>
 * @defgroup qbuem_batch_action BatchAction
 * @ingroup qbuem_pipeline
 *
 * BatchAction is a pipeline stage that collects up to N items at a time and processes them as a batch.
 * Workers accumulate items until `max_batch_size` is reached or the `max_wait_ms` timeout expires,
 * then invoke the processing function.
 *
 * ## Function signature
 * ```cpp
 * Task<Result<std::vector<Out>>>(std::vector<In> batch, ActionEnv env)
 * ```
 *
 * ## Context propagation
 * Each output item is assigned the context of the **first** item in the batch.
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/service_registry.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace qbuem {

/**
 * @brief Batch processing action — calls the processing function with up to N items at a time.
 *
 * @tparam In  Input item type.
 * @tparam Out Output item type.
 */
template <typename In, typename Out>
class BatchAction {
public:
  /** @brief Normalized batch processing function type. */
  using Fn = std::function<Task<Result<std::vector<Out>>>(std::vector<In>, ActionEnv)>;

  /**
   * @brief BatchAction configuration struct.
   */
  struct Config {
    size_t max_batch_size = 64;    ///< Maximum batch size
    size_t max_wait_ms    = 10;    ///< Maximum wait time for batch collection (ms)
    size_t workers        = 1;     ///< Number of workers
    size_t channel_cap    = 1024;  ///< Input channel capacity
    ServiceRegistry* registry = nullptr; ///< Pipeline ServiceRegistry
  };

  /**
   * @brief Creates a BatchAction.
   *
   * Processing function signature: `Task<Result<std::vector<Out>>>(std::vector<In>, ActionEnv)`
   *
   * @tparam FnT Processing function type.
   * @param fn   Batch processing function.
   * @param cfg  Configuration.
   */
  template <typename FnT>
    requires requires(FnT f, std::vector<In> v, ActionEnv e) {
      { f(std::move(v), e) } -> std::same_as<Task<Result<std::vector<Out>>>>;
    }
  BatchAction(FnT fn, Config cfg = {})
      : fn_(std::move(fn)),
        cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<In>>>(cfg_.channel_cap)) {}

  BatchAction(const BatchAction&) = delete;
  BatchAction& operator=(const BatchAction&) = delete;
  BatchAction(BatchAction&&) = default;
  BatchAction& operator=(BatchAction&&) = default;

  // -------------------------------------------------------------------------
  // Item submission
  // -------------------------------------------------------------------------

  /**
   * @brief Pushes an item into the input channel (backpressure).
   *
   * @param item Item to process.
   * @param ctx  Item context.
   */
  Task<Result<void>> push(In item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Attempts to push an item non-blocking.
   *
   * @returns true on success, false if the channel is full.
   */
  bool try_push(In item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  // -------------------------------------------------------------------------
  // Lifecycle
  // -------------------------------------------------------------------------

  /**
   * @brief Starts the BatchAction — spawns worker coroutines on the Dispatcher.
   *
   * @param dispatcher Dispatcher to run coroutines on.
   * @param out        Output channel (results are discarded if nullptr).
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
    out_channel_ = out ? out : std::make_shared<AsyncChannel<ContextualItem<Out>>>(cfg_.channel_cap);
    stop_src_    = std::make_unique<std::stop_source>();

    for (size_t i = 0; i < cfg_.workers; ++i) {
      worker_count_.fetch_add(1, std::memory_order_relaxed);
      dispatcher.spawn(worker_loop(i));
    }
  }

  /**
   * @brief Closes the input channel and waits until all workers have completed.
   *
   * The output channel is automatically closed after drain() returns.
   */
  Task<void> drain() {
    in_channel_->close();
    while (worker_count_.load(std::memory_order_acquire) > 0) {
      struct Yield {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
          if (auto* r = Reactor::current())
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
   * @brief Stops the BatchAction immediately (sends a cancellation signal).
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    in_channel_->close();
  }

  /**
   * @brief Returns the output channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
    return out_channel_;
  }

  /**
   * @brief Returns the input channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<In>>> input() const {
    return in_channel_;
  }

private:
  // -------------------------------------------------------------------------
  // Worker loop
  // -------------------------------------------------------------------------

  /**
   * @brief Batch collection and processing worker loop.
   *
   * Batch collection strategy:
   * 1. Wait for the first item via blocking recv() (exit on EOS).
   * 2. Collect additional items via try_recv() within deadline(max_wait_ms).
   * 3. Process immediately when max_batch_size is reached or deadline expires.
   */
  Task<void> worker_loop(size_t worker_idx) {
    auto stop_token = stop_src_ ? stop_src_->get_token() : std::stop_token{};

    for (;;) {
      if (stop_token.stop_requested()) break;

      // --- Collect batch ---
      std::vector<ContextualItem<In>> batch_items;
      batch_items.reserve(cfg_.max_batch_size);

      // First item: wait via blocking recv (detects EOS)
      auto first = co_await in_channel_->recv();
      if (!first) break; // EOS
      batch_items.push_back(std::move(*first));

      // Remaining items: collect non-blocking within max_wait_ms
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(cfg_.max_wait_ms);

      while (batch_items.size() < cfg_.max_batch_size) {
        auto item = in_channel_->try_recv();
        if (item) {
          batch_items.push_back(std::move(*item));
          continue;
        }

        // Channel closed — process what has been collected so far
        if (in_channel_->is_closed()) break;

        // Timeout — process immediately
        if (std::chrono::steady_clock::now() >= deadline) break;

        // Yield control to the reactor: give other coroutines a chance to produce items
        struct Yield {
          bool await_ready() noexcept { return false; }
          void await_suspend(std::coroutine_handle<> h) noexcept {
            if (auto* r = Reactor::current())
              r->post([h]() mutable { h.resume(); });
            else
              h.resume();
          }
          void await_resume() noexcept {}
        };
        co_await Yield{};

        if (std::chrono::steady_clock::now() >= deadline) break;
      }

      // --- Split batch: value vector + first context ---
      Context first_ctx = batch_items.front().ctx;
      std::vector<In> values;
      values.reserve(batch_items.size());
      for (auto& ci : batch_items)
        values.push_back(std::move(ci.value));

      // --- Build ActionEnv ---
      ActionEnv env{
          .ctx        = first_ctx,
          .stop       = stop_token,
          .worker_idx = worker_idx,
          .registry   = cfg_.registry ? cfg_.registry : &global_registry(),
      };

      // --- Invoke processing function ---
      auto result = co_await fn_(std::move(values), env);

      // --- Forward results to the output channel ---
      // Assign the context of the first input item to all output items
      if (result.has_value() && out_channel_) {
        for (auto& out_item : *result) {
          auto send_r = co_await out_channel_->send(
              ContextualItem<Out>{std::move(out_item), first_ctx});
          if (!send_r.has_value())
            break; // Channel closed — stop sending
        }
      }
      // On error the batch is dropped (DLQ support planned for a future version)
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
};

} // namespace qbuem

/** @} */
