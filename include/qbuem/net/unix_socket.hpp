#pragma once

/**
 * @file qbuem/net/unix_socket.hpp
 * @brief 비동기 AF_UNIX 도메인 소켓 타입.
 * @ingroup qbuem_net
 *
 * `UnixSocket`은 AF_UNIX 스트림 소켓을 RAII로 관리하며,
 * 코루틴 기반의 비동기 bind/connect/accept/read/write를 제공합니다.
 *
 * ### 설계 원칙
 * - Move-only: 복사 불가, 소멸자에서 fd 자동 close
 * - `bind()`: AF_UNIX SOCK_STREAM 서버 소켓 (bind + listen)
 * - `connect()`: 비동기 클라이언트 연결
 * - `accept()`: 비동기 클라이언트 수락, UnixSocket으로 반환
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <cerrno>
#include <coroutine>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace qbuem {

/**
 * @brief 비동기 AF_UNIX 도메인 소켓.
 *
 * AF_UNIX SOCK_STREAM 소켓을 래핑하여 IPC 통신을 위한
 * 코루틴 기반 비동기 인터페이스를 제공합니다.
 * Move-only 타입이며 소멸 시 소켓이 자동으로 닫힙니다.
 *
 * ### 사용 예시 (서버)
 * @code
 * auto server = UnixSocket::bind("/tmp/my.sock");
 * if (!server) { // 에러 처리 }
 *
 * auto client = co_await server->accept();
 * @endcode
 *
 * ### 사용 예시 (클라이언트)
 * @code
 * auto conn = co_await UnixSocket::connect("/tmp/my.sock");
 * if (!conn) { // 에러 처리 }
 * @endcode
 */
class UnixSocket {
public:
  /** @brief 유효하지 않은 fd로 초기화. */
  UnixSocket() noexcept : fd_(-1) {}

  /**
   * @brief 기존 fd로 UnixSocket을 구성합니다.
   * @param fd 논블로킹 AF_UNIX 소켓 파일 디스크립터.
   */
  explicit UnixSocket(int fd) noexcept : fd_(fd) {}

  /** @brief 소멸자: 열린 소켓을 자동으로 닫습니다. */
  ~UnixSocket() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @brief 복사 생성자 삭제 (Move-only). */
  UnixSocket(const UnixSocket &) = delete;

  /** @brief 복사 대입 삭제 (Move-only). */
  UnixSocket &operator=(const UnixSocket &) = delete;

  /** @brief 이동 생성자. */
  UnixSocket(UnixSocket &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief 이동 대입 연산자. */
  UnixSocket &operator=(UnixSocket &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── 팩토리 메서드 ──────────────────────────────────────────────────────

  /**
   * @brief 지정된 Unix 도메인 경로에 바인딩하고 수신 대기합니다.
   *
   * 소켓 파일이 이미 존재하면 `unlink(2)`로 먼저 삭제한 뒤 새로 생성합니다.
   *
   * @param path Unix 도메인 소켓 파일 경로. 107바이트 이하여야 합니다.
   * @returns 성공 시 UnixSocket(리슨 소켓), 실패 시 에러 코드.
   */
  static Result<UnixSocket> bind(const char *path) noexcept {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return unexpected(std::error_code(errno, std::system_category()));

    // 기존 소켓 파일 제거 (무시 가능)
    ::unlink(path);

    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    ::strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    socklen_t len = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + ::strlen(path) + 1);

    if (::bind(fd, reinterpret_cast<const sockaddr *>(&sa), len) != 0) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      return unexpected(ec);
    }

    if (::listen(fd, SOMAXCONN) != 0) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      return unexpected(ec);
    }

    return UnixSocket(fd);
  }

  /**
   * @brief 지정된 Unix 도메인 경로로 비동기 연결을 수행합니다.
   *
   * 논블로킹 `connect(2)` 호출 후 EINPROGRESS 시 Reactor 쓰기 이벤트를 대기합니다.
   *
   * @param path 연결할 서버 소켓 파일 경로.
   * @returns 성공 시 연결된 UnixSocket, 실패 시 에러 코드.
   */
  static Task<Result<UnixSocket>> connect(const char *path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }

    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    ::strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    socklen_t len = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + ::strlen(path) + 1);

    int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&sa), len);
    if (rc == 0) {
      co_return UnixSocket(fd);
    }
    if (errno != EINPROGRESS) {
      auto ec = std::error_code(errno, std::system_category());
      ::close(fd);
      co_return unexpected(ec);
    }

    // EINPROGRESS: Reactor 쓰기 이벤트 대기
    struct ConnectAwaiter {
      int fd_;
      int err_ = 0;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) { handle.resume(); return; }
        reactor->register_event(fd_, EventType::Write, [handle, this](int f) {
          int so_err = 0;
          socklen_t slen = sizeof(so_err);
          ::getsockopt(f, SOL_SOCKET, SO_ERROR, &so_err, &slen);
          err_ = so_err;
          Reactor::current()->unregister_event(f, EventType::Write);
          handle.resume();
        });
      }

      int await_resume() const noexcept { return err_; }
    };

    int so_err = co_await ConnectAwaiter{fd};
    if (so_err != 0) {
      ::close(fd);
      co_return unexpected(std::error_code(so_err, std::system_category()));
    }
    co_return UnixSocket(fd);
  }

  // ─── 비동기 accept ──────────────────────────────────────────────────────

  /**
   * @brief 클라이언트 연결을 비동기로 수락합니다.
   *
   * 리슨 소켓이 읽기 가능해질 때까지 Reactor 이벤트를 대기한 뒤 `accept(2)`를 호출합니다.
   *
   * @returns 성공 시 클라이언트 UnixSocket, 실패 시 에러 코드.
   */
  Task<Result<UnixSocket>> accept() {
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
              sockaddr_un addr{};
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
    co_return UnixSocket(aw.client_fd_);
  }

  // ─── 비동기 I/O ─────────────────────────────────────────────────────────

  /**
   * @brief 버퍼에 데이터를 비동기로 읽습니다.
   *
   * @param buf 수신 데이터를 저장할 버퍼.
   * @returns 읽은 바이트 수. EOF면 0. 에러 시 error_code.
   */
  Task<Result<size_t>> read(std::span<std::byte> buf) {
    ssize_t n = co_await AsyncRead{fd_, buf.data(), buf.size()};
    if (n < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief 버퍼의 데이터를 비동기로 씁니다.
   *
   * @param buf 전송할 데이터 버퍼.
   * @returns 전송한 바이트 수. 에러 시 error_code.
   */
  Task<Result<size_t>> write(std::span<const std::byte> buf) {
    ssize_t n = co_await AsyncWrite{fd_, buf.data(), buf.size()};
    if (n < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  // ─── 접근자 ─────────────────────────────────────────────────────────────

  /**
   * @brief 내부 파일 디스크립터를 반환합니다.
   * @returns 소켓 fd. 유효하지 않으면 -1.
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief 관리 중인 AF_UNIX 소켓 파일 디스크립터. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
