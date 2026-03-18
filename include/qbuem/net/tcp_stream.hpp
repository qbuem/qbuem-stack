#pragma once

/**
 * @file qbuem/net/tcp_stream.hpp
 * @brief Asynchronous TCP stream connection type.
 * @ingroup qbuem_net
 *
 * `TcpStream` manages a non-blocking TCP socket file descriptor via RAII and
 * provides a coroutine-based asynchronous read/write interface.
 *
 * ### Design Principles
 * - Move-only: non-copyable, fd is automatically closed in the destructor
 * - All I/O is performed asynchronously via `Reactor::current()`
 * - scatter-gather I/O (`readv`/`writev`) supported
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>

#include <cerrno>
#include <coroutine>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <span>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>

namespace qbuem {

/**
 * @brief Asynchronous TCP connection stream.
 *
 * Wraps a non-blocking TCP socket to provide a coroutine-based read/write
 * interface. Move-only type; the socket is closed automatically on destruction.
 *
 * ### Usage Example
 * @code
 * auto addr = SocketAddr::from_ipv4("127.0.0.1", 8080);
 * auto stream = co_await TcpStream::connect(*addr);
 * if (!stream) { // error handling }
 *
 * std::array<std::byte, 1024> buf{};
 * auto n = co_await stream->read(buf);
 * @endcode
 */
class TcpStream {
public:
  /** @brief Initialize with an invalid fd. */
  TcpStream() noexcept : fd_(-1) {}

  /**
   * @brief Construct a TcpStream from an already-connected fd.
   * @param fd Non-blocking TCP socket file descriptor.
   */
  explicit TcpStream(int fd) noexcept : fd_(fd) {}

  /** @brief Destructor: automatically closes the open socket. */
  ~TcpStream() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @brief Copy constructor deleted (Move-only). */
  TcpStream(const TcpStream &) = delete;

  /** @brief Copy assignment deleted (Move-only). */
  TcpStream &operator=(const TcpStream &) = delete;

  /** @brief Move constructor. */
  TcpStream(TcpStream &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief Move assignment operator. */
  TcpStream &operator=(TcpStream &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── Asynchronous Connect ────────────────────────────────────────────────

  /**
   * @brief Perform an asynchronous TCP connection to the specified address.
   *
   * Calls non-blocking `connect(2)` and, on EINPROGRESS, registers a write
   * event with the Reactor and waits for connection completion.
   *
   * @param addr Remote socket address to connect to.
   * @returns TcpStream on success, or an error code on failure.
   */
  static Task<Result<TcpStream>> connect(SocketAddr addr) {
#ifdef __linux__
    int fd = ::socket(
        addr.family() == SocketAddr::Family::IPv6 ? AF_INET6 : AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    int fd = ::socket(
        addr.family() == SocketAddr::Family::IPv6 ? AF_INET6 : AF_INET,
        SOCK_STREAM, 0);
    if (fd >= 0) {
      ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (fd < 0) {
      co_return Result<TcpStream>::err(
          std::error_code(errno, std::system_category()));
    }

    sockaddr_storage ss{};
    socklen_t len{};
    auto r = addr.to_sockaddr(ss, len);
    if (!r) {
      ::close(fd);
      co_return Result<TcpStream>::err(r.error());
    }

    int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&ss), len);
    if (rc == 0) {
      co_return TcpStream(fd);
    }
    if (errno != EINPROGRESS) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      co_return Result<TcpStream>::err(ec);
    }

    // EINPROGRESS: wait for a Reactor write event
    struct ConnectAwaiter {
      int fd_;
      int err_ = 0;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) {
          handle.resume();
          return;
        }
        reactor->register_event(fd_, EventType::Write, [handle, this](int f) {
          int so_err = 0;
          socklen_t len = sizeof(so_err);
          ::getsockopt(f, SOL_SOCKET, SO_ERROR, &so_err, &len);
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
    co_return TcpStream(fd);
  }

  // ─── Asynchronous I/O ────────────────────────────────────────────────────

  /**
   * @brief Asynchronously read data into a buffer.
   *
   * Waits for a Reactor event until the socket becomes readable.
   *
   * @param buf Buffer to store received data.
   * @returns Number of bytes read. 0 on EOF. error_code on failure.
   */
  Task<Result<size_t>> read(std::span<std::byte> buf) {
    ssize_t n = co_await AsyncRead{fd_, buf.data(), buf.size()};
    if (n < 0) {
      co_return Result<size_t>::err(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief Asynchronously write data from a buffer.
   *
   * Waits for a Reactor event until the socket becomes writable.
   *
   * @param buf Buffer containing data to send.
   * @returns Number of bytes sent. error_code on failure.
   */
  Task<Result<size_t>> write(std::span<const std::byte> buf) {
    ssize_t n = co_await AsyncWrite{fd_, buf.data(), buf.size()};
    if (n < 0) {
      co_return Result<size_t>::err(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief Asynchronous scatter read from multiple buffers (readv).
   *
   * Waits for a Reactor read event, then calls `readv(2)`.
   *
   * @param bufs Array of iovec entries, each pointing to an independent buffer.
   * @returns Total bytes read. 0 on EOF. error_code on failure.
   */
  Task<Result<size_t>> readv(std::span<iovec> bufs) {
    struct ReadvAwaiter {
      int fd_;
      iovec *iov_;
      int iovcnt_;
      ssize_t result_ = -1;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) { handle.resume(); return; }
        reactor->register_event(fd_, EventType::Read, [handle, this](int f) {
          result_ = ::readv(f, iov_, iovcnt_);
          Reactor::current()->unregister_event(f, EventType::Read);
          handle.resume();
        });
      }

      ssize_t await_resume() const noexcept { return result_; }
    };

    ssize_t n = co_await ReadvAwaiter{fd_, bufs.data(),
                                      static_cast<int>(bufs.size())};
    if (n < 0) {
      co_return Result<size_t>::err(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief Asynchronous gather write to multiple buffers (writev).
   *
   * Waits for a Reactor write event, then calls `writev(2)`.
   *
   * @param bufs Array of const iovec entries, each pointing to a buffer to send.
   * @returns Total bytes sent. error_code on failure.
   */
  Task<Result<size_t>> writev(std::span<const iovec> bufs) {
    struct WritevAwaiter {
      int fd_;
      const iovec *iov_;
      int iovcnt_;
      ssize_t result_ = -1;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) { handle.resume(); return; }
        reactor->register_event(fd_, EventType::Write, [handle, this](int f) {
          result_ = ::writev(f, iov_, iovcnt_);
          Reactor::current()->unregister_event(f, EventType::Write);
          handle.resume();
        });
      }

      ssize_t await_resume() const noexcept { return result_; }
    };

    ssize_t n = co_await WritevAwaiter{fd_, bufs.data(),
                                       static_cast<int>(bufs.size())};
    if (n < 0) {
      co_return Result<size_t>::err(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  // ─── Socket Options ──────────────────────────────────────────────────────

  /**
   * @brief Set the TCP_NODELAY option (disables Nagle's algorithm).
   *
   * Useful for low-latency applications that need small packets sent immediately
   * without buffering delay.
   *
   * @param v true to enable TCP_NODELAY, false to disable.
   */
  void set_nodelay(bool v) noexcept {
    int flag = v ? 1 : 0;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
  }

  /**
   * @brief Set the SO_KEEPALIVE option (enables TCP keepalive).
   *
   * Periodically checks whether an idle connection is still alive.
   *
   * @param v true to enable keepalive, false to disable.
   */
  void set_keepalive(bool v) noexcept {
    int flag = v ? 1 : 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  }

  // ─── Accessors ───────────────────────────────────────────────────────────

  /**
   * @brief Returns the underlying file descriptor.
   * @returns Socket fd. -1 if invalid.
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief The managed TCP socket file descriptor. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
