#pragma once

/**
 * @file qbuem/core/transport.hpp
 * @brief TLS layer injection point — ITransport abstract interface
 * @defgroup qbuem_transport Transport
 * @ingroup qbuem_core
 *
 * qbuem-stack does not implement TLS directly.
 * Implementations such as OpenSSL, mbedTLS, or BoringSSL implement this
 * interface at the service layer and inject it into `Connection`.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace qbuem {

/**
 * @brief Abstract interface for a bidirectional stream transport layer.
 *
 * Both a plain TCP connection and a TLS wrapper are represented via this
 * interface. All I/O methods are non-blocking coroutines that suspend on the
 * Reactor thread without blocking.
 *
 * ### Lifetime Rules
 * - `read()`/`write()` must not be called after `close()`.
 * - The destructor is not required to guarantee `close()` — explicit calls
 *   are recommended.
 *
 * ### Implementation Guide
 * - `read()`:  On EAGAIN/EWOULDBLOCK register a read event with the Reactor and co_await.
 * - `write()`: On send-buffer saturation register a write event with the Reactor and co_await.
 * - TLS: Calling `read()`/`write()` before `handshake()` completes must return an error.
 */
class ITransport {
public:
  virtual ~ITransport() = default;

  /**
   * @brief Read data into `buf`.
   *
   * @param buf Receive buffer. Returns 0 immediately if the size is 0.
   * @returns Number of bytes read. 0 on EOS (EOF/connection closed).
   *          On error, an `errc::*` code.
   */
  virtual Task<Result<size_t>> read(std::span<std::byte> buf) = 0;

  /**
   * @brief Transmit all data in `buf`.
   *
   * @param buf Send buffer. Returns 0 immediately if empty.
   * @returns Number of bytes sent.
   */
  virtual Task<Result<size_t>> write(std::span<const std::byte> buf) = 0;

  /**
   * @brief Perform the TLS handshake.
   *
   * Plain TCP implementations return `ok()` immediately as a no-op.
   * TLS implementations must complete the handshake before `read()`/`write()` can be used.
   *
   * @returns `ok()` on success. TLS alert code on failure.
   */
  virtual Task<Result<void>> handshake() = 0;

  /**
   * @brief Close the transport layer.
   *
   * TLS implementations send a `close_notify` alert.
   * TCP implementations call `shutdown(SHUT_WR)` then close the socket.
   *
   * Calling this on an already-closed transport is a no-op.
   */
  virtual Task<Result<void>> close() = 0;

  /**
   * @brief Return the protocol name negotiated via ALPN.
   *
   * Only valid after a successful TLS handshake.
   *
   * @returns `"h2"` (HTTP/2), `"http/1.1"` (HTTP/1.1), or `""` (no negotiation).
   */
  virtual std::string_view negotiated_protocol() const noexcept { return ""; }

  /**
   * @brief Return the peer X.509 certificate fingerprint (optional).
   *
   * Used for client authentication in mTLS environments.
   * Returns an empty string if not supported.
   */
  virtual std::string_view peer_certificate_fingerprint() const noexcept {
    return "";
  }
};

} // namespace qbuem

/** @} */
