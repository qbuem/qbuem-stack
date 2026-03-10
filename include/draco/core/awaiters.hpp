#pragma once

#include <draco/core/reactor.hpp>
#include <draco/core/task.hpp>

#include <coroutine>
#include <netinet/in.h>
#include <sys/socket.h>
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

/**
 * @brief Awaiter for asynchronous accept (Phase 5).
 *
 * Suspends the coroutine until the listening socket becomes readable, then
 * calls accept(2) and resumes.  Returns the accepted client fd, or -1 on
 * error.  The caller must set O_NONBLOCK on the client fd if needed.
 *
 * Usage (inside a Task<void> coroutine):
 *   while (true) {
 *     int client_fd = co_await AsyncAccept{listen_fd};
 *     if (client_fd < 0) break;
 *     // handle client_fd …
 *   }
 */
struct AsyncAccept {
  int listen_fd;
  int client_fd = -1;

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) {
    auto *reactor = Reactor::current();
    if (!reactor) {
      handle.resume();
      return;
    }

    reactor->register_event(
        listen_fd, EventType::Read, [handle, this](int lfd) {
          struct sockaddr_in addr{};
          socklen_t len = sizeof(addr);
          client_fd =
              accept(lfd, reinterpret_cast<struct sockaddr *>(&addr), &len);
          // Unregister so the caller can re-register for the next accept.
          Reactor::current()->unregister_event(lfd, EventType::Read);
          handle.resume();
        });
  }

  int await_resume() const noexcept { return client_fd; }
};

inline AsyncSleep sleep(int ms) { return AsyncSleep{ms}; }

} // namespace draco
