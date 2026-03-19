#pragma once

/**
 * @file qbuem/net/plain_transport.hpp
 * @brief Plain (non-TLS) TCP transport layer implementation.
 * @ingroup qbuem_net
 *
 * `PlainTransport` is the plain TCP implementation of the `ITransport` interface.
 * It wraps a `TcpStream` and communicates over a raw TCP socket without TLS.
 *
 * ### Design Principles
 * - Delegates all `ITransport` virtual methods to `TcpStream` (zero-overhead delegation)
 * - `handshake()` returns success immediately (no TLS handshake needed)
 * - `close()` calls TCP `shutdown(SHUT_WR)` then closes the socket
 * - Move-only: takes ownership of the `TcpStream`
 *
 * ### Usage Example
 * @code
 * auto stream = co_await TcpStream::connect(addr);
 * if (!stream) { // error handling }
 *
 * auto transport = std::make_unique<PlainTransport>(std::move(*stream));
 * // inject into Connection, etc.
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/core/transport.hpp>
#include <qbuem/net/tcp_stream.hpp>

#include <cstddef>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

namespace qbuem {

/**
 * @brief Plain TCP `ITransport` implementation (no TLS).
 *
 * Owns a `TcpStream` and delegates all `ITransport` virtual methods to it.
 *
 * ### handshake()
 * No TLS handshake — returns `Result<void>{}` immediately.
 *
 * ### close()
 * Terminates the write side with `shutdown(SHUT_WR)`, then closes the socket.
 * Calling `close()` on an already-closed transport is a no-op.
 */
class PlainTransport final : public ITransport {
public:
  /**
   * @brief Construct a PlainTransport by taking ownership of a `TcpStream`.
   *
   * @param stream `TcpStream` to take ownership of (moved in).
   */
  explicit PlainTransport(TcpStream stream) noexcept
      : stream_(std::move(stream)) {}

  /** @brief Destructor: the socket is closed automatically via `TcpStream` RAII. */
  ~PlainTransport() override = default;

  /** @brief Copy constructor deleted (Move-only). */
  PlainTransport(const PlainTransport &) = delete;

  /** @brief Copy assignment deleted (Move-only). */
  PlainTransport &operator=(const PlainTransport &) = delete;

  /** @brief Move constructor. */
  PlainTransport(PlainTransport &&) noexcept = default;

  /** @brief Move assignment operator. */
  PlainTransport &operator=(PlainTransport &&) noexcept = default;

  // ─── ITransport Implementation ────────────────────────────────────────────

  /**
   * @brief Delegates to `TcpStream::read()` to read data.
   *
   * @param buf Receive buffer.
   * @returns Number of bytes read. 0 on EOF. error_code on failure.
   */
  Task<Result<size_t>> read(std::span<std::byte> buf) override {
    co_return co_await stream_.read(buf);
  }

  /**
   * @brief Delegates to `TcpStream::write()` to write data.
   *
   * @param buf Send buffer.
   * @returns Number of bytes sent. error_code on failure.
   */
  Task<Result<size_t>> write(std::span<const std::byte> buf) override {
    co_return co_await stream_.write(buf);
  }

  /**
   * @brief No handshake required for plain TCP — returns success immediately.
   *
   * @returns Always `Result<void>{}`.
   */
  Task<Result<void>> handshake() override {
    co_return Result<void>{};
  }

  /**
   * @brief Shuts down the write side with `shutdown(SHUT_WR)` then closes the socket.
   *
   * If the socket is already closed (`fd() < 0`), returns ok immediately.
   *
   * @returns `Result<void>{}` on success, or an error code on failure.
   */
  Task<Result<void>> close() override {
    int fd = stream_.fd();
    if (fd < 0) {
      co_return Result<void>{};
    }
    ::shutdown(fd, SHUT_WR);
    // Move-destruct TcpStream so its destructor calls ::close(fd)
    TcpStream tmp = std::move(stream_);
    (void)tmp; // destructor calls ::close(fd)
    co_return Result<void>{};
  }

  /**
   * @brief Returns `""`. PlainTransport does not perform ALPN negotiation.
   *
   * @returns Empty string.
   */
  std::string_view negotiated_protocol() const noexcept override {
    return "";
  }

  /**
   * @brief Returns `""`. PlainTransport has no peer certificate.
   *
   * @returns Empty string.
   */
  std::string_view peer_certificate_fingerprint() const noexcept override {
    return "";
  }

  // ─── Extended Accessors ──────────────────────────────────────────────────

  /**
   * @brief Returns a reference to the internal `TcpStream`.
   *
   * Use this to adjust socket options such as TCP_NODELAY.
   *
   * @returns lvalue reference to `TcpStream`.
   */
  TcpStream &stream() noexcept { return stream_; }

  /**
   * @brief Returns a const reference to the internal `TcpStream`.
   *
   * @returns const lvalue reference to `TcpStream`.
   */
  const TcpStream &stream() const noexcept { return stream_; }

  /**
   * @brief Returns the underlying file descriptor.
   * @returns Socket fd. -1 if invalid.
   */
  [[nodiscard]] int fd() const noexcept { return stream_.fd(); }

private:
  /** @brief The owned TCP stream. */
  TcpStream stream_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
