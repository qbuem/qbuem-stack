#pragma once

/**
 * @file qbuem/net/udp_socket.hpp
 * @brief Asynchronous UDP socket type.
 * @ingroup qbuem_net
 *
 * `UdpSocket` manages a non-blocking UDP socket via RAII and provides
 * a coroutine-based asynchronous send/receive interface.
 *
 * ### Design Principles
 * - Move-only: non-copyable, fd is automatically closed in the destructor
 * - `send_to`: send to a specified destination address
 * - `recv_from`: receive data and return the sender's address
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>

#include <cerrno>
#include <coroutine>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace qbuem {

/**
 * @brief Asynchronous UDP socket.
 *
 * Wraps a non-blocking UDP socket to provide a coroutine-based send/receive
 * interface. Move-only type; the socket is closed automatically on destruction.
 *
 * ### Usage Example
 * @code
 * auto addr = SocketAddr::from_ipv4("0.0.0.0", 9000);
 * auto sock = UdpSocket::bind(*addr);
 * if (!sock) { // error handling }
 *
 * std::array<std::byte, 1500> buf{};
 * auto [n, from] = co_await sock->recv_from(buf);
 * @endcode
 */
class UdpSocket {
public:
  /** @brief Initialize with an invalid fd. */
  UdpSocket() noexcept : fd_(-1) {}

  /**
   * @brief Construct a UdpSocket from an already-bound fd.
   * @param fd Non-blocking UDP socket file descriptor.
   */
  explicit UdpSocket(int fd) noexcept : fd_(fd) {}

  /** @brief Destructor: automatically closes the open socket. */
  ~UdpSocket() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @brief Copy constructor deleted (Move-only). */
  UdpSocket(const UdpSocket &) = delete;

  /** @brief Copy assignment deleted (Move-only). */
  UdpSocket &operator=(const UdpSocket &) = delete;

  /** @brief Move constructor. */
  UdpSocket(UdpSocket &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief Move assignment operator. */
  UdpSocket &operator=(UdpSocket &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── Factory Methods ─────────────────────────────────────────────────────

  /**
   * @brief Create a UDP socket bound to the specified address.
   *
   * @param addr Local address to bind to. Both IPv4 and IPv6 are supported.
   * @returns UdpSocket on success, or an error code on failure.
   */
  static Result<UdpSocket> bind(SocketAddr addr) noexcept {
    int domain = (addr.family() == SocketAddr::Family::IPv6) ? AF_INET6 : AF_INET;
    int fd = ::socket(domain, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return std::unexpected(std::error_code(errno, std::system_category()));

    // SO_REUSEPORT: allow multiple workers to share the same port
    {
      int opt = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    sockaddr_storage ss{};
    socklen_t len{};
    auto r = addr.to_sockaddr(ss, len);
    if (!r) {
      ::close(fd);
      return std::unexpected(r.error());
    }

    if (::bind(fd, reinterpret_cast<const sockaddr *>(&ss), len) != 0) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      return std::unexpected(ec);
    }

    return UdpSocket(fd);
  }

  // ─── Asynchronous I/O ────────────────────────────────────────────────────

  /**
   * @brief Asynchronously send a datagram to the specified destination address.
   *
   * Waits for a Reactor event until the socket becomes writable.
   *
   * @param buf  Buffer containing data to send.
   * @param dest Destination socket address.
   * @returns Number of bytes sent. error_code on failure.
   */
  Task<Result<size_t>> send_to(std::span<const std::byte> buf,
                               SocketAddr dest) {
    sockaddr_storage ss{};
    socklen_t len{};
    auto r = dest.to_sockaddr(ss, len);
    if (!r) {
      co_return std::unexpected(r.error());
    }

    struct SendToAwaiter {
      int fd_;
      const std::byte *data_;
      size_t size_;
      const sockaddr_storage *ss_;
      socklen_t sslen_;
      ssize_t result_ = -1;
      int err_ = 0;

      [[nodiscard]] bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (reactor == nullptr) { handle.resume(); return; }
        reactor->register_event(fd_, EventType::Write, [handle, this](int f) {
          result_ = ::sendto(f, data_, size_, 0,
                             reinterpret_cast<const sockaddr *>(ss_), sslen_);
          err_ = (result_ < 0) ? errno : 0;
          Reactor::current()->unregister_event(f, EventType::Write);
          handle.resume();
        });
      }

      void await_resume() const noexcept {}
    };

    SendToAwaiter aw{fd_, buf.data(), buf.size(), &ss, len};
    co_await aw;

    if (aw.result_ < 0) {
      co_return std::unexpected(std::error_code(aw.err_, std::system_category()));
    }
    co_return static_cast<size_t>(aw.result_);
  }

  /**
   * @brief Asynchronously receive a datagram and return the sender's address.
   *
   * Waits for a Reactor event until the socket becomes readable.
   *
   * @param buf Buffer to store received data.
   * @returns Pair of (bytes received, sender SocketAddr). error_code on failure.
   */
  Task<Result<std::pair<size_t, SocketAddr>>> recv_from(
      std::span<std::byte> buf) {
    struct RecvFromAwaiter {
      int fd_;
      std::byte *data_;
      size_t size_;
      sockaddr_storage from_{};
      socklen_t fromlen_ = sizeof(sockaddr_storage);
      ssize_t result_ = -1;
      int err_ = 0;

      [[nodiscard]] bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (reactor == nullptr) { handle.resume(); return; }
        reactor->register_event(fd_, EventType::Read, [handle, this](int f) {
          result_ = ::recvfrom(f, data_, size_, 0,
                               reinterpret_cast<sockaddr *>(&from_), &fromlen_);
          err_ = (result_ < 0) ? errno : 0;
          Reactor::current()->unregister_event(f, EventType::Read);
          handle.resume();
        });
      }

      void await_resume() const noexcept {}
    };

    RecvFromAwaiter aw{fd_, buf.data(), buf.size()};
    co_await aw;

    if (aw.result_ < 0) {
      co_return std::unexpected(std::error_code(aw.err_, std::system_category()));
    }

    // Convert sockaddr_storage to SocketAddr
    SocketAddr from_addr;
    if (aw.from_.ss_family == AF_INET) {
      const auto *sa = reinterpret_cast<const sockaddr_in *>(&aw.from_);
      from_addr.family_ = SocketAddr::Family::IPv4;
      from_addr.port_   = ntohs(sa->sin_port);
      from_addr.addr_.ipv4_ = sa->sin_addr;
    } else if (aw.from_.ss_family == AF_INET6) {
      const auto *sa = reinterpret_cast<const sockaddr_in6 *>(&aw.from_);
      from_addr.family_ = SocketAddr::Family::IPv6;
      from_addr.port_   = ntohs(sa->sin6_port);
      from_addr.addr_.ipv6_ = sa->sin6_addr;
    }

    co_return std::make_pair(static_cast<size_t>(aw.result_), from_addr);
  }

  // ─── Accessors ───────────────────────────────────────────────────────────

  /**
   * @brief Returns the underlying file descriptor.
   * @returns Socket fd. -1 if invalid.
   */
  [[nodiscard]] int fd() const noexcept { return fd_; }

private:
  /** @brief The managed UDP socket file descriptor. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
