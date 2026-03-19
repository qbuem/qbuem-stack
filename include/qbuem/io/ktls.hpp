#pragma once

/**
 * @file qbuem/io/ktls.hpp
 * @brief Kernel TLS (kTLS) integration — `setsockopt(SOL_TLS, TLS_TX/RX)` support.
 * @defgroup qbuem_ktls Kernel TLS
 * @ingroup qbuem_io
 *
 * kTLS offloads TLS encryption to kernel space, reducing the number of
 * system calls and memory copies compared to user-space TLS implementations.
 *
 * ## Prerequisites
 * - Linux 4.13+ (TLS_TX), Linux 4.17+ (TLS_RX)
 * - `CONFIG_TLS` kernel build option enabled
 * - Handshake must be completed by a user-space TLS library (OpenSSL / BoringSSL, etc.)
 *   before passing session keys to the kernel
 *
 * ## Usage Flow
 * ```
 * 1. Create TCP socket + TLS handshake (user-space)
 * 2. Extract session keys (tx_key, rx_key, iv, seq)
 * 3. ktls::enable_tx(fd, params) — offload transmit encryption to the kernel
 * 4. ktls::enable_rx(fd, params) — offload receive decryption to the kernel
 * 5. Subsequent send()/recv() → kernel handles encryption/decryption directly
 * ```
 *
 * ## sendfile + kTLS Combination
 * Using `sendfile(2)` on a kTLS-enabled socket allows static files to be
 * transmitted with TLS encryption and zero user-space copies.
 *
 * ## Platform Support
 * - Linux: `SOL_TLS`, `TLS_TX`, `TLS_RX` (linux/tls.h)
 * - Non-Linux: all functions return `errc::not_supported`
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__linux__)
#  include <sys/sendfile.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  if __has_include(<linux/tls.h>)
#    include <linux/tls.h>
#    define QBUEM_HAS_KTLS 1
#  endif
#endif

namespace qbuem::io {

// ---------------------------------------------------------------------------
// TLS Session Parameters
// ---------------------------------------------------------------------------

/**
 * @brief TLS 1.3 AES-128-GCM session parameters.
 *
 * Contains session key material extracted from a user-space TLS library
 * after handshake completion. Passed directly to the kernel kTLS configuration.
 */
struct KtlsSessionParams {
  std::array<uint8_t, 16> key;     ///< AES-128 encryption key
  std::array<uint8_t, 12> iv;      ///< IV (implicit nonce: 4-byte salt + 8-byte explicit nonce)
  std::array<uint8_t, 8>  seq;     ///< TLS record sequence number (big-endian)
};

/**
 * @brief TLS 1.3 AES-256-GCM session parameters.
 */
struct KtlsSessionParams256 {
  std::array<uint8_t, 32> key;     ///< AES-256 encryption key
  std::array<uint8_t, 12> iv;      ///< IV
  std::array<uint8_t, 8>  seq;     ///< TLS record sequence number
};

// ---------------------------------------------------------------------------
// kTLS Enable Functions
// ---------------------------------------------------------------------------

/**
 * @brief Enables kTLS on the transmit path of a socket (TLS 1.3 AES-128-GCM).
 *
 * After calling `setsockopt(fd, SOL_TLS, TLS_TX, ...)`, subsequent
 * `send()/write()` calls perform TLS encryption inside the kernel.
 *
 * @param sockfd  TCP socket fd with completed TLS handshake.
 * @param params  TLS session parameters (key, IV, sequence number).
 * @returns `Result<void>` on success, or an error code on failure.
 */
[[nodiscard]] inline Result<void> enable_tx(
    int sockfd, const KtlsSessionParams &params) noexcept {
#if defined(QBUEM_HAS_KTLS) && defined(TLS_TX)
  // Compile-time guards: AES-128-GCM requires a 12-byte IV (4-byte salt +
  // 8-byte explicit nonce) and a 16-byte key.  These assertions catch any
  // accidental struct field-size changes before a bad memcpy occurs.
  static_assert(KtlsSessionParams{}.iv.size()  == 12,
                "KtlsSessionParams::iv must be exactly 12 bytes (TLS 1.3 GCM nonce)");
  static_assert(KtlsSessionParams{}.key.size() == 16,
                "KtlsSessionParams::key must be exactly 16 bytes (AES-128)");
  struct tls12_crypto_info_aes_gcm_128 crypto_info{};
  crypto_info.info.version    = TLS_1_3_VERSION;
  crypto_info.info.cipher_type = TLS_CIPHER_AES_GCM_128;
  std::memcpy(crypto_info.key, params.key.data(), sizeof(crypto_info.key));
  std::memcpy(crypto_info.iv,  params.iv.data() + 4,
              sizeof(crypto_info.iv));  // explicit nonce (8 bytes)
  std::memcpy(crypto_info.salt, params.iv.data(),
              sizeof(crypto_info.salt)); // implicit salt (4 bytes)
  std::memcpy(crypto_info.rec_seq, params.seq.data(),
              sizeof(crypto_info.rec_seq));
  if (::setsockopt(sockfd, SOL_TLS, TLS_TX, &crypto_info, sizeof(crypto_info)) < 0) {
    return std::unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd; (void)params;
  return std::unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

/**
 * @brief Enables kTLS on the receive path of a socket (TLS 1.3 AES-128-GCM).
 *
 * After calling `setsockopt(fd, SOL_TLS, TLS_RX, ...)`, subsequent
 * `recv()/read()` calls perform TLS decryption inside the kernel.
 *
 * @param sockfd  TCP socket fd with completed TLS handshake.
 * @param params  TLS session parameters.
 * @returns `Result<void>` on success, or an error code on failure.
 */
[[nodiscard]] inline Result<void> enable_rx(
    int sockfd, const KtlsSessionParams &params) noexcept {
#if defined(QBUEM_HAS_KTLS) && defined(TLS_RX)
  static_assert(KtlsSessionParams{}.iv.size()  == 12,
                "KtlsSessionParams::iv must be exactly 12 bytes (TLS 1.3 GCM nonce)");
  static_assert(KtlsSessionParams{}.key.size() == 16,
                "KtlsSessionParams::key must be exactly 16 bytes (AES-128)");
  struct tls12_crypto_info_aes_gcm_128 crypto_info{};
  crypto_info.info.version    = TLS_1_3_VERSION;
  crypto_info.info.cipher_type = TLS_CIPHER_AES_GCM_128;
  std::memcpy(crypto_info.key, params.key.data(), sizeof(crypto_info.key));
  std::memcpy(crypto_info.iv,  params.iv.data() + 4, sizeof(crypto_info.iv));
  std::memcpy(crypto_info.salt, params.iv.data(), sizeof(crypto_info.salt));
  std::memcpy(crypto_info.rec_seq, params.seq.data(),
              sizeof(crypto_info.rec_seq));
  if (::setsockopt(sockfd, SOL_TLS, TLS_RX, &crypto_info, sizeof(crypto_info)) < 0) {
    return std::unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd; (void)params;
  return std::unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

/**
 * @brief Enables kTLS on both TX and RX paths of a socket in one call.
 *
 * @param sockfd    TLS socket fd.
 * @param tx_params Transmit session parameters.
 * @param rx_params Receive session parameters.
 * @returns `Result<void>` on success. Returns an error if either TX or RX fails.
 */
[[nodiscard]] inline Result<void> enable_ktls(
    int sockfd,
    const KtlsSessionParams &tx_params,
    const KtlsSessionParams &rx_params) noexcept {
  if (auto r = enable_tx(sockfd, tx_params); !r) return r;
  return enable_rx(sockfd, rx_params);
}

// ---------------------------------------------------------------------------
// sendfile + kTLS zero-copy encrypted transfer
// ---------------------------------------------------------------------------

/**
 * @brief Zero-copy encrypted file transfer via `sendfile(2)` on a kTLS socket.
 *
 * ## How It Works
 * When `sendfile(2)` is called on a kTLS-enabled socket, the kernel:
 * 1. Zero-copies file data from page cache to the socket buffer.
 * 2. Performs AES-GCM encryption in the kTLS layer before NIC transmission.
 * 3. The result is an **encrypted TLS record transmitted without any user-space copy**.
 *
 * ## Prerequisites
 * - Must be called after `prepare_socket_for_ktls()` + `enable_tx()`.
 * - Linux 4.17+ (TLS_TX sendfile support).
 *
 * ## Large File Handling
 * If `count == 0`, the entire file from `offset` is transferred.
 * Bytes not sent in a single call are completed via a retry loop.
 *
 * @param sockfd  Socket fd with kTLS TX enabled.
 * @param filefd  File fd to transfer (opened with `O_RDONLY`).
 * @param offset  Start offset within the file. Advanced by the number of bytes sent.
 * @param count   Maximum bytes to transfer. 0 means to end of file.
 * @returns Total bytes transferred, or an error.
 *
 * @note Returns `errc::not_supported` on non-Linux platforms.
 */
[[nodiscard]] inline Result<size_t> ktls_sendfile(
    int      sockfd,
    int      filefd,
    off_t   &offset,
    size_t   count = 0) noexcept {
#if defined(__linux__)
  size_t total = 0;
  for (;;) {
    size_t chunk = (count == 0 || (count - total) > (1u << 30))
                       ? (1u << 30)  // max 1GB chunk
                       : (count - total);
    ssize_t sent = ::sendfile(sockfd, filefd, &offset, chunk);
    if (sent > 0) {
      total += static_cast<size_t>(sent);
      if (count != 0 && total >= count) break;
      continue;
    }
    if (sent == 0) break; // EOF
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Non-blocking socket: return however much has been sent so far
      if (total > 0) break;
      return std::unexpected(std::error_code{errno, std::system_category()});
    }
    return std::unexpected(std::error_code{errno, std::system_category()});
  }
  return total;
#else
  (void)sockfd; (void)filefd; (void)offset; (void)count;
  return std::unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

/**
 * @brief kTLS sendfile helper: sends an entire file starting from offset 0.
 *
 * @param sockfd  kTLS TX socket fd.
 * @param filefd  File fd to transfer.
 * @returns Total bytes transferred, or an error.
 */
[[nodiscard]] inline Result<size_t> ktls_sendfile_all(
    int sockfd, int filefd) noexcept {
  off_t offset = 0;
  return ktls_sendfile(sockfd, filefd, offset, 0);
}

// ---------------------------------------------------------------------------
// kTLS graceful fallback
// ---------------------------------------------------------------------------

/**
 * @brief Attempts kTLS and automatically falls back to plain TCP if unsupported.
 *
 * If `enable_ktls()` fails with `errc::not_supported`, the warning is silently
 * ignored and `Result<void>` success is returned. In this case subsequent
 * send()/recv() calls remain handled by the user-space TLS library (e.g. OpenSSL).
 *
 * Other errors (permission denied, invalid parameters, etc.) are propagated as-is.
 *
 * ### Usage Example
 * @code
 * // Use kernel offload if kTLS is supported; otherwise fall back to user-space TLS
 * auto r = qbuem::io::enable_ktls_with_fallback(fd, tx_params, rx_params);
 * if (!r) {
 *     // Actual error (not not_supported)
 *     return r;
 * }
 * @endcode
 *
 * @param sockfd    TCP socket fd with completed TLS handshake.
 * @param tx_params Transmit session parameters.
 * @param rx_params Receive session parameters.
 * @returns `Result<void>` ok on success or fallback; error on fatal failure.
 */
[[nodiscard]] inline Result<void> enable_ktls_with_fallback(
    int sockfd,
    const KtlsSessionParams &tx_params,
    const KtlsSessionParams &rx_params) noexcept {
  auto r = enable_ktls(sockfd, tx_params, rx_params);
  if (!r && r.error() == std::make_error_code(std::errc::not_supported)) {
    // kTLS not available on this kernel — silently fall back to user-space TLS.
    return Result<void>{};
  }
  return r;
}

// ---------------------------------------------------------------------------
// kTLS status checks
// ---------------------------------------------------------------------------

/**
 * @brief Checks whether kTLS TX is enabled on a socket.
 *
 * @param sockfd Socket fd to check.
 * @returns true if kTLS TX is enabled.
 */
[[nodiscard]] inline bool is_ktls_tx_enabled(int sockfd) noexcept {
#if defined(__linux__) && defined(SOL_TLS) && defined(TLS_TX)
  int optval = 0;
  socklen_t optlen = sizeof(optval);
  return ::getsockopt(sockfd, SOL_TLS, TLS_TX, &optval, &optlen) == 0;
#else
  (void)sockfd;
  return false;
#endif
}

/**
 * @brief Checks whether kTLS RX is enabled on a socket.
 */
[[nodiscard]] inline bool is_ktls_rx_enabled(int sockfd) noexcept {
#if defined(__linux__) && defined(SOL_TLS) && defined(TLS_RX)
  int optval = 0;
  socklen_t optlen = sizeof(optval);
  return ::getsockopt(sockfd, SOL_TLS, TLS_RX, &optval, &optlen) == 0;
#else
  (void)sockfd;
  return false;
#endif
}

// ---------------------------------------------------------------------------
// SOL_TLS socket preparation
// ---------------------------------------------------------------------------

/**
 * @brief Prepares a TCP socket for kTLS usage.
 *
 * Call after TLS handshake completion and before enabling kTLS.
 * Internally uses `setsockopt(fd, IPPROTO_TCP, TCP_ULP, "tls", 3)`.
 *
 * @param sockfd TCP socket fd to prepare.
 * @returns `Result<void>` on success.
 */
[[nodiscard]] inline Result<void> prepare_socket_for_ktls(int sockfd) noexcept {
#if defined(__linux__)
  if (::setsockopt(sockfd, IPPROTO_TCP, TCP_ULP, "tls", sizeof("tls") - 1) < 0) {
    return std::unexpected(std::error_code{errno, std::system_category()});
  }
  return Result<void>{};
#else
  (void)sockfd;
  return std::unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

} // namespace qbuem::io

/** @} */
