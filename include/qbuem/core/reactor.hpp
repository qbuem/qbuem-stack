#pragma once

/**
 * @file qbuem/core/reactor.hpp
 * @brief Platform-independent abstract interface for the event loop Reactor.
 * @ingroup qbuem_io
 *
 * This header provides the core I/O event loop abstraction for qbuem-stack.
 *
 * ### Shared-Nothing Design
 * Each `Reactor` instance runs on **exactly one thread**.
 * Minimizing data sharing between threads prevents lock contention and cache
 * invalidation. This pattern is a proven approach used by high-performance
 * servers such as Nginx, Redis, and Node.js.
 *
 * ### Platform Abstraction
 * Concrete implementations differ by platform:
 * - Linux: `epoll`-based implementation
 * - macOS/BSD: `kqueue`-based implementation
 * - Tests: mock implementation
 *
 * ### Thread-Local Access
 * The current thread's Reactor is accessed via `Reactor::current()`.
 * This allows coroutine awaiters to reach the Reactor without explicit
 * reference passing.
 */

/**
 * @defgroup qbuem_io I/O & Event Loop
 * @brief I/O event handling components: Reactor, Dispatcher, Connection, etc.
 *
 * This group forms the core infrastructure for asynchronous I/O:
 * - `Reactor`: single-threaded event loop abstraction
 * - `Dispatcher`: orchestrator managing multiple Reactors
 * - `Connection`: client connection lifetime management
 * @{
 */

#include <qbuem/common.hpp>
#include <functional>

namespace qbuem {

/**
 * @brief Enumeration of file descriptor or timer event types.
 *
 * Specifies which kind of event to watch when registering with the Reactor.
 *
 * - `Read`:  fd has data available to read (EPOLLIN / EVFILT_READ)
 * - `Write`: fd is ready for writing (EPOLLOUT / EVFILT_WRITE)
 * - `Error`: fd has an error condition (EPOLLERR / EVFILT_EXCEPT)
 */
enum class EventType { Read, Write, Error };

/**
 * @brief Platform-independent abstract event loop interface.
 *
 * `Reactor` handles asynchronous events on file descriptors and timers.
 * Each instance runs exclusively on a single thread (Shared-Nothing).
 *
 * Concrete classes implement this interface per platform.
 * Coroutine-based awaiters (`AsyncRead`, `AsyncWrite`, etc.) interact with
 * the Reactor through this interface.
 *
 * @note This is an abstract class and cannot be instantiated directly.
 *       Use a platform-specific concrete implementation or a mock for testing.
 *
 * ### Typical Usage Pattern
 * @code
 * // Reactor is usually created and managed by Dispatcher.
 * // Inside a coroutine, access the current thread's Reactor via current():
 * auto *reactor = qbuem::Reactor::current();
 * reactor->register_event(fd, EventType::Read, [](int f) {
 *     // called when fd becomes readable
 * });
 * @endcode
 */
class Reactor {
public:
  /** @brief Virtual destructor. Allows derived classes to release resources safely. */
  virtual ~Reactor() = default;

  /**
   * @brief Register a file descriptor with the Reactor and set an event callback.
   *
   * `callback` is called when the specified event occurs.
   * Registering the same fd/type combination again overwrites the existing callback.
   *
   * @param fd       File descriptor to watch.
   * @param type     Event type to watch (`EventType::Read`, `Write`, or `Error`).
   * @param callback Callback invoked when the event fires. Receives the fd as argument.
   * @returns `Result<void>::ok()` on success, or an error code on failure.
   *
   * @note This function must only be called from the thread running this Reactor.
   * @warning The fd must have the `O_NONBLOCK` flag set. Registering a blocking fd
   *          may cause the entire event loop to block.
   */
  virtual Result<void> register_event(int fd, EventType type,
                                      std::function<void(int)> callback) = 0;

  /**
   * @brief Register a one-shot timer to call a callback after the specified delay.
   *
   * Timers are one-shot. For repeating timers, re-register inside the callback.
   *
   * @param timeout_ms Time to wait before the timer fires (milliseconds).
   * @param callback   Callback invoked on expiry. Receives the timer ID as argument.
   * @returns `Result<int>` containing the timer ID on success, or an error code.
   *          The returned ID is used with `unregister_timer()` to cancel the timer.
   *
   * @note If timeout_ms is 0, the callback is invoked immediately in the event loop.
   */
  virtual Result<int> register_timer(int timeout_ms,
                                     std::function<void(int)> callback) = 0;

  /**
   * @brief Stop watching a file descriptor for the specified event type.
   *
   * Coroutine awaiters must call this after processing an event to prevent
   * duplicate handling in the next event cycle.
   *
   * @param fd   File descriptor to stop watching.
   * @param type Event type to unregister.
   * @returns `Result<void>::ok()` on success, or an error code on failure.
   */
  virtual Result<void> unregister_event(int fd, EventType type) = 0;

  /**
   * @brief Cancel a registered timer.
   *
   * Used to cancel a timer that has not yet expired.
   * Passing an already-expired timer ID is silently ignored.
   *
   * @param timer_id Timer ID returned by `register_timer()`.
   * @returns `Result<void>::ok()` on success, or an error code on failure.
   */
  virtual Result<void> unregister_timer(int timer_id) = 0;

  /**
   * @brief Check whether the Reactor's event loop is currently running.
   * @returns true if running, false otherwise.
   */
  virtual bool is_running() const = 0;

  /**
   * @brief Run a single iteration of the event loop.
   *
   * Processes registered events and returns. If no events occur within
   * `timeout_ms`, returns after the timeout.
   *
   * This function is called repeatedly by the Dispatcher's worker thread loop.
   *
   * @param timeout_ms Maximum time to wait for events (milliseconds).
   *                   -1 waits indefinitely until an event occurs.
   * @returns `Result<int>` containing the number of events processed, or an error code.
   */
  virtual Result<int> poll(int timeout_ms) = 0;

  /**
   * @brief Stop the event loop.
   *
   * After this call, `is_running()` returns false and any in-progress
   * `poll()` call returns early.
   *
   * @note This function must be implemented in a thread-safe manner.
   *       It must be possible to call it from another thread to stop
   *       the current Reactor.
   */
  virtual void stop() = 0;

  /**
   * @brief Enqueue an arbitrary callback to run on the Reactor thread.
   *
   * Use this when you want to safely execute work on the Reactor's thread
   * from another thread (or the same thread). When waking a coroutine,
   * always delegate via this function rather than calling `handle.resume()`
   * directly.
   *
   * The implementation uses a platform-specific wakeup mechanism:
   * - epoll:    eventfd write → epoll_wait early return
   * - kqueue:   EVFILT_USER NOTE_TRIGGER
   * - io_uring: eventfd POLL_ADD (OpKind::Wake)
   *
   * @param fn Callback to execute. Called safely inside the `poll()` loop.
   * @note Thread-safe. Can be called from any thread.
   */
  virtual void post(std::function<void()> fn) = 0;

  /**
   * @brief Set a write deadline for the given fd.
   *
   * If no write event occurs on fd within `timeout_ms`, calls `timeout_cb(fd)`.
   * When writing completes, cancel the timer via `unregister_timer(timer_id)`.
   *
   * The default implementation combines `register_timer()` with
   * `unregister_event(fd, Write)` inside the callback.
   *
   * @param fd          File descriptor to watch.
   * @param timeout_ms  Deadline in milliseconds.
   * @param timeout_cb  Callback invoked on timeout. Argument is the fd.
   * @returns Timer ID on success (use for cancellation). Error code on failure.
   */
  Result<int> register_write_timeout(int fd, int timeout_ms,
                                     std::function<void(int)> timeout_cb) {
    return register_timer(timeout_ms, [this, fd,
                                       cb = std::move(timeout_cb)](int) {
      unregister_event(fd, EventType::Write);
      cb(fd);
    });
  }

  /**
   * @brief Return the Reactor associated with the current thread.
   *
   * Returns the unique Reactor instance for each thread via thread-local storage.
   *
   * Coroutine awaiters use this function to access the current thread's
   * Reactor without explicit reference passing.
   *
   * @returns Pointer to the current thread's Reactor. nullptr if `set_current()`
   *          has not been called.
   * @note Dispatcher calls `set_current()` when starting each worker thread.
   */
  static Reactor *current();

  /**
   * @brief Set the Reactor for the current thread.
   *
   * Called by Dispatcher at the start of each worker thread.
   * After this call, `current()` on that thread returns `r`.
   *
   * @param r Reactor pointer to associate with the current thread. nullptr is allowed.
   */
  static void set_current(Reactor *r);
};

} // namespace qbuem

/** @} */ // end of qbuem_io
