#pragma once

/**
 * @file qbuem/net/socket_addr.hpp
 * @brief Platform-independent socket address value type definition.
 * @defgroup qbuem_net Network Primitives
 * @ingroup qbuem_net
 *
 * Provides a value type that represents IPv4, IPv6, and Unix domain socket
 * addresses without heap allocation.
 *
 * ### Design Goals
 * - No heap allocation (stack value type)
 * - Direct conversion to/from platform sockaddr structures
 * - String representation generated in a fixed buffer with zero allocation
 * @{
 */

#include <qbuem/common.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstdint>
#include <cstring>
#include <format>

namespace qbuem {

/**
 * @brief Value type representing an IPv4, IPv6, or Unix domain socket address.
 *
 * Encodes three address families in a single struct.
 * All storage lives on the stack; no heap allocation occurs.
 *
 * ### Usage Example
 * @code
 * auto addr4 = SocketAddr::from_ipv4("127.0.0.1", 8080);
 * auto addr6 = SocketAddr::from_ipv6("::1", 8080);
 * auto addru = SocketAddr::from_unix("/tmp/my.sock");
 *
 * sockaddr_storage ss{};
 * socklen_t len{};
 * addr4.to_sockaddr(ss, len);
 * @endcode
 */
struct SocketAddr {
  /**
   * @brief Socket address family enumeration.
   */
  enum class Family {
    IPv4, ///< AF_INET (IPv4)
    IPv6, ///< AF_INET6 (IPv6)
    Unix  ///< AF_UNIX (Unix domain)
  };

  /** @brief Stored address family. */
  Family family_ = Family::IPv4;

  /** @brief Port number (unused for Unix domain, set to 0). */
  uint16_t port_ = 0;

  /** @brief Address storage union. */
  union {
    in_addr  ipv4_;         ///< IPv4 address storage
    in6_addr ipv6_;         ///< IPv6 address storage
    char     unix_[108];    // NOLINT(modernize-avoid-c-arrays) ///< Unix domain socket path (max sun_path size)
  } addr_{};

  // ─── Factory Methods ────────────────────────────────────────────────────────

  /**
   * @brief Create a SocketAddr from an IPv4 address and port.
   *
   * @param ip   Dotted-decimal IPv4 address string (e.g., "127.0.0.1").
   * @param port Port number in host byte order.
   * @returns SocketAddr on successful parsing, or an error code on failure.
   */
  static Result<SocketAddr> from_ipv4(const char *ip, uint16_t port) noexcept {
    SocketAddr a;
    a.family_ = Family::IPv4;
    a.port_   = port;
    if (inet_pton(AF_INET, ip, &a.addr_.ipv4_) != 1)
      return std::unexpected(
          std::make_error_code(std::errc::invalid_argument));
    return a;
  }

  /**
   * @brief Create a SocketAddr from an IPv6 address and port.
   *
   * @param ip   Colon-notation IPv6 address string (e.g., "::1").
   * @param port Port number in host byte order.
   * @returns SocketAddr on successful parsing, or an error code on failure.
   */
  static Result<SocketAddr> from_ipv6(const char *ip, uint16_t port) noexcept {
    SocketAddr a;
    a.family_ = Family::IPv6;
    a.port_   = port;
    if (inet_pton(AF_INET6, ip, &a.addr_.ipv6_) != 1)
      return std::unexpected(
          std::make_error_code(std::errc::invalid_argument));
    return a;
  }

  /**
   * @brief Create a SocketAddr from a Unix domain socket path.
   *
   * @param path Socket file path. Must be 107 bytes or fewer.
   * @returns SocketAddr on success, or an error code if the path is too long.
   */
  static Result<SocketAddr> from_unix(const char *path) noexcept {
    SocketAddr a;
    a.family_ = Family::Unix;
    a.port_   = 0;
    size_t len = __builtin_strlen(path);
    if (len >= sizeof(a.addr_.unix_))
      return std::unexpected(
          std::make_error_code(std::errc::invalid_argument));
    __builtin_memcpy(a.addr_.unix_, path, len + 1);
    return a;
  }

  /**
   * @brief Construct SocketAddr directly from a sockaddr_in binary struct.
   *
   * Avoids the inet_ntop → string → inet_pton round-trip used by from_ipv4().
   * Use this in DNS resolution after getaddrinfo() returns a sockaddr_in.
   *
   * @param sa    Filled sockaddr_in from getaddrinfo/accept/etc.
   * @param port  Port in host byte order.
   */
  static SocketAddr from_sockaddr_in(const sockaddr_in &sa,
                                     uint16_t port) noexcept {
    SocketAddr a;
    a.family_     = Family::IPv4;
    a.port_       = port;
    a.addr_.ipv4_ = sa.sin_addr;
    return a;
  }

  /**
   * @brief Construct SocketAddr directly from a sockaddr_in6 binary struct.
   *
   * Avoids the inet_ntop → string → inet_pton round-trip used by from_ipv6().
   *
   * @param sa    Filled sockaddr_in6 from getaddrinfo/accept/etc.
   * @param port  Port in host byte order.
   */
  static SocketAddr from_sockaddr_in6(const sockaddr_in6 &sa,
                                      uint16_t port) noexcept {
    SocketAddr a;
    a.family_     = Family::IPv6;
    a.port_       = port;
    a.addr_.ipv6_ = sa.sin6_addr;
    return a;
  }

  // ─── Platform sockaddr Conversion ──────────────────────────────────────────

  /**
   * @brief Fill a platform sockaddr_storage structure.
   *
   * Produces a sockaddr suitable for passing to syscalls such as
   * `connect()` and `bind()`.
   *
   * @param[out] out sockaddr_storage structure to fill.
   * @param[out] len Actual size of the filled structure.
   * @returns `Result<void>{}` on success, or an error code on failure.
   */
  Result<void> to_sockaddr(sockaddr_storage &out, socklen_t &len) const noexcept {
    __builtin_memset(&out, 0, sizeof(out));
    switch (family_) {
    case Family::IPv4: {
      auto *sa = reinterpret_cast<sockaddr_in *>(&out);
      sa->sin_family = AF_INET;
      sa->sin_port   = htons(port_);
      sa->sin_addr   = addr_.ipv4_;
      len = sizeof(sockaddr_in);
      return Result<void>{};
    }
    case Family::IPv6: {
      auto *sa = reinterpret_cast<sockaddr_in6 *>(&out);
      sa->sin6_family = AF_INET6;
      sa->sin6_port   = htons(port_);
      sa->sin6_addr   = addr_.ipv6_;
      len = sizeof(sockaddr_in6);
      return Result<void>{};
    }
    case Family::Unix: {
      auto *sa = reinterpret_cast<sockaddr_un *>(&out);
      sa->sun_family = AF_UNIX;
      __builtin_strncpy(sa->sun_path, addr_.unix_, sizeof(sa->sun_path) - 1);
      len = static_cast<socklen_t>(
          offsetof(sockaddr_un, sun_path) + __builtin_strlen(addr_.unix_) + 1);
      return Result<void>{};
    }
    }
    return std::unexpected(std::make_error_code(std::errc::address_family_not_supported));
  }

  // ─── String Conversion (zero-alloc) ────────────────────────────────────────

  /**
   * @brief Convert the address to a human-readable string without heap allocation.
   *
   * - IPv4: `"127.0.0.1:8080"`
   * - IPv6: `"[::1]:8080"`
   * - Unix: `"unix:/tmp/my.sock"`
   *
   * @param buf  Buffer to store the result.
   * @param n    Buffer size (at least INET6_ADDRSTRLEN + 10 recommended).
   * @returns Number of characters written (excluding null terminator). -1 if buffer too small.
   */
  int to_chars(char *buf, size_t n) const noexcept {
    if (n == 0) return -1;
    switch (family_) {
    case Family::IPv4: {
      char ip[INET_ADDRSTRLEN]; // NOLINT(modernize-avoid-c-arrays)
      inet_ntop(AF_INET, &addr_.ipv4_, ip, sizeof(ip));
      auto r = std::format_to_n(buf, n - 1, "{}:{}", ip, static_cast<unsigned>(port_));
      *r.out = '\0';
      return static_cast<int>(r.out - buf);
    }
    case Family::IPv6: {
      char ip[INET6_ADDRSTRLEN]; // NOLINT(modernize-avoid-c-arrays)
      inet_ntop(AF_INET6, &addr_.ipv6_, ip, sizeof(ip));
      auto r = std::format_to_n(buf, n - 1, "[{}]:{}", ip, static_cast<unsigned>(port_));
      *r.out = '\0';
      return static_cast<int>(r.out - buf);
    }
    case Family::Unix: {
      auto r = std::format_to_n(buf, n - 1, "unix:{}", addr_.unix_);
      *r.out = '\0';
      return static_cast<int>(r.out - buf);
    }
    }
    return -1;
  }

  // ─── Accessors ──────────────────────────────────────────────────────────────

  /** @brief Returns the socket address family. */
  [[nodiscard]] Family family() const noexcept { return family_; }

  /**
   * @brief Returns the port number in host byte order.
   * @returns Port number for IPv4/IPv6, 0 for Unix domain.
   */
  [[nodiscard]] uint16_t port() const noexcept { return port_; }
};

} // namespace qbuem

/** @} */ // end of qbuem_net
