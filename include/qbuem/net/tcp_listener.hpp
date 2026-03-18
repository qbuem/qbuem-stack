#pragma once

/**
 * @file qbuem/net/tcp_listener.hpp
 * @brief Asynchronous TCP listening socket type.
 * @ingroup qbuem_net
 *
 * `TcpListener` manages a SO_REUSEPORT listen socket via RAII and provides
 * a coroutine-based asynchronous accept interface.
 *
 * ### Design Principles
 * - Move-only: non-copyable, fd is automatically closed in the destructor
 * - SO_REUSEPORT enabled by default (multiple workers share the same port)
 * - TCP_FASTOPEN and TCP_DEFER_ACCEPT (Linux only) applied automatically when available
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/tcp_stream.hpp>

#include <cerrno>
#include <coroutine>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace qbuem {

/**
 * @brief Asynchronous TCP listening socket.
 *
 * After binding to a port with `bind()`, repeatedly call `accept()` to
 * accept client connections. Each accepted connection is returned as a TcpStream.
 *
 * ### Usage Example
 * @code
 * auto addr = SocketAddr::from_ipv4("0.0.0.0", 8080);
 * auto listener = TcpListener::bind(*addr);
 * if (!listener) { // error handling }
 *
 * while (true) {
 *     auto stream = co_await listener->accept();
 *     if (!stream) break;
 *     // handle stream
 * }
 * @endcode
 */
class TcpListener {
public:
  /** @brief Initialize with an invalid fd. */
  TcpListener() noexcept : fd_(-1) {}

  /**
   * @brief Construct a TcpListener from an already-listening fd.
   * @param fd Non-blocking listen socket file descriptor.
   */
  explicit TcpListener(int fd) noexcept : fd_(fd) {}

  /** @brief Destructor: automatically closes the listen socket. */
  ~TcpListener() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @brief Copy constructor deleted (Move-only). */
  TcpListener(const TcpListener &) = delete;

  /** @brief Copy assignment deleted (Move-only). */
  TcpListener &operator=(const TcpListener &) = delete;

  /** @brief Move constructor. */
  TcpListener(TcpListener &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief Move assignment operator. */
  TcpListener &operator=(TcpListener &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── Factory Methods ────────────────────────────────────────────────────────

  /**
   * @brief Bind to the specified address and create a listening socket.
   *
   * The following options are applied automatically:
   * - SO_REUSEPORT: allows multiple workers to share the same port
   * - TCP_FASTOPEN: enables TFO when available (queue size 128)
   * - TCP_DEFER_ACCEPT: on Linux, defers accept until data arrives
   *
   * @param addr Local address to bind to.
   * @returns TcpListener on success, or an error code on failure.
   */
  static Result<TcpListener> bind(SocketAddr addr) noexcept {
    int domain = (addr.family() == SocketAddr::Family::IPv6) ? AF_INET6 : AF_INET;
    int fd = ::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return unexpected(std::error_code(errno, std::system_category()));

    // SO_REUSEPORT: allow multiple workers to share the port
    {
      int opt = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    // SO_REUSEADDR: allow reuse of addresses in TIME_WAIT state
    {
      int opt = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

#ifdef TCP_FASTOPEN
    // TCP_FASTOPEN: send data on the first SYN when available
    {
      int qlen = 128;
      ::setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
    }
#endif

#ifdef TCP_DEFER_ACCEPT
    // TCP_DEFER_ACCEPT: on Linux, return from accept only when data has arrived
    {
      int timeout = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &timeout, sizeof(timeout));
    }
#endif

    sockaddr_storage ss{};
    socklen_t len{};
    auto r = addr.to_sockaddr(ss, len);
    if (!r) {
      ::close(fd);
      return unexpected(r.error());
    }

    if (::bind(fd, reinterpret_cast<const sockaddr *>(&ss), len) != 0) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      return unexpected(ec);
    }

    if (::listen(fd, SOMAXCONN) != 0) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      return unexpected(ec);
    }

    return TcpListener(fd);
  }

  // ─── Asynchronous accept ────────────────────────────────────────────────────

  /**
   * @brief Asynchronously accept a client connection.
   *
   * Waits for a Reactor event until the listen socket becomes readable, then
   * accepts a client socket via `accept4(2)`.
   * The returned TcpStream socket is configured with SOCK_NONBLOCK | SOCK_CLOEXEC.
   *
   * @returns Client TcpStream on success, or an error code on failure.
   */
  Task<Result<TcpStream>> accept() {
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
              sockaddr_storage addr{};
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
    co_return TcpStream(aw.client_fd_);
  }

  // ─── Accessors ──────────────────────────────────────────────────────────────

  /**
   * @brief Returns the underlying file descriptor.
   * @returns The listen socket fd. -1 if invalid.
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief The managed listen socket file descriptor. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
