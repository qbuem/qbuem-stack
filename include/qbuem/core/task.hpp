#pragma once

/**
 * @file qbuem/core/task.hpp
 * @brief C++20 coroutine Task type — symmetric transfer support
 * @defgroup qbuem_task Task Coroutine
 * @ingroup qbuem_core
 *
 * Task<T> is a single-continuation coroutine type.
 * Supports suspend/resume via co_await, and fire-and-forget via detach().
 * Lifetime rules: the Task destructor destroys the frame; after detach() the
 * frame is self-managing.
 * @{
 */

#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace qbuem {

/**
 * @brief Global handler for unhandled coroutine exceptions.
 *
 * Default: nullptr (calls std::terminate()).
 * Set via `set_unhandled_exception_handler()` to invoke the handler instead
 * of terminating.
 *
 * ### Thread Safety
 * Set this once at application startup. Replacing the handler is not thread-safe.
 */
inline std::function<void(std::exception_ptr)> g_unhandled_exception_handler;

/**
 * @brief Set the global handler for unhandled coroutine exceptions.
 *
 * @param handler Function to receive the exception. Pass nullptr to revert
 *                to std::terminate() behavior.
 *
 * Example:
 * @code
 * qbuem::set_unhandled_exception_handler([](std::exception_ptr ep) {
 *   try { std::rethrow_exception(ep); }
 *   catch (const std::exception& e) {
 *     log_error("Unhandled coroutine exception: {}", e.what());
 *   }
 * });
 * @endcode
 */
inline void set_unhandled_exception_handler(
    std::function<void(std::exception_ptr)> handler) {
  g_unhandled_exception_handler = std::move(handler);
}

/**
 * @brief A Coroutine Task type with symmetric transfer support.
 *
 * Lifecycle rules:
 *  - When owned by a Task object, the Task destructor destroys the frame.
 *  - When detach() is called the Task releases ownership. The coroutine frame
 *    is then responsible for destroying itself when it completes (no
 *    continuation). This avoids leaking fire-and-forget coroutines.
 *
 * @warning **Thread safety**: Task is NOT thread-safe.
 *  - `detach()` writes `promise.detached` from the owning thread.
 *  - `final_suspend()` reads `promise.detached` / `promise.continuation` from
 *    the coroutine frame's execution context.
 *  - `await_suspend()` writes `promise.continuation` from the awaiting
 *    coroutine's thread.
 *
 *  All of the above MUST happen on the same thread (the Reactor event loop).
 *  If coroutines are dispatched across multiple threads, callers are
 *  responsible for external synchronization.  A TSan data race will be
 *  reported if `detach()` races with `final_suspend()` on different threads.
 */
template <typename T = void> struct Task {
  using value_type = T;

  struct promise_type {
    std::optional<T> value; // optional allows non-default-constructible T
    std::coroutine_handle<> continuation;
    bool detached = false;

    // Prevent GCC HALO (Heap Allocation Elision Optimization) from placing
    // coroutine frames on the C stack, which causes stack-use-after-return
    // under ASan when the coroutine suspends and the C stack frame is freed.
    static void* operator new(std::size_t n) { return ::operator new(n); }
    static void  operator delete(void* p) noexcept { ::operator delete(p); }

    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() { return {}; }

    auto final_suspend() noexcept {
      struct final_awaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<promise_type> h) noexcept {
          if (h.promise().continuation)
            return h.promise().continuation;
          if (h.promise().detached) {
            h.destroy();
            return std::noop_coroutine();
          }
          return std::noop_coroutine();
        }
        void await_resume() noexcept {}
      };
      return final_awaiter{};
    }

    void unhandled_exception() {
      if (g_unhandled_exception_handler)
        g_unhandled_exception_handler(std::current_exception());
      else
        std::terminate();
    }
    void return_value(T v) { value.emplace(std::move(v)); }
  };

  std::coroutine_handle<promise_type> handle;

  explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
  ~Task() {
    if (handle && !handle.promise().detached)
      handle.destroy();
  }

  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  Task(Task &&other) noexcept : handle(other.handle) { other.handle = nullptr; }
  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      if (handle && !handle.promise().detached)
        handle.destroy();
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }

  bool resume() {
    if (!handle || handle.done())
      return false;
    handle.resume();
    return handle && !handle.done();
  }

  /**
   * @brief Release ownership of the coroutine frame.
   *
   * After detach() the frame is self-managed: it destroys itself when the
   * coroutine completes with no continuation.
   */
  void detach() {
    if (handle) {
      handle.promise().detached = true;
      handle = nullptr;
    }
  }

  // Awaiter interface
  bool await_ready() const noexcept { return !handle || handle.done(); }
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> awaiting) noexcept {
    handle.promise().continuation = awaiting;
    return handle;
  }
  T await_resume() { return std::move(*handle.promise().value); }
};

template <> struct Task<void> {
  struct promise_type {
    std::coroutine_handle<> continuation;
    bool detached = false;

    // Prevent GCC HALO (Heap Allocation Elision Optimization) from placing
    // coroutine frames on the C stack, which causes stack-use-after-return
    // under ASan when the coroutine suspends and the C stack frame is freed.
    static void* operator new(std::size_t n) { return ::operator new(n); }
    static void  operator delete(void* p) noexcept { ::operator delete(p); }

    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() { return {}; }

    auto final_suspend() noexcept {
      struct final_awaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<promise_type> h) noexcept {
          if (h.promise().continuation)
            return h.promise().continuation;
          if (h.promise().detached) {
            h.destroy();
            return std::noop_coroutine();
          }
          return std::noop_coroutine();
        }
        void await_resume() noexcept {}
      };
      return final_awaiter{};
    }

    void unhandled_exception() {
      if (g_unhandled_exception_handler)
        g_unhandled_exception_handler(std::current_exception());
      else
        std::terminate();
    }
    void return_void() {}
  };

  std::coroutine_handle<promise_type> handle;

  explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
  ~Task() {
    if (handle && !handle.promise().detached)
      handle.destroy();
  }

  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  Task(Task &&other) noexcept : handle(other.handle) { other.handle = nullptr; }
  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      if (handle && !handle.promise().detached)
        handle.destroy();
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }

  bool resume() {
    if (!handle || handle.done())
      return false;
    handle.resume();
    return handle && !handle.done();
  }

  /**
   * @brief Release ownership of the coroutine frame.
   *
   * After detach() the frame is self-managed: it destroys itself when the
   * coroutine completes with no continuation.
   */
  void detach() {
    if (handle) {
      handle.promise().detached = true;
      handle = nullptr;
    }
  }

  // Awaiter interface
  bool await_ready() const noexcept { return !handle || handle.done(); }
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> awaiting) noexcept {
    handle.promise().continuation = awaiting;
    return handle;
  }
  void await_resume() noexcept {}
};

} // namespace qbuem

/** @} */
