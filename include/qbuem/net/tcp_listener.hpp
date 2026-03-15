#pragma once

/**
 * @file qbuem/net/tcp_listener.hpp
 * @brief 비동기 TCP 수신 대기 소켓 타입.
 * @ingroup qbuem_net
 *
 * `TcpListener`는 SO_REUSEPORT 리슨 소켓을 RAII로 관리하며,
 * 코루틴 기반의 비동기 accept 인터페이스를 제공합니다.
 *
 * ### 설계 원칙
 * - Move-only: 복사 불가, 소멸자에서 fd 자동 close
 * - SO_REUSEPORT 기본 활성화 (멀티 워커 포트 공유)
 * - 가용 시 TCP_FASTOPEN, TCP_DEFER_ACCEPT(Linux 한정) 자동 적용
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
 * @brief 비동기 TCP 수신 대기 소켓.
 *
 * `bind()`로 포트에 바인딩한 후 `accept()`를 반복 호출하여
 * 클라이언트 연결을 수락합니다. 각 수락된 연결은 TcpStream으로 반환됩니다.
 *
 * ### 사용 예시
 * @code
 * auto addr = SocketAddr::from_ipv4("0.0.0.0", 8080);
 * auto listener = TcpListener::bind(*addr);
 * if (!listener) { // 에러 처리 }
 *
 * while (true) {
 *     auto stream = co_await listener->accept();
 *     if (!stream) break;
 *     // stream 처리
 * }
 * @endcode
 */
class TcpListener {
public:
  /** @brief 유효하지 않은 fd로 초기화. */
  TcpListener() noexcept : fd_(-1) {}

  /**
   * @brief 이미 리슨 중인 fd로 TcpListener를 구성합니다.
   * @param fd 논블로킹 리슨 소켓 파일 디스크립터.
   */
  explicit TcpListener(int fd) noexcept : fd_(fd) {}

  /** @brief 소멸자: 리슨 소켓을 자동으로 닫습니다. */
  ~TcpListener() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @brief 복사 생성자 삭제 (Move-only). */
  TcpListener(const TcpListener &) = delete;

  /** @brief 복사 대입 삭제 (Move-only). */
  TcpListener &operator=(const TcpListener &) = delete;

  /** @brief 이동 생성자. */
  TcpListener(TcpListener &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief 이동 대입 연산자. */
  TcpListener &operator=(TcpListener &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── 팩토리 메서드 ──────────────────────────────────────────────────────

  /**
   * @brief 지정된 주소에 바인딩하고 수신 대기 소켓을 생성합니다.
   *
   * 다음 옵션을 자동으로 적용합니다:
   * - SO_REUSEPORT: 동일 포트를 여러 워커가 공유 가능
   * - TCP_FASTOPEN: 가용 시 TFO 활성화 (큐 크기 128)
   * - TCP_DEFER_ACCEPT: Linux에서 데이터가 올 때까지 accept 지연
   *
   * @param addr 바인딩할 로컬 주소.
   * @returns 성공 시 TcpListener, 실패 시 에러 코드.
   */
  static Result<TcpListener> bind(SocketAddr addr) noexcept {
    int domain = (addr.family() == SocketAddr::Family::IPv6) ? AF_INET6 : AF_INET;
    int fd = ::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return unexpected(std::error_code(errno, std::system_category()));

    // SO_REUSEPORT: 멀티 워커 포트 공유
    {
      int opt = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    // SO_REUSEADDR: TIME_WAIT 상태 주소 재사용
    {
      int opt = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

#ifdef TCP_FASTOPEN
    // TCP_FASTOPEN: 첫 번째 SYN에서 데이터 전송 (가용 시)
    {
      int qlen = 128;
      ::setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
    }
#endif

#ifdef TCP_DEFER_ACCEPT
    // TCP_DEFER_ACCEPT: Linux에서 데이터가 도착해야 accept 반환
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

  // ─── 비동기 accept ──────────────────────────────────────────────────────

  /**
   * @brief 클라이언트 연결을 비동기로 수락합니다.
   *
   * 리슨 소켓이 읽기 가능해질 때까지 Reactor 이벤트를 대기한 뒤
   * `accept4(2)`로 클라이언트 소켓을 수락합니다.
   * 반환된 TcpStream의 소켓은 SOCK_NONBLOCK | SOCK_CLOEXEC로 설정됩니다.
   *
   * @returns 성공 시 클라이언트 TcpStream, 실패 시 에러 코드.
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

  // ─── 접근자 ─────────────────────────────────────────────────────────────

  /**
   * @brief 내부 파일 디스크립터를 반환합니다.
   * @returns 리슨 소켓 fd. 유효하지 않으면 -1.
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief 관리 중인 리슨 소켓 파일 디스크립터. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
