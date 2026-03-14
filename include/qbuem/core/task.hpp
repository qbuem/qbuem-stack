#pragma once

/**
 * @file qbuem/core/task.hpp
 * @brief C++20 코루틴 Task 타입 — 대칭 전송(symmetric transfer) 지원
 * @defgroup qbuem_task Task Coroutine
 * @ingroup qbuem_core
 *
 * Task<T>는 단일-연속(single-continuation) 코루틴 타입.
 * co_await로 중단/재개, detach()로 fire-and-forget 지원.
 * 수명 규칙: Task 소멸자가 프레임 파괴, detach() 후에는 자기 파괴.
 * @{
 */

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace qbuem {

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
    std::optional<T> value; // optional allows non-default-constructible T
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

} // namespace qbuem

/** @} */
