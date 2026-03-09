#pragma once

#include <draco/core/reactor.hpp>
#include <draco/core/task.hpp>

#include <coroutine>
#include <unistd.h>

namespace draco {

/**
 * @brief Awaiter for asynchronous read.
 */
struct AsyncRead {
  int fd;
  void *buf;
  size_t count;
  ssize_t result = -1;

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) {
    auto *reactor = Reactor::current();
    if (!reactor) {
      handle.resume();
      return;
    }

    reactor->register_event(fd, EventType::Read, [handle, this](int f) {
      result = read(f, buf, count);
      Reactor::current()->unregister_event(f, EventType::Read);
      handle.resume();
    });
  }

  ssize_t await_resume() const noexcept { return result; }
};

/**
 * @brief Awaiter for asynchronous write.
 */
struct AsyncWrite {
  int fd;
  const void *buf;
  size_t count;
  ssize_t result = -1;

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) {
    auto *reactor = Reactor::current();
    if (!reactor) {
      handle.resume();
      return;
    }

    reactor->register_event(fd, EventType::Write, [handle, this](int f) {
      result = write(f, buf, count);
      Reactor::current()->unregister_event(f, EventType::Write);
      handle.resume();
    });
  }

  ssize_t await_resume() const noexcept { return result; }
};

/**
 * @brief Awaiter for asynchronous sleep.
 */
struct AsyncSleep {
  int timeout_ms;

  bool await_ready() const noexcept { return timeout_ms <= 0; }

  void await_suspend(std::coroutine_handle<> handle) {
    auto *reactor = Reactor::current();
    if (!reactor) {
      handle.resume();
      return;
    }

    reactor->register_timer(timeout_ms, [handle, reactor](int timer_id) {
      reactor->unregister_timer(timer_id);
      handle.resume();
    });
  }

  void await_resume() const noexcept {}
};

inline AsyncSleep sleep(int ms) { return AsyncSleep{ms}; }

} // namespace draco
