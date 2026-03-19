#pragma once

/**
 * @file qbuem/core/dispatcher.hpp
 * @brief Multi-core event loop manager — owns one Reactor per core
 * @defgroup qbuem_dispatcher Dispatcher
 * @ingroup qbuem_core
 *
 * Dispatcher creates std::thread::hardware_concurrency() Reactors and
 * processes events independently on each core. Registered fds are
 * distributed to worker Reactors in round-robin fashion.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace qbuem {

/**
 * @brief Orchestrator managing multiple Reactor worker threads.
 *
 * Dispatcher is the entry point for concurrency in qbuem-stack.
 * Applications typically create a single Dispatcher, which handles all I/O.
 *
 * ### Core Responsibilities
 * 1. **Thread pool management**: Creates and manages `thread_count` worker threads.
 * 2. **Reactor assignment**: Assigns an independent Reactor to each worker thread.
 * 3. **Listener registration**: Registers incoming fds with specific worker Reactors.
 * 4. **Load distribution**: Selects workers based on fd hash.
 *
 * ### Usage Example
 * @code
 * qbuem::Dispatcher dispatcher; // creates workers equal to CPU core count
 *
 * // Register a listening socket — assigned to the appropriate worker Reactor
 * dispatcher.register_listener(listen_fd, [](int fd) {
 *     // handle new connection accept
 * });
 *
 * dispatcher.run(); // blocking: waits until all worker threads terminate
 * @endcode
 *
 * @note Dispatcher is thread-safe. `stop()` can be called from another thread.
 * @note Dispatcher itself is neither copyable nor movable.
 */
class Dispatcher {
public:
  /**
   * @brief Construct a Dispatcher with the specified number of worker threads.
   *
   * Reactor instances are created at construction, but worker threads do not
   * start until `run()` is called. This separates creation from startup so
   * initial configuration (listener registration, etc.) can occur before the
   * event loop begins.
   *
   * @param thread_count Number of worker threads. Defaults to the hardware
   *                     concurrency count (`std::thread::hardware_concurrency()`).
   *                     Passing 0 creates at least 1 thread.
   */
  explicit Dispatcher(
      size_t thread_count = std::thread::hardware_concurrency());

  /**
   * @brief Start all worker threads and block until they complete.
   *
   * Each worker thread runs the event loop on its own Reactor.
   * Blocks until `stop()` is called or all worker threads terminate.
   *
   * @note This function is typically called at the end of `main()` or the
   *       server entry point.
   */
  void run();

  /**
   * @brief Stop all worker threads and Reactors.
   *
   * Calls `stop()` on each Reactor to terminate its event loop.
   * This function is thread-safe and can be called from another thread.
   *
   * @note After `stop()` is called, `run()` returns once all worker threads finish.
   */
  void stop();

  /**
   * @brief Register an incoming fd with a worker Reactor.
   *
   * Assigns the fd to the appropriate worker Reactor and registers a read
   * event callback on that Reactor. The callback is invoked whenever new
   * data or a connection is available on the fd.
   *
   * The same fd is always assigned to the same worker Reactor (based on
   * fd % thread_count), ensuring processing occurs on the same thread
   * throughout the connection's lifetime.
   *
   * @param fd       File descriptor to watch (usually a listening socket).
   * @param callback Callback to invoke when an event occurs on fd.
   * @returns `Result<void>{}` on success, or an error code on failure.
   *
   * @note Register before calling `run()` to have the fd watched from the
   *       moment the event loop starts.
   */
  Result<void> register_listener(int fd, std::function<void(int)> callback);

  /**
   * @brief Return the worker Reactor assigned to the given fd.
   *
   * Uses the same assignment algorithm as `register_listener()`.
   * Used when direct Reactor access is needed for a specific connection.
   *
   * @param fd File descriptor to query the worker for.
   * @returns Pointer to the Reactor assigned to this fd. nullptr if no workers exist.
   */
  Reactor *get_worker_reactor(int fd);

  /**
   * @brief Return the number of worker threads (= number of Reactors).
   *
   * Used in SO_REUSEPORT multi-socket accept patterns to determine how many
   * per-reactor listening sockets to create.
   */
  size_t thread_count() const noexcept { return reactors_.size(); }

  /**
   * @brief Register an incoming fd with a specific worker Reactor by index.
   *
   * Used in SO_REUSEPORT multi-socket mode so each reactor thread owns a
   * dedicated listening socket. Returns an error if reactor_idx is out of range.
   *
   * @param fd          File descriptor to watch (listening socket).
   * @param reactor_idx Worker index to register with (0 ~ thread_count()-1).
   * @param callback    Callback to invoke when an event occurs.
   * @returns `Result<void>{}` on success, or an error code on failure.
   */
  Result<void> register_listener_at(int fd, size_t reactor_idx,
                                    std::function<void(int)> callback);

  /**
   * @brief Select a worker Reactor in round-robin order and enqueue a callback.
   *
   * Safe to call from multiple threads. The selected Reactor's `post()`
   * implementation wakes the worker thread and executes the callback.
   *
   * @param fn Callback to run on the worker thread.
   */
  void post(std::function<void()> fn);

  /**
   * @brief Enqueue a callback on a specific worker Reactor by index.
   *
   * @param reactor_idx Worker index in the range 0 ~ thread_count()-1.
   * @param fn          Callback to run on the worker thread.
   */
  void post_to(size_t reactor_idx, std::function<void()> fn);

  /**
   * @brief Run a coroutine Task as fire-and-forget on a round-robin worker.
   *
   * Calls `task.detach()` then passes the coroutine handle to a worker thread
   * via `post()`. The frame self-destructs when the coroutine completes.
   *
   * @warning To wake another Reactor thread from within a coroutine, do NOT
   *          call `handle.resume()` directly. Always delegate via the target
   *          Reactor's `post()`.
   *
   * @param task Task<void> to run as fire-and-forget. Ownership is transferred.
   */
  void spawn(Task<void> task);

  /**
   * @brief Task<Result<void>> overload — ignores the result and runs fire-and-forget.
   *
   * Converts Task<Result<void>> to Task<void> and spawns it.
   * Errors inside the coroutine are silently discarded (handle them explicitly if logging is needed).
   */
  void spawn(Task<Result<void>> task) {
    spawn([](Task<Result<void>> t) -> Task<void> {
        co_await std::move(t);
    }(std::move(task)));
  }

  /**
   * @brief Run a coroutine Task as fire-and-forget on a specific worker.
   *
   * @param reactor_idx Worker index in the range 0 ~ thread_count()-1.
   * @param task        Task<void> to run as fire-and-forget.
   */
  void spawn_on(size_t reactor_idx, Task<void> task);

private:
  /** @brief Running state flag. Used for synchronization between `run()` and `stop()`. */
  std::atomic<bool> running_{false};

  /** @brief Round-robin counter for `post()`. */
  std::atomic<size_t> next_post_idx_{0};

  /**
   * @brief List of worker Reactor instances.
   *
   * The Reactor at index i is owned by the worker thread at index i.
   * Lifetime is tied to the Dispatcher.
   */
  std::vector<std::unique_ptr<Reactor>> reactors_;
};

} // namespace qbuem

/** @} */
