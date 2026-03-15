#pragma once

/**
 * @file qbuem/net/udp_socket.hpp
 * @brief 비동기 UDP 소켓 타입.
 * @ingroup qbuem_net
 *
 * `UdpSocket`은 논블로킹 UDP 소켓을 RAII로 관리하며,
 * 코루틴 기반의 비동기 송수신 인터페이스를 제공합니다.
 *
 * ### 설계 원칙
 * - Move-only: 복사 불가, 소멸자에서 fd 자동 close
 * - `send_to`: 목적지 주소 지정 송신
 * - `recv_from`: 수신 데이터 및 발신자 주소 반환
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
 * @brief 비동기 UDP 소켓.
 *
 * 논블로킹 UDP 소켓을 래핑하여 코루틴 기반 송수신 인터페이스를 제공합니다.
 * Move-only 타입이며 소멸 시 소켓이 자동으로 닫힙니다.
 *
 * ### 사용 예시
 * @code
 * auto addr = SocketAddr::from_ipv4("0.0.0.0", 9000);
 * auto sock = UdpSocket::bind(*addr);
 * if (!sock) { // 에러 처리 }
 *
 * std::array<std::byte, 1500> buf{};
 * auto [n, from] = co_await sock->recv_from(buf);
 * @endcode
 */
class UdpSocket {
public:
  /** @brief 유효하지 않은 fd로 초기화. */
  UdpSocket() noexcept : fd_(-1) {}

  /**
   * @brief 이미 바인딩된 fd로 UdpSocket을 구성합니다.
   * @param fd 논블로킹 UDP 소켓 파일 디스크립터.
   */
  explicit UdpSocket(int fd) noexcept : fd_(fd) {}

  /** @brief 소멸자: 열린 소켓을 자동으로 닫습니다. */
  ~UdpSocket() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @brief 복사 생성자 삭제 (Move-only). */
  UdpSocket(const UdpSocket &) = delete;

  /** @brief 복사 대입 삭제 (Move-only). */
  UdpSocket &operator=(const UdpSocket &) = delete;

  /** @brief 이동 생성자. */
  UdpSocket(UdpSocket &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief 이동 대입 연산자. */
  UdpSocket &operator=(UdpSocket &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── 팩토리 메서드 ──────────────────────────────────────────────────────

  /**
   * @brief 지정된 주소에 바인딩하는 UDP 소켓을 생성합니다.
   *
   * @param addr 바인딩할 로컬 주소. IPv4/IPv6 모두 지원.
   * @returns 성공 시 UdpSocket, 실패 시 에러 코드.
   */
  static Result<UdpSocket> bind(SocketAddr addr) noexcept {
    int domain = (addr.family() == SocketAddr::Family::IPv6) ? AF_INET6 : AF_INET;
    int fd = ::socket(domain, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return unexpected(std::error_code(errno, std::system_category()));

    // SO_REUSEPORT: 멀티 워커 포트 공유 허용
    {
      int opt = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

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

    return UdpSocket(fd);
  }

  // ─── 비동기 I/O ─────────────────────────────────────────────────────────

  /**
   * @brief 지정된 목적지 주소로 데이터그램을 비동기로 전송합니다.
   *
   * 소켓이 쓰기 가능해질 때까지 Reactor 이벤트를 대기합니다.
   *
   * @param buf  전송할 데이터 버퍼.
   * @param dest 목적지 소켓 주소.
   * @returns 전송한 바이트 수. 에러 시 error_code.
   */
  Task<Result<size_t>> send_to(std::span<const std::byte> buf,
                               SocketAddr dest) {
    sockaddr_storage ss{};
    socklen_t len{};
    auto r = dest.to_sockaddr(ss, len);
    if (!r) {
      co_return unexpected(r.error());
    }

    struct SendToAwaiter {
      int fd_;
      const std::byte *data_;
      size_t size_;
      const sockaddr_storage *ss_;
      socklen_t sslen_;
      ssize_t result_ = -1;
      int err_ = 0;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) { handle.resume(); return; }
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
      co_return unexpected(std::error_code(aw.err_, std::system_category()));
    }
    co_return static_cast<size_t>(aw.result_);
  }

  /**
   * @brief 데이터그램을 비동기로 수신하고 발신자 주소를 함께 반환합니다.
   *
   * 소켓이 읽기 가능해질 때까지 Reactor 이벤트를 대기합니다.
   *
   * @param buf 수신 데이터를 저장할 버퍼.
   * @returns 수신한 바이트 수와 발신자 SocketAddr 쌍. 에러 시 error_code.
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

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) { handle.resume(); return; }
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
      co_return unexpected(std::error_code(aw.err_, std::system_category()));
    }

    // sockaddr_storage → SocketAddr 변환
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

  // ─── 접근자 ─────────────────────────────────────────────────────────────

  /**
   * @brief 내부 파일 디스크립터를 반환합니다.
   * @returns 소켓 fd. 유효하지 않으면 -1.
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief 관리 중인 UDP 소켓 파일 디스크립터. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
