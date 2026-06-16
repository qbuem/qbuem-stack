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
      : state_(std::make_shared<State>()) {
    state_->fn         = std::move(fn);
    state_->cfg        = std::move(cfg);
    state_->in_channel = std::make_shared<AsyncChannel<ContextualItem<In>>>(
        state_->cfg.channel_cap);
  }

  BatchAction(const BatchAction&) = delete;
  BatchAction& operator=(const BatchAction&) = delete;
  BatchAction(BatchAction&&) = default;
  BatchAction& operator=(BatchAction&&) = default;

  /**
   * @brief Close the input channel so in-flight workers drain instead of leaking.
   *
   * Workers are coroutines on a Dispatcher and cannot be block-joined here; each
   * holds a shared_ptr to the State, so destroying this handle never frees the
   * State out from under a running worker. Closing the input makes recv() return
   * EOS so workers run to completion on their own. For deterministic shutdown,
   * `co_await drain()` before destruction.
   */
  ~BatchAction() {
    if (state_ && state_->stop_src) {  // started
      state_->stop_src->request_stop();
      if (state_->in_channel) state_->in_channel->close();
    }
  }

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
    co_return co_await state_->in_channel->send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Attempts to push an item non-blocking.
   *
   * @returns true on success, false if the channel is full.
   */
  bool try_push(In item, Context ctx = {}) {
    return state_->in_channel->try_send(
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
    state_->out_channel = out ? out
        : std::make_shared<AsyncChannel<ContextualItem<Out>>>(state_->cfg.channel_cap);
    state_->stop_src    = std::make_unique<std::stop_source>();

    for (size_t i = 0; i < state_->cfg.workers; ++i) {
      state_->worker_count.fetch_add(1, std::memory_order_relaxed);
      // Pass the State shared_ptr by value into the static worker: it is copied
      // into the coroutine frame, keeping the State alive for the worker's whole
      // life regardless of when this handle is destroyed.
      dispatcher.spawn(worker_loop(state_, i));
    }
  }

  /**
   * @brief Closes the input channel and waits until all workers have completed.
   *
   * The output channel is automatically closed after drain() returns.
   */
  Task<void> drain() {
    state_->in_channel->close();
    while (state_->worker_count.load(std::memory_order_acquire) > 0) {
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
    if (state_->out_channel)
      state_->out_channel->close();
    co_return;
  }

  /**
   * @brief Stops the BatchAction immediately (sends a cancellation signal).
   */
  void stop() {
    if (state_->stop_src) state_->stop_src->request_stop();
    state_->in_channel->close();
  }

  /**
   * @brief Returns the output channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
    return state_->out_channel;
  }

  /**
   * @brief Returns the input channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<In>>> input() const {
    return state_->in_channel;
  }

private:
  // -------------------------------------------------------------------------
  // Shared state — outlives the handle so detached workers never dangle.
  // -------------------------------------------------------------------------
  struct State {
    Fn                                                 fn;
    Config                                             cfg;
    std::shared_ptr<AsyncChannel<ContextualItem<In>>>  in_channel;
    std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out_channel;
    std::unique_ptr<std::stop_source>                  stop_src;
    std::atomic<size_t>                                worker_count{0};
  };

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
  // static + self-contained: depends only on the State it is handed (kept alive
  // by the shared_ptr in the coroutine frame), never the BatchAction handle — so
  // a worker can safely outlive the handle that spawned it.
  static Task<void> worker_loop(std::shared_ptr<State> st, size_t worker_idx) {
    auto stop_token = st->stop_src ? st->stop_src->get_token() : std::stop_token{};

    for (;;) {
      if (stop_token.stop_requested()) break;

      // --- Collect batch ---
      std::vector<ContextualItem<In>> batch_items;
      batch_items.reserve(st->cfg.max_batch_size);

      // First item: wait via blocking recv (detects EOS)
      auto first = co_await st->in_channel->recv();
      if (!first) break; // EOS
      batch_items.push_back(std::move(*first));

      // Remaining items: collect non-blocking within max_wait_ms
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(st->cfg.max_wait_ms);

      while (batch_items.size() < st->cfg.max_batch_size) {
        auto item = st->in_channel->try_recv();
        if (item) {
          batch_items.push_back(std::move(*item));
          continue;
        }

        // Channel closed — process what has been collected so far
        if (st->in_channel->is_closed()) break;

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
          .registry   = st->cfg.registry ? st->cfg.registry : &global_registry(),
      };

      // --- Invoke processing function ---
      auto result = co_await st->fn(std::move(values), env);

      // --- Forward results to the output channel ---
      // Assign the context of the first input item to all output items
      if (result.has_value() && st->out_channel) {
        for (auto& out_item : *result) {
          auto send_r = co_await st->out_channel->send(
              ContextualItem<Out>{std::move(out_item), first_ctx});
          if (!send_r.has_value())
            break; // Channel closed — stop sending
        }
      }
      // On error the batch is dropped (DLQ support planned for a future version)
    }

    size_t remaining = st->worker_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0 && st->out_channel)
      st->out_channel->close();
    co_return;
  }

  std::shared_ptr<State> state_;
};

} // namespace qbuem

/** @} */
