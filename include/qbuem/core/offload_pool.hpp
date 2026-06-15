#pragma once

/**
 * @file qbuem/core/offload_pool.hpp
 * @brief Worker-thread pool for offloading blocking / CPU-bound work off reactors.
 * @ingroup qbuem_core
 *
 * The reactor threads must never block (Pillar 1, L1: no blocking syscalls on a
 * reactor thread). But real systems still need to run synchronous third-party
 * libraries, CPU-bound transforms (compression, hashing large buffers, image /
 * media encoding), or file I/O on platforms without io_uring. `OffloadPool` is
 * the sanctioned home for that work.
 *
 * From inside a reactor coroutine:
 * @code
 * OffloadPool pool;                      // create once, share across the app
 *
 * Task<Result<void>> handler(Request req, std::stop_token) {
 *   // Heavy/blocking work runs on a pool thread; the reactor keeps serving
 *   // other connections. The coroutine resumes on its ORIGINAL reactor.
 *   std::string digest = co_await pool.run([buf = std::move(req).body()] {
 *     return expensive_blocking_hash(buf);   // safe: not on a reactor thread
 *   });
 *   co_return Response{}.body(digest);
 * }
 * @endcode
 *
 * ### Mechanics
 * `co_await pool.run(fn)`:
 *  1. captures the awaiting coroutine's current Reactor (`Reactor::current()`),
 *  2. submits `fn` to a pool worker — runs it without blocking any reactor,
 *  3. on completion, posts the coroutine's resume back to its origin reactor
 *     (so reactor affinity is preserved; no cross-reactor resume), and
 *  4. hands `fn()`'s return value back to the awaiting coroutine.
 *
 * ### Lifetime
 * The pool must outlive every in-flight `run()` whose coroutine has not yet
 * resumed. Shut the pool down (or let it destruct) only after the reactors that
 * own those coroutines have drained — otherwise a queued resume would target a
 * dead reactor. `shutdown()` drains and runs every already-queued job before
 * joining, so pending resumes are still posted.
 *
 * The pool's queue uses a mutex + condition variable: that is appropriate here
 * because the work being offloaded is, by definition, not latency-critical — the
 * point is to keep it OFF the latency-critical reactor threads.
 */

#include <qbuem/core/reactor.hpp>

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace qbuem {

class OffloadPool;

/**
 * @brief Awaitable produced by `OffloadPool::run()`. Runs a callable on a pool
 *        thread and resumes the awaiting coroutine on its origin reactor.
 * @tparam F Nullary callable. Its result type `R` is returned by `co_await`.
 */
template <typename F>
struct [[nodiscard]] OffloadAwaiter {
  using R = std::invoke_result_t<F &>;

  OffloadPool         *pool;
  F                    fn;
  Reactor             *origin = nullptr;
  std::coroutine_handle<> waiter{};
  struct Empty {};
  std::conditional_t<std::is_void_v<R>, Empty, std::optional<R>> result{};

  bool await_ready() const noexcept { return false; }

  // Defined out of line below (needs the full OffloadPool definition).
  void await_suspend(std::coroutine_handle<> h);

  R await_resume() {
    if constexpr (std::is_void_v<R>)
      return;
    else
      return std::move(*result);
  }
};

/**
 * @brief Fixed-size pool of worker threads for offloaded blocking / CPU work.
 *
 * Construct one and share it; submit work via `run()` (awaitable, preferred) or
 * `submit()` (fire-and-forget). Not copyable or movable.
 */
class OffloadPool {
public:
  /**
   * @brief Construct the pool.
   * @param threads Worker thread count. Defaults to hardware concurrency
   *                (CPU-bound work scales with cores); minimum 1.
   */
  explicit OffloadPool(std::size_t threads = default_threads()) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i)
      workers_.emplace_back([this] { worker_loop(); });
  }

  ~OffloadPool() { shutdown(); }

  OffloadPool(const OffloadPool &)            = delete;
  OffloadPool &operator=(const OffloadPool &) = delete;
  OffloadPool(OffloadPool &&)                 = delete;
  OffloadPool &operator=(OffloadPool &&)      = delete;

  /// @brief Default worker count: the hardware concurrency (min 1).
  static std::size_t default_threads() noexcept {
    std::size_t n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
  }

  /// @brief Number of worker threads.
  [[nodiscard]] std::size_t thread_count() const noexcept {
    return workers_.size();
  }

  /**
   * @brief Enqueue a fire-and-forget job to run on a pool thread.
   *
   * Prefer `run()` from a coroutine. Use `submit()` only when you do not need
   * the result and are not resuming a coroutine.
   */
  void submit(std::function<void()> job) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) return; // pool is shutting down; drop late submissions
      queue_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

  /**
   * @brief Run @p fn on a pool thread; resume the caller on its origin reactor.
   *
   * @return An awaitable whose `co_await` yields `fn()`'s result. Must be
   *         awaited from a reactor coroutine for correct resume affinity.
   */
  template <typename F>
  [[nodiscard]] OffloadAwaiter<F> run(F fn) {
    return OffloadAwaiter<F>{.pool = this, .fn = std::move(fn)};
  }

  /**
   * @brief Stop accepting new work, run everything already queued, then join.
   *
   * Idempotent. Called automatically by the destructor.
   */
  void shutdown() {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) return;
      stopping_ = true;
    }
    cv_.notify_all();
    workers_.clear(); // std::jthread destructors join each worker
  }

private:
  void worker_loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) return; // stopping_ && drained
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      job();
    }
  }

  std::mutex                        mutex_;
  std::condition_variable           cv_;
  std::deque<std::function<void()>> queue_;
  bool                              stopping_ = false;
  std::vector<std::jthread>         workers_;
};

// ─── OffloadAwaiter::await_suspend (needs the full OffloadPool) ───────────────
template <typename F>
void OffloadAwaiter<F>::await_suspend(std::coroutine_handle<> h) {
  waiter = h;
  origin = Reactor::current(); // captured on the reactor thread, before offload
  pool->submit([this] {
    if constexpr (std::is_void_v<R>)
      fn();
    else
      result.emplace(fn());
    // Resume on the origin reactor (no cross-reactor resume). If there is no
    // reactor (awaited off a reactor thread, e.g. in a unit test), resume here.
    auto w = waiter;
    if (origin)
      origin->post([w]() mutable { w.resume(); });
    else
      w.resume();
  });
}

} // namespace qbuem
