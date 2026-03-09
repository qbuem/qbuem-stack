#pragma once

#include <coroutine>
#include <exception>
#include <iostream>
#include <utility>

namespace draco {

/**
 * @brief A Coroutine Task type with symmetric transfer support.
 */
template <typename T = void> struct Task {
  struct promise_type {
    T value;
    std::coroutine_handle<> continuation;

    promise_type() { std::cout << "[Debug] Task Promise created" << std::endl; }
    ~promise_type() {
      std::cout << "[Debug] Task Promise destroyed" << std::endl;
    }

    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() { return {}; }
    auto final_suspend() noexcept {
      struct final_awaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<promise_type> h) noexcept {
          std::cout << "[Debug] Task final_suspend, kont: "
                    << (h.promise().continuation ? "exists" : "null")
                    << std::endl;
          if (h.promise().continuation)
            return h.promise().continuation;
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
    if (handle) {
      std::cout << "[Debug] Task destructor destroying handle" << std::endl;
      handle.destroy();
    }
  }

  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  Task(Task &&other) noexcept : handle(other.handle) { other.handle = nullptr; }
  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      if (handle)
        handle.destroy();
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }

  bool resume() {
    if (!handle || handle.done())
      return false;
    std::cout << "[Debug] Resuming Task" << std::endl;
    handle.resume();
    return handle && !handle.done();
  }

  std::coroutine_handle<promise_type> detach() {
    std::cout << "[Debug] Detaching Task" << std::endl;
    auto h = handle;
    handle = nullptr;
    return h;
  }

  // Awaiter interface
  bool await_ready() const noexcept { return !handle || handle.done(); }
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> awaiting) noexcept {
    std::cout << "[Debug] Task await_suspend" << std::endl;
    handle.promise().continuation = awaiting;
    return handle;
  }
  T await_resume() { return std::move(handle.promise().value); }
};

template <> struct Task<void> {
  struct promise_type {
    std::coroutine_handle<> continuation;

    promise_type() {
      std::cout << "[Debug] Task<void> Promise created" << std::endl;
    }
    ~promise_type() {
      std::cout << "[Debug] Task<void> Promise destroyed" << std::endl;
    }

    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() { return {}; }
    auto final_suspend() noexcept {
      struct final_awaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<promise_type> h) noexcept {
          std::cout << "[Debug] Task<void> final_suspend, kont: "
                    << (h.promise().continuation ? "exists" : "null")
                    << std::endl;
          if (h.promise().continuation)
            return h.promise().continuation;
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
    if (handle) {
      std::cout << "[Debug] Task<void> destructor destroying handle"
                << std::endl;
      handle.destroy();
    }
  }

  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  Task(Task &&other) noexcept : handle(other.handle) { other.handle = nullptr; }
  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      if (handle)
        handle.destroy();
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }

  bool resume() {
    if (!handle || handle.done())
      return false;
    std::cout << "[Debug] Resuming Task<void>" << std::endl;
    handle.resume();
    return handle && !handle.done();
  }

  std::coroutine_handle<promise_type> detach() {
    std::cout << "[Debug] Detaching Task<void>" << std::endl;
    auto h = handle;
    handle = nullptr;
    return h;
  }

  // Awaiter interface
  bool await_ready() const noexcept { return !handle || handle.done(); }
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> awaiting) noexcept {
    std::cout << "[Debug] Task<void> await_suspend" << std::endl;
    handle.promise().continuation = awaiting;
    return handle;
  }
  void await_resume() noexcept {}
};

} // namespace draco
