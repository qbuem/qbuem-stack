#pragma once

/**
 * @file qbuem/pipeline/task_group.hpp
 * @brief Structured concurrency — TaskGroup (Nursery pattern)
 * @defgroup qbuem_task_group TaskGroup
 * @ingroup qbuem_pipeline
 *
 * TaskGroup is a structured concurrency primitive that ties child coroutine
 * lifetimes to the parent.
 *
 * ## Guarantees
 * - The parent coroutine does not return before `join()` / `join_all<T>()` completes.
 * - If any child fails, a cancellation signal is sent to the remaining children
 *   and the error is propagated.
 * - All child coroutine frames are destroyed after `join()` completes.
 *
 * ## Usage example
 * @code
 * TaskGroup tg;
 * tg.spawn(task_a(env));
 * tg.spawn(task_b(env));
 * co_await tg.join();  // wait for all to complete
 *
 * // Collect results:
 * TaskGroup tg2;
 * tg2.spawn<int>(compute_a());
 * tg2.spawn<int>(compute_b());
 * auto res = co_await tg2.join_all<int>(); // Result<vector<int>>
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <any>
#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace qbuem {

/**
 * @brief Structured concurrency — manages a group of child coroutines.
 *
 * TaskGroup is non-copyable.
 * Create it on the stack or heap, call `spawn()`, then `co_await` `join()`/`join_all()`.
 */
class TaskGroup {
public:
  TaskGroup() : state_(std::make_shared<State>()) {}
  ~TaskGroup() = default;

  TaskGroup(const TaskGroup &) = delete;
  TaskGroup &operator=(const TaskGroup &) = delete;
  TaskGroup(TaskGroup &&) = default;
  TaskGroup &operator=(TaskGroup &&) = default;

  // -------------------------------------------------------------------------
  // spawn overloads
  // -------------------------------------------------------------------------

  /**
   * @brief Add a void coroutine Task to the group.
   *
   * The Task is executed via post() on the current Reactor.
   *
   * @param task Task<Result<void>> to run. Ownership is transferred.
   */
  void spawn(Task<Result<void>> task) {
    state_->pending.fetch_add(1, std::memory_order_relaxed);
    launch(run_void(std::move(task), state_));
  }

  /**
   * @brief Add a result-returning coroutine Task to the group.
   *
   * @tparam T Result type.
   * @param task Task<Result<T>> to run.
   */
  template <typename T>
  void spawn(Task<Result<T>> task) {
    state_->pending.fetch_add(1, std::memory_order_relaxed);
    launch(run_typed<T>(std::move(task), state_));
  }

  // -------------------------------------------------------------------------
  // join
  // -------------------------------------------------------------------------

  /**
   * @brief Wait until all child coroutines have completed.
   *
   * If any child fails, the remaining children are cancelled and the first
   * error is returned.
   *
   * @returns `Result<void>{}` on success, or the first error code on failure.
   */
  Task<Result<void>> join() {
    co_return co_await JoinAwaiter{state_, Reactor::current()};
  }

  /**
   * @brief Collect results from all children.
   *
   * @tparam T Result type (must match the spawn<T>() calls).
   * @returns `Result<std::vector<T>>`.
   */
  template <typename T>
  Task<Result<std::vector<T>>> join_all() {
    auto r = co_await JoinAwaiter{state_, Reactor::current()};
    if (!r.has_value())
      co_return unexpected(r.error());

    std::vector<T> results;
    std::lock_guard lock(state_->results_mutex);
    results.reserve(state_->results.size());
    for (auto &a : state_->results)
      results.push_back(std::any_cast<T>(std::move(a)));
    co_return results;
  }

  /**
   * @brief Send a cancellation signal to all children.
   */
  void cancel() { state_->stop_src.request_stop(); }

  /**
   * @brief Return the child cancellation stop token.
   */
  [[nodiscard]] std::stop_token stop_token() const noexcept {
    return state_->stop_src.get_token();
  }

private:
  // -------------------------------------------------------------------------
  // Shared state between TaskGroup and all child wrappers
  // -------------------------------------------------------------------------
  struct State {
    std::atomic<size_t>      pending{0};
    std::atomic<bool>        has_error{false};
    std::error_code          first_error;
    std::mutex               error_mutex;

    std::stop_source         stop_src;

    // Typed results (join_all)
    std::mutex               results_mutex;
    std::vector<std::any>    results;

    // join() awaiter
    std::mutex               waiters_mutex;
    std::coroutine_handle<>  join_waiter;
    Reactor                 *join_reactor = nullptr;

    void notify_done(std::error_code ec) {
      if (ec && !has_error.exchange(true, std::memory_order_relaxed)) {
        std::lock_guard lock(error_mutex);
        first_error = ec;
        stop_src.request_stop();
      }
      size_t rem = pending.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (rem == 0)
        wake_join_waiter();
    }

    void wake_join_waiter() {
      std::coroutine_handle<> waiter;
      Reactor *r = nullptr;
      {
        std::lock_guard lock(waiters_mutex);
        waiter = join_waiter;
        r      = join_reactor;
        join_waiter  = {};
        join_reactor = nullptr;
      }
      if (!waiter)
        return;
      if (r)
        r->post([waiter]() mutable { waiter.resume(); });
      else
        waiter.resume();
    }
  };

  std::shared_ptr<State> state_;

  // -------------------------------------------------------------------------
  // Launch a wrapper coroutine via Reactor::post()
  // -------------------------------------------------------------------------
  static void launch(Task<void> wrapper) {
    auto h = wrapper.handle;
    wrapper.detach();
    Reactor *r = Reactor::current();
    if (r)
      r->post([h]() mutable { h.resume(); });
    else
      h.resume();
  }

  // -------------------------------------------------------------------------
  // Wrapper coroutines
  // -------------------------------------------------------------------------

  static Task<void> run_void(Task<Result<void>> task,
                             std::shared_ptr<State> st) {
    auto result = co_await task;
    std::error_code ec;
    if (!result.has_value())
      ec = result.error();
    st->notify_done(ec);
    co_return;
  }

  template <typename T>
  static Task<void> run_typed(Task<Result<T>> task,
                              std::shared_ptr<State> st) {
    auto result = co_await task;
    if (result.has_value()) {
      std::lock_guard lock(st->results_mutex);
      st->results.emplace_back(*std::move(result));
    }
    std::error_code ec;
    if (!result.has_value())
      ec = result.error();
    st->notify_done(ec);
    co_return;
  }

  // -------------------------------------------------------------------------
  // join() awaiter
  // -------------------------------------------------------------------------
  struct JoinAwaiter {
    std::shared_ptr<State> state;
    Reactor               *reactor;

    bool await_ready() const noexcept {
      return state->pending.load(std::memory_order_acquire) == 0;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      std::lock_guard lock(state->waiters_mutex);
      // Double-check: may have reached zero between await_ready and here
      if (state->pending.load(std::memory_order_acquire) == 0)
        return false; // don't suspend, resume immediately
      state->join_waiter  = h;
      state->join_reactor = reactor;
      return true;
    }

    Result<void> await_resume() const {
      if (state->has_error.load(std::memory_order_acquire))
        return unexpected(state->first_error);
      return Result<void>{};
    }
  };
};

} // namespace qbuem

/** @} */
