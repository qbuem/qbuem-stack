#pragma once

#include <coroutine>
#include <exception>
#include <utility>

namespace draco {

/**
 * @brief A Coroutine Task type with symmetric transfer support.
 *
 * Lifecycle rules:
 *  - When owned by a Task object, the Task destructor destroys the frame.
 *  - When detach() is called the Task releases ownership. The coroutine frame
 *    is then responsible for destroying itself when it completes (no
 *    continuation). This avoids leaking fire-and-forget coroutines.
 */
template <typename T = void> struct Task {
  struct promise_type {
    T value;
    std::coroutine_handle<> continuation;
    bool detached = false;

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

    void unhandled_exception() { std::terminate(); }
    void return_value(T v) { value = std::move(v); }
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
  T await_resume() { return std::move(handle.promise().value); }
};

template <> struct Task<void> {
  struct promise_type {
    std::coroutine_handle<> continuation;
    bool detached = false;

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

    void unhandled_exception() { std::terminate(); }
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

} // namespace draco
