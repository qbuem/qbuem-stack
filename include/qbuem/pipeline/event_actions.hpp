#pragma once

/**
 * @file qbuem/pipeline/event_actions.hpp
 * @brief Advanced event-processing actions — DebounceAction, ThrottleAction, ScatterGatherAction
 * @defgroup qbuem_event_actions EventActions
 * @ingroup qbuem_pipeline
 *
 * ## Included components
 *
 * ### DebounceAction<T>
 * Emits the last item after a silence period (gap) with no new arrivals.
 * Useful for waiting for a stable interval after an event burst.
 *
 * ### ThrottleAction<T>
 * Limits throughput using a token-bucket algorithm.
 * Supports `rate_per_sec` rate and `burst` burst capacity.
 *
 * ### ScatterGatherAction<In, SubIn, SubOut, Out>
 * Scatters one input into multiple SubIn items,
 * processes each in parallel, then gathers the results into a single Out.
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
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

namespace qbuem {

// ============================================================================
// DebounceAction<T>
// ============================================================================

/**
 * @brief Debounce action — emit the last item after a static silence gap.
 *
 * Resets the timer each time a new item arrives.
 * Forwards the last item to the output channel once no new item has arrived
 * within the `gap` window.
 *
 * @tparam T Item type.
 */
template <typename T>
class DebounceAction {
public:
  /**
   * @brief DebounceAction configuration.
   */
  struct Config {
    std::chrono::milliseconds gap{100}; ///< Silence gap (quiet time)
    size_t channel_cap = 256;           ///< Input channel capacity
  };

  /**
   * @brief Construct a DebounceAction.
   *
   * @param cfg Configuration.
   */
  explicit DebounceAction(Config cfg = {})
      : cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<T>>>(
            cfg_.channel_cap)) {}

  DebounceAction(const DebounceAction&) = delete;
  DebounceAction& operator=(const DebounceAction&) = delete;
  DebounceAction(DebounceAction&&)                 = default;
  DebounceAction& operator=(DebounceAction&&)      = default;

  /**
   * @brief Push an item into the input channel (with backpressure).
   */
  Task<Result<void>> push(T item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<T>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Attempt to push an item in a non-blocking manner.
   */
  bool try_push(T item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<T>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Start the DebounceAction.
   *
   * @param dispatcher Dispatcher on which to run the coroutine.
   * @param out        Output channel.
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<T>>> out = nullptr) {
    out_channel_ = out;
    stop_src_    = std::make_unique<std::stop_source>();
    dispatcher.spawn(worker_loop());
  }

  /**
   * @brief Close the input channel and wait for the worker to finish.
   */
  Task<void> drain() {
    in_channel_->close();
    while (running_.load(std::memory_order_acquire)) {
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
   * @brief Stop immediately.
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    in_channel_->close();
  }

  /**
   * @brief Return the output channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> output() const {
    return out_channel_;
  }

  /**
   * @brief Return the input channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> input() const {
    return in_channel_;
  }

private:
  /**
   * @brief Debounce worker loop.
   *
   * Algorithm:
   * 1. Wait for an item via blocking recv().
   * 2. Update the deadline whenever a new item arrives.
   * 3. Forward the last item when no new item arrives before the deadline.
   */
  Task<void> worker_loop() {
    running_.store(true, std::memory_order_release);

    std::optional<ContextualItem<T>> pending;
    auto deadline = std::chrono::steady_clock::time_point::max();

    for (;;) {
      // If there is a pending item, poll try_recv until the deadline
      if (pending) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          // Gap elapsed — forward the last item
          if (out_channel_) {
            co_await out_channel_->send(
                ContextualItem<T>{std::move(pending->value), pending->ctx});
          }
          pending.reset();
          deadline = std::chrono::steady_clock::time_point::max();
          continue;
        }

        // Check for a new item in a non-blocking manner
        auto item = in_channel_->try_recv();
        if (item) {
          // New item arrived — refresh deadline
          pending  = std::move(*item);
          deadline = std::chrono::steady_clock::now() +
                     std::chrono::duration_cast<std::chrono::nanoseconds>(cfg_.gap);
          continue;
        }

        if (in_channel_->is_closed()) break;

        // Yield — give the Reactor a chance to run other coroutines
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
      } else {
        // No pending item — blocking recv
        auto item = co_await in_channel_->recv();
        if (!item) break; // EOS
        pending  = std::move(*item);
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::duration_cast<std::chrono::nanoseconds>(cfg_.gap);
      }
    }

    // On EOS, flush any remaining pending item
    if (pending && out_channel_) {
      co_await out_channel_->send(
          ContextualItem<T>{std::move(pending->value), pending->ctx});
    }

    running_.store(false, std::memory_order_release);
    co_return;
  }

  Config                                                  cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>        in_channel_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>        out_channel_;
  std::unique_ptr<std::stop_source>                       stop_src_;
  std::atomic<bool>                                       running_{false};
};

// ============================================================================
// ThrottleAction<T>
// ============================================================================

/**
 * @brief Throttle action — token-bucket throughput limiter.
 *
 * Passes items through at a rate of `rate_per_sec` tokens per second.
 * Allows instantaneous bursts of up to `burst` items.
 *
 * Waits until enough tokens have accumulated when the bucket is empty.
 *
 * @tparam T Item type.
 */
template <typename T>
class ThrottleAction {
public:
  /**
   * @brief ThrottleAction configuration.
   */
  struct Config {
    size_t rate_per_sec = 1000; ///< Items processed per second
    size_t burst        = 100;  ///< Burst capacity
    size_t channel_cap  = 1024; ///< Input channel capacity
  };

  /**
   * @brief Construct a ThrottleAction.
   *
   * @param cfg Configuration.
   */
  explicit ThrottleAction(Config cfg = {})
      : cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<T>>>(
            cfg_.channel_cap)),
        tokens_(static_cast<double>(cfg_.burst)) {}

  ThrottleAction(const ThrottleAction&) = delete;
  ThrottleAction& operator=(const ThrottleAction&) = delete;
  ThrottleAction(ThrottleAction&&)                 = default;
  ThrottleAction& operator=(ThrottleAction&&)      = default;

  /**
   * @brief Push an item into the input channel (with backpressure).
   */
  Task<Result<void>> push(T item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<T>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Attempt to push an item in a non-blocking manner.
   */
  bool try_push(T item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<T>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Start the ThrottleAction.
   *
   * @param dispatcher Dispatcher on which to run the coroutine.
   * @param out        Output channel.
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<T>>> out = nullptr) {
    out_channel_ = out;
    stop_src_    = std::make_unique<std::stop_source>();
    last_refill_ = std::chrono::steady_clock::now();
    dispatcher.spawn(worker_loop());
  }

  /**
   * @brief Close the input channel and wait for the worker to finish.
   */
  Task<void> drain() {
    in_channel_->close();
    while (running_.load(std::memory_order_acquire)) {
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
   * @brief Stop immediately.
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    in_channel_->close();
  }

  /**
   * @brief Return the output channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> output() const {
    return out_channel_;
  }

  /**
   * @brief Return the input channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> input() const {
    return in_channel_;
  }

private:
  /**
   * @brief Refill tokens proportional to elapsed time.
   *
   * Caps the token count at burst capacity.
   */
  void refill_tokens() {
    auto now = std::chrono::steady_clock::now();
    double elapsed_sec =
        std::chrono::duration<double>(now - last_refill_).count();
    last_refill_ = now;

    tokens_ += elapsed_sec * static_cast<double>(cfg_.rate_per_sec);
    double max_tokens = static_cast<double>(cfg_.burst);
    if (tokens_ > max_tokens)
      tokens_ = max_tokens;
  }

  /**
   * @brief Throttle worker loop.
   *
   * Token bucket algorithm:
   * 1. Receive an item via recv().
   * 2. Refill tokens.
   * 3. If tokens >= 1, consume a token and forward immediately.
   * 4. If insufficient tokens, yield and retry.
   */
  Task<void> worker_loop() {
    running_.store(true, std::memory_order_release);
    auto stop_token = stop_src_ ? stop_src_->get_token() : std::stop_token{};

    for (;;) {
      if (stop_token.stop_requested()) break;

      auto citem = co_await in_channel_->recv();
      if (!citem) break; // EOS

      // Wait until a token is available
      for (;;) {
        refill_tokens();
        if (tokens_ >= 1.0) {
          tokens_ -= 1.0;
          break; // Token acquired
        }

        // Wait: yield for the time needed to generate one token
        // (time to generate 1 token = 1 / rate_per_sec seconds)
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

      if (out_channel_) {
        co_await out_channel_->send(std::move(*citem));
      }
    }

    running_.store(false, std::memory_order_release);
    co_return;
  }

  Config                                                  cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>        in_channel_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>        out_channel_;
  std::unique_ptr<std::stop_source>                       stop_src_;
  std::atomic<bool>                                       running_{false};
  // Token bucket state (accessed only by the worker coroutine — single worker, no sync needed)
  double                                                  tokens_{0.0};
  std::chrono::steady_clock::time_point                   last_refill_;
};

// ============================================================================
// ScatterGatherAction<In, SubIn, SubOut, Out>
// ============================================================================

/**
 * @brief Scatter-gather action — distribute then aggregate.
 *
 * Processing flow:
 * 1. **Scatter**: Split one `In` into multiple `SubIn` items.
 * 2. **Process**: Process each `SubIn` in parallel to produce a `SubOut`.
 * 3. **Gather**: Aggregate all `SubOut` items into a single `Out`.
 *
 * @tparam In     Original input type.
 * @tparam SubIn  Distributed sub-task input type.
 * @tparam SubOut Sub-task output type.
 * @tparam Out    Aggregated final output type.
 */
template <typename In, typename SubIn, typename SubOut, typename Out>
class ScatterGatherAction {
public:
  /** @brief Scatter function type: `In → vector<SubIn>`. */
  using ScatterFn = std::function<std::vector<SubIn>(In)>;
  /** @brief Sub-task processing function type: `(SubIn, ActionEnv) → Task<Result<SubOut>>`. */
  using ProcessFn = std::function<Task<Result<SubOut>>(SubIn, ActionEnv)>;
  /** @brief Gather function type: `(In, vector<SubOut>) → Out`. */
  using GatherFn  = std::function<Out(In, std::vector<SubOut>)>;

  /**
   * @brief ScatterGatherAction configuration.
   */
  struct Config {
    size_t max_parallel = 8;    ///< Maximum number of parallel sub-tasks
    size_t channel_cap  = 256;  ///< Input channel capacity
    ServiceRegistry* registry = nullptr; ///< Pipeline ServiceRegistry
  };

  /**
   * @brief Construct a ScatterGatherAction.
   *
   * @param scatter Scatter function.
   * @param process Sub-task processing function.
   * @param gather  Gather function.
   * @param cfg     Configuration.
   */
  ScatterGatherAction(ScatterFn scatter, ProcessFn process, GatherFn gather,
                      Config cfg = {})
      : scatter_(std::move(scatter)),
        process_(std::move(process)),
        gather_(std::move(gather)),
        cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<In>>>(
            cfg_.channel_cap)) {}

  ScatterGatherAction(const ScatterGatherAction&) = delete;
  ScatterGatherAction& operator=(const ScatterGatherAction&) = delete;
  ScatterGatherAction(ScatterGatherAction&&)                 = default;
  ScatterGatherAction& operator=(ScatterGatherAction&&)      = default;

  /**
   * @brief Push an item into the input channel (with backpressure).
   */
  Task<Result<void>> push(In item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Attempt to push an item in a non-blocking manner.
   */
  bool try_push(In item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Start the ScatterGatherAction.
   *
   * @param dispatcher Dispatcher on which to run the coroutine.
   * @param out        Output channel.
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
    out_channel_ = out;
    stop_src_    = std::make_unique<std::stop_source>();
    dispatcher.spawn(worker_loop());
  }

  /**
   * @brief Close the input channel and wait for the worker to finish.
   */
  Task<void> drain() {
    in_channel_->close();
    while (running_.load(std::memory_order_acquire)) {
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
   * @brief Stop immediately.
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    in_channel_->close();
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
  /**
   * @brief Coroutine to process a single SubIn item.
   *
   * Atomically writes the result to the shared result vector and
   * decrements the completion counter.
   */
  struct SubTask {
    static Task<void> run(
        SubIn item,
        size_t idx,
        ActionEnv env,
        std::shared_ptr<std::vector<std::optional<SubOut>>> results,
        std::shared_ptr<std::atomic<size_t>> pending,
        std::shared_ptr<AsyncChannel<int>> done_chan,
        std::function<Task<Result<SubOut>>(SubIn, ActionEnv)> process) {
      auto result = co_await process(std::move(item), env);
      if (result.has_value())
        (*results)[idx] = std::move(*result);
      // Signal done when the last sub-task completes
      size_t remaining = pending->fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (remaining == 0)
        done_chan->try_send(0);
      co_return;
    }
  };

  /**
   * @brief Scatter-gather worker loop.
   *
   * For each input item:
   * 1. Call scatter() to produce the SubIn list.
   * 2. Process each SubIn in parallel coroutines (up to max_parallel).
   * 3. Wait for all SubIn items to finish processing.
   * 4. Call gather() to aggregate the results and emit the output.
   */
  Task<void> worker_loop() {
    running_.store(true, std::memory_order_release);
    auto stop_token = stop_src_ ? stop_src_->get_token() : std::stop_token{};
    size_t worker_idx = 0;

    for (;;) {
      if (stop_token.stop_requested()) break;

      auto citem = co_await in_channel_->recv();
      if (!citem) break; // EOS

      // Save original value (needed by gather)
      In orig_value = citem->value;
      Context orig_ctx = citem->ctx;

      // 1. Scatter
      std::vector<SubIn> sub_items = scatter_(orig_value);
      if (sub_items.empty()) {
        // No sub-tasks — pass empty results to gather
        Out out_val = gather_(std::move(orig_value), {});
        if (out_channel_) {
          co_await out_channel_->send(
              ContextualItem<Out>{std::move(out_val), orig_ctx});
        }
        continue;
      }

      size_t total = sub_items.size();
      auto results = std::make_shared<std::vector<std::optional<SubOut>>>(total);
      auto pending = std::make_shared<std::atomic<size_t>>(total);
      // done_chan: signals when all sub-tasks complete
      auto done_chan = std::make_shared<AsyncChannel<int>>(2);

      // 2. Parallel processing — spawn in batches of max_parallel
      size_t dispatch_count = std::min(total, cfg_.max_parallel);

      ServiceRegistry* reg =
          cfg_.registry ? cfg_.registry : &global_registry();

      for (size_t batch_start = 0; batch_start < total;
           batch_start += dispatch_count) {
        size_t batch_end = std::min(batch_start + dispatch_count, total);
        size_t batch_sz  = batch_end - batch_start;

        auto batch_done    = std::make_shared<AsyncChannel<int>>(2);
        auto batch_pending = std::make_shared<std::atomic<size_t>>(batch_sz);

        for (size_t i = batch_start; i < batch_end; ++i) {
          ActionEnv env{
              .ctx        = orig_ctx,
              .stop       = stop_token,
              .worker_idx = worker_idx + i,
              .registry   = reg,
          };

          auto task = SubTask::run(
              std::move(sub_items[i]), i, env, results, batch_pending,
              batch_done, process_);
          auto h = task.handle;
          task.detach();
          if (auto* r = Reactor::current())
            r->post([h]() mutable { h.resume(); });
        }

        // Wait for the current batch to finish before starting the next one
        co_await batch_done->recv();
      }
      // done_chan is superseded by the batch loop
      (void)done_chan;
      (void)pending;

      // 4. Gather: collect only successful SubOut items
      std::vector<SubOut> sub_results;
      sub_results.reserve(total);
      for (auto& opt : *results) {
        if (opt.has_value())
          sub_results.push_back(std::move(*opt));
      }

      Out out_val = gather_(std::move(orig_value), std::move(sub_results));
      if (out_channel_) {
        co_await out_channel_->send(
            ContextualItem<Out>{std::move(out_val), orig_ctx});
      }
    }

    running_.store(false, std::memory_order_release);
    co_return;
  }

  ScatterFn                                               scatter_;
  ProcessFn                                               process_;
  GatherFn                                                gather_;
  Config                                                  cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<In>>>       in_channel_;
  std::shared_ptr<AsyncChannel<ContextualItem<Out>>>      out_channel_;
  std::unique_ptr<std::stop_source>                       stop_src_;
  std::atomic<bool>                                       running_{false};
};

} // namespace qbuem

/** @} */
