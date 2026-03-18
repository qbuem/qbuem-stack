#pragma once

/**
 * @file qbuem/transport/plain_transport.hpp
 * @brief Concrete ITransport implementation for plain TCP connections.
 * @defgroup qbuem_transport_impl Transport Implementations
 * @ingroup qbuem_transport
 *
 * `PlainTransport` is a concrete `ITransport` implementation that uses
 * a plain TCP fd without TLS.
 *
 * ### Design principles
 * - Fully implements the `ITransport` interface
 * - Reactor-based async I/O via `AsyncRead` / `AsyncWrite` awaiters
 * - `handshake()`: returns ok() immediately since this is plain TCP (no-op)
 * - `close()`: closes the fd after `shutdown(SHUT_WR)`
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/core/transport.hpp>

#include <cerrno>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

namespace qbuem {

/**
 * @brief Concrete ITransport implementation for plain TCP connections.
 *
 * Wraps a non-blocking TCP socket fd and implements the `ITransport` interface.
 * Performs direct reads and writes to the socket without TLS processing.
 *
 * ### Typical usage pattern
 * @code
 * // Inject the fd obtained from TcpListener::accept() into PlainTransport
 * auto transport = std::make_unique<PlainTransport>(client_fd);
 * // Inject into Connection, etc., for use
 * @endcode
 *
 * @note The destructor does not close the fd. Close it by calling `close()` explicitly
 *       or when the owning TcpStream is destroyed.
 *       Use this class when fd lifetime is managed externally.
 */
class PlainTransport : public ITransport {
public:
  /**
   * @brief Constructs a PlainTransport with the specified fd.
   *
   * @param fd Non-blocking TCP socket file descriptor.
   *           Must have `O_NONBLOCK` set.
   */
  explicit PlainTransport(int fd) noexcept : fd_(fd) {}

  /**
   * @brief Destructor.
   *
   * Does not close the fd. The fd lifetime is managed by the caller.
   * Call `close()` explicitly if this instance should own the fd.
   */
  ~PlainTransport() override = default;

  // ─── ITransport implementation ──────────────────────────────────────────

  /**
   * @brief Asynchronously reads data from the socket.
   *
   * Registers with the Reactor via the `AsyncRead` awaiter until the socket becomes readable.
   *
   * @param buf Buffer to store received data. Returns 0 immediately if empty.
   * @returns Number of bytes read. 0 on EOF/connection close. error_code on error.
   */
  Task<Result<size_t>> read(std::span<std::byte> buf) override {
    if (buf.empty()) co_return size_t{0};
    ssize_t n = co_await AsyncRead{fd_, buf.data(), buf.size()};
    if (n < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief Asynchronously writes data to the socket.
   *
   * Registers with the Reactor via the `AsyncWrite` awaiter until the socket becomes writable.
   *
   * @param buf Data buffer to send. Returns 0 immediately if empty.
   * @returns Number of bytes sent. error_code on error.
   */
  Task<Result<size_t>> write(std::span<const std::byte> buf) override {
    if (buf.empty()) co_return size_t{0};
    ssize_t n = co_await AsyncWrite{fd_, buf.data(), buf.size()};
    if (n < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief TLS handshake (no-op for plain TCP).
   *
   * No handshake is required for plain TCP connections, so ok() is returned immediately.
   *
   * @returns Always `Result<void>::ok()`.
   */
  Task<Result<void>> handshake() override {
    co_return Result<void>::ok();
  }

  /**
   * @brief Closes the transport layer.
   *
   * Terminates writes via `shutdown(SHUT_WR)` and then closes the fd.
   * Is a no-op if already closed.
   *
   * @returns `Result<void>::ok()` on success, error code on failure.
   */
  Task<Result<void>> close() override {
    if (fd_ < 0) {
      co_return Result<void>::ok();
    }
    int fd = fd_;
    fd_ = -1;
    // Send FIN by shutting down writes (receive side remains open)
    ::shutdown(fd, SHUT_WR);
    if (::close(fd) != 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return Result<void>::ok();
  }

  /**
   * @brief Returns the ALPN negotiated protocol (empty string for plain TCP).
   * @returns Always `""`.
   */
  std::string_view negotiated_protocol() const noexcept override {
    return "";
  }

  // ─── Accessors ───────────────────────────────────────────────────────────

  /**
   * @brief Returns the internal file descriptor.
   * @returns Socket fd. -1 after close().
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief The managed TCP socket file descriptor. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_transport_impl
