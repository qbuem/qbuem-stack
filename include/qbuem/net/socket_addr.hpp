#pragma once

/**
 * @file qbuem/net/socket_addr.hpp
 * @brief 플랫폼 독립적인 소켓 주소 값 타입 정의.
 * @defgroup qbuem_net Network Primitives
 * @ingroup qbuem_net
 *
 * IPv4, IPv6, Unix 도메인 소켓 주소를 힙 할당 없이 표현하는 값 타입을 제공합니다.
 *
 * ### 설계 목표
 * - 힙 할당 없음 (스택 값 타입)
 * - 플랫폼 sockaddr 구조체 직접 변환 지원
 * - 문자열 표현은 고정 버퍼에 zero-alloc으로 생성
 * @{
 */

#include <qbuem/common.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstdint>
#include <cstring>

namespace qbuem {

/**
 * @brief IPv4/IPv6/Unix 도메인 소켓 주소를 표현하는 값 타입.
 *
 * 세 가지 주소 패밀리를 하나의 구조체로 표현합니다.
 * 모든 저장소는 스택에 위치하며 힙 할당이 발생하지 않습니다.
 *
 * ### 사용 예시
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
   * @brief 소켓 주소 패밀리 열거형.
   */
  enum class Family {
    IPv4, ///< AF_INET (IPv4)
    IPv6, ///< AF_INET6 (IPv6)
    Unix  ///< AF_UNIX (Unix 도메인)
  };

  /** @brief 저장된 주소 패밀리. */
  Family family_ = Family::IPv4;

  /** @brief 포트 번호 (Unix 도메인의 경우 미사용, 0으로 설정). */
  uint16_t port_ = 0;

  /** @brief 주소 저장 유니온. */
  union {
    in_addr  ipv4_;         ///< IPv4 주소 저장소
    in6_addr ipv6_;         ///< IPv6 주소 저장소
    char     unix_[108];    ///< Unix 도메인 소켓 경로 (sun_path 최대 크기)
  } addr_{};

  // ─── 팩토리 메서드 ──────────────────────────────────────────────────────

  /**
   * @brief IPv4 주소와 포트로 SocketAddr를 생성합니다.
   *
   * @param ip   점-십진 표기법 IPv4 주소 문자열 (예: "127.0.0.1").
   * @param port 포트 번호 (호스트 바이트 순서).
   * @returns 파싱 성공 시 SocketAddr, 실패 시 에러 코드.
   */
  static Result<SocketAddr> from_ipv4(const char *ip, uint16_t port) noexcept {
    SocketAddr a;
    a.family_ = Family::IPv4;
    a.port_   = port;
    if (inet_pton(AF_INET, ip, &a.addr_.ipv4_) != 1)
      return unexpected(
          std::make_error_code(std::errc::invalid_argument));
    return a;
  }

  /**
   * @brief IPv6 주소와 포트로 SocketAddr를 생성합니다.
   *
   * @param ip   콜론 표기법 IPv6 주소 문자열 (예: "::1").
   * @param port 포트 번호 (호스트 바이트 순서).
   * @returns 파싱 성공 시 SocketAddr, 실패 시 에러 코드.
   */
  static Result<SocketAddr> from_ipv6(const char *ip, uint16_t port) noexcept {
    SocketAddr a;
    a.family_ = Family::IPv6;
    a.port_   = port;
    if (inet_pton(AF_INET6, ip, &a.addr_.ipv6_) != 1)
      return unexpected(
          std::make_error_code(std::errc::invalid_argument));
    return a;
  }

  /**
   * @brief Unix 도메인 소켓 경로로 SocketAddr를 생성합니다.
   *
   * @param path 소켓 파일 경로. 길이는 107바이트 이하여야 합니다.
   * @returns 생성 성공 시 SocketAddr, 경로가 너무 길면 에러 코드.
   */
  static Result<SocketAddr> from_unix(const char *path) noexcept {
    SocketAddr a;
    a.family_ = Family::Unix;
    a.port_   = 0;
    size_t len = __builtin_strlen(path);
    if (len >= sizeof(a.addr_.unix_))
      return unexpected(
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

  // ─── 플랫폼 sockaddr 변환 ───────────────────────────────────────────────

  /**
   * @brief 플랫폼 sockaddr_storage 구조체를 채웁니다.
   *
   * `connect()`, `bind()` 등의 syscall에 전달할 sockaddr를 생성합니다.
   *
   * @param[out] out 채워질 sockaddr_storage 구조체.
   * @param[out] len 실제 사용된 구조체의 크기.
   * @returns 성공 시 `Result<void>::ok()`, 실패 시 에러 코드.
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
      return Result<void>::ok();
    }
    case Family::IPv6: {
      auto *sa = reinterpret_cast<sockaddr_in6 *>(&out);
      sa->sin6_family = AF_INET6;
      sa->sin6_port   = htons(port_);
      sa->sin6_addr   = addr_.ipv6_;
      len = sizeof(sockaddr_in6);
      return Result<void>::ok();
    }
    case Family::Unix: {
      auto *sa = reinterpret_cast<sockaddr_un *>(&out);
      sa->sun_family = AF_UNIX;
      __builtin_strncpy(sa->sun_path, addr_.unix_, sizeof(sa->sun_path) - 1);
      len = static_cast<socklen_t>(
          offsetof(sockaddr_un, sun_path) + __builtin_strlen(addr_.unix_) + 1);
      return Result<void>::ok();
    }
    }
    return unexpected(std::make_error_code(std::errc::address_family_not_supported));
  }

  // ─── 문자열 변환 (zero-alloc) ───────────────────────────────────────────

  /**
   * @brief 주소를 사람이 읽을 수 있는 문자열로 변환합니다 (힙 할당 없음).
   *
   * - IPv4: `"127.0.0.1:8080"`
   * - IPv6: `"[::1]:8080"`
   * - Unix: `"unix:/tmp/my.sock"`
   *
   * @param buf  결과를 저장할 버퍼.
   * @param n    버퍼 크기 (최소 INET6_ADDRSTRLEN + 10 권장).
   * @returns 저장된 문자 수 (null 종결자 제외). 버퍼 부족이면 -1.
   */
  int to_chars(char *buf, size_t n) const noexcept {
    switch (family_) {
    case Family::IPv4: {
      char ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &addr_.ipv4_, ip, sizeof(ip));
      return __builtin_snprintf(buf, n, "%s:%u", ip, static_cast<unsigned>(port_));
    }
    case Family::IPv6: {
      char ip[INET6_ADDRSTRLEN];
      inet_ntop(AF_INET6, &addr_.ipv6_, ip, sizeof(ip));
      return __builtin_snprintf(buf, n, "[%s]:%u", ip, static_cast<unsigned>(port_));
    }
    case Family::Unix: {
      return __builtin_snprintf(buf, n, "unix:%s", addr_.unix_);
    }
    }
    return -1;
  }

  // ─── 접근자 ─────────────────────────────────────────────────────────────

  /** @brief 소켓 주소 패밀리를 반환합니다. */
  Family family() const noexcept { return family_; }

  /**
   * @brief 포트 번호를 반환합니다 (호스트 바이트 순서).
   * @returns IPv4/IPv6는 포트 번호, Unix 도메인은 0.
   */
  uint16_t port() const noexcept { return port_; }
};

} // namespace qbuem

/** @} */ // end of qbuem_net
