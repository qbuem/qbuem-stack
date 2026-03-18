#pragma once

/**
 * @file qbuem/net/unix_socket.hpp
 * @brief Asynchronous AF_UNIX domain socket type.
 * @ingroup qbuem_net
 *
 * `UnixSocket` manages an AF_UNIX stream socket via RAII and provides
 * coroutine-based asynchronous bind/connect/accept/read/write.
 *
 * ### Design Principles
 * - Move-only: non-copyable, fd is automatically closed in the destructor
 * - `bind()`: AF_UNIX SOCK_STREAM server socket (bind + listen)
 * - `connect()`: asynchronous client connection
 * - `accept()`: asynchronous client accept, returned as UnixSocket
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <cerrno>
#include <coroutine>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace qbuem {

/**
 * @brief Asynchronous AF_UNIX domain socket.
 *
 * Wraps an AF_UNIX SOCK_STREAM socket to provide a coroutine-based
 * asynchronous interface for IPC communication.
 * Move-only type; the socket is closed automatically on destruction.
 *
 * ### Usage Example (Server)
 * @code
 * auto server = UnixSocket::bind("/tmp/my.sock");
 * if (!server) { // error handling }
 *
 * auto client = co_await server->accept();
 * @endcode
 *
 * ### Usage Example (Client)
 * @code
 * auto conn = co_await UnixSocket::connect("/tmp/my.sock");
 * if (!conn) { // error handling }
 * @endcode
 */
class UnixSocket {
public:
  /** @brief Initialize with an invalid fd. */
  UnixSocket() noexcept : fd_(-1) {}

  /**
   * @brief Construct a UnixSocket from an existing fd.
   * @param fd Non-blocking AF_UNIX socket file descriptor.
   */
  explicit UnixSocket(int fd) noexcept : fd_(fd) {}

  /** @brief Destructor: automatically closes the open socket. */
  ~UnixSocket() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @brief Copy constructor deleted (Move-only). */
  UnixSocket(const UnixSocket &) = delete;

  /** @brief Copy assignment deleted (Move-only). */
  UnixSocket &operator=(const UnixSocket &) = delete;

  /** @brief Move constructor. */
  UnixSocket(UnixSocket &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief Move assignment operator. */
  UnixSocket &operator=(UnixSocket &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── Factory Methods ────────────────────────────────────────────────────────

  /**
   * @brief Bind to the specified Unix domain path and start listening.
   *
   * If a socket file already exists, it is removed with `unlink(2)` before
   * creating a new one.
   *
   * @param path Unix domain socket file path. Must be 107 bytes or fewer.
   * @returns UnixSocket (listen socket) on success, or an error code on failure.
   */
  static Result<UnixSocket> bind(const char *path) noexcept {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return unexpected(std::error_code(errno, std::system_category()));

    // Remove any existing socket file (ignore errors)
    ::unlink(path);

    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    ::strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    socklen_t len = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + ::strlen(path) + 1);

    if (::bind(fd, reinterpret_cast<const sockaddr *>(&sa), len) != 0) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      return unexpected(ec);
    }

    if (::listen(fd, SOMAXCONN) != 0) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      return unexpected(ec);
    }

    return UnixSocket(fd);
  }

  /**
   * @brief Asynchronously connect to the specified Unix domain path.
   *
   * Calls non-blocking `connect(2)` and waits for a Reactor write event
   * on EINPROGRESS.
   *
   * @param path Server socket file path to connect to.
   * @returns Connected UnixSocket on success, or an error code on failure.
   */
  static Task<Result<UnixSocket>> connect(const char *path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }

    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    ::strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    socklen_t len = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + ::strlen(path) + 1);

    int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&sa), len);
    if (rc == 0) {
      co_return UnixSocket(fd);
    }
    if (errno != EINPROGRESS) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      co_return unexpected(ec);
    }

    // EINPROGRESS: wait for a Reactor write event
    struct ConnectAwaiter {
      int fd_;
      int err_ = 0;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) { handle.resume(); return; }
        reactor->register_event(fd_, EventType::Write, [handle, this](int f) {
          int so_err = 0;
          socklen_t slen = sizeof(so_err);
          ::getsockopt(f, SOL_SOCKET, SO_ERROR, &so_err, &slen);
          err_ = so_err;
          Reactor::current()->unregister_event(f, EventType::Write);
          handle.resume();
        });
      }

      int await_resume() const noexcept { return err_; }
    };

    int so_err = co_await ConnectAwaiter{fd};
    if (so_err != 0) {
      ::close(fd);
      co_return unexpected(std::error_code(so_err, std::system_category()));
    }
    co_return UnixSocket(fd);
  }

  // ─── Asynchronous accept ────────────────────────────────────────────────────

  /**
   * @brief Asynchronously accept a client connection.
   *
   * Waits for a Reactor read event until the listen socket becomes readable,
   * then calls `accept(2)`.
   *
   * @returns Client UnixSocket on success, or an error code on failure.
   */
  Task<Result<UnixSocket>> accept() {
    struct AcceptAwaiter {
      int listen_fd_;
      int client_fd_ = -1;
      int err_        = 0;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) { handle.resume(); return; }
        reactor->register_event(
            listen_fd_, EventType::Read, [handle, this](int lfd) {
              sockaddr_un addr{};
              socklen_t len = sizeof(addr);
#ifdef __linux__
              client_fd_ = ::accept4(lfd,
                                     reinterpret_cast<sockaddr *>(&addr),
                                     &len,
                                     SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
              client_fd_ = ::accept(lfd,
                                    reinterpret_cast<sockaddr *>(&addr),
                                    &len);
              if (client_fd_ >= 0) {
                ::fcntl(client_fd_, F_SETFL,
                        ::fcntl(client_fd_, F_GETFL, 0) | O_NONBLOCK);
                ::fcntl(client_fd_, F_SETFD, FD_CLOEXEC);
              }
#endif
              err_ = (client_fd_ < 0) ? errno : 0;
              Reactor::current()->unregister_event(lfd, EventType::Read);
              handle.resume();
            });
      }

      void await_resume() const noexcept {}
    };

    AcceptAwaiter aw{fd_};
    co_await aw;

    if (aw.client_fd_ < 0) {
      co_return unexpected(std::error_code(aw.err_, std::system_category()));
    }
    co_return UnixSocket(aw.client_fd_);
  }

  // ─── Asynchronous I/O ───────────────────────────────────────────────────────

  /**
   * @brief Asynchronously read data into a buffer.
   *
   * @param buf Buffer to store received data.
   * @returns Number of bytes read. 0 on EOF. error_code on failure.
   */
  Task<Result<size_t>> read(std::span<std::byte> buf) {
    ssize_t n = co_await AsyncRead{fd_, buf.data(), buf.size()};
    if (n < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief Asynchronously write data from a buffer.
   *
   * @param buf Buffer containing data to send.
   * @returns Number of bytes sent. error_code on failure.
   */
  Task<Result<size_t>> write(std::span<const std::byte> buf) {
    ssize_t n = co_await AsyncWrite{fd_, buf.data(), buf.size()};
    if (n < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  // ─── Accessors ──────────────────────────────────────────────────────────────

  /**
   * @brief Returns the underlying file descriptor.
   * @returns Socket fd. -1 if invalid.
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief The managed AF_UNIX socket file descriptor. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
