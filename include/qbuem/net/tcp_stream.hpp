#pragma once

/**
 * @file qbuem/net/tcp_stream.hpp
 * @brief 비동기 TCP 스트림 연결 타입.
 * @ingroup qbuem_net
 *
 * `TcpStream`은 논블로킹 TCP 소켓 파일 디스크립터를 RAII로 관리하며,
 * 코루틴 기반의 비동기 읽기/쓰기 인터페이스를 제공합니다.
 *
 * ### 설계 원칙
 * - Move-only: 복사 불가, 소멸자에서 fd 자동 close
 * - 모든 I/O는 `Reactor::current()`를 통해 비동기로 수행
 * - scatter-gather I/O (`readv`/`writev`) 지원
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>

#include <cerrno>
#include <coroutine>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <span>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace qbuem {

/**
 * @brief 비동기 TCP 연결 스트림.
 *
 * 논블로킹 TCP 소켓을 래핑하여 코루틴 기반 읽기/쓰기 인터페이스를 제공합니다.
 * Move-only 타입이며 소멸 시 소켓이 자동으로 닫힙니다.
 *
 * ### 사용 예시
 * @code
 * auto addr = SocketAddr::from_ipv4("127.0.0.1", 8080);
 * auto stream = co_await TcpStream::connect(*addr);
 * if (!stream) { // 에러 처리 }
 *
 * std::array<std::byte, 1024> buf{};
 * auto n = co_await stream->read(buf);
 * @endcode
 */
class TcpStream {
public:
  /** @brief 유효하지 않은 fd로 초기화. */
  TcpStream() noexcept : fd_(-1) {}

  /**
   * @brief 이미 연결된 fd로 TcpStream을 구성합니다.
   * @param fd 논블로킹 TCP 소켓 파일 디스크립터.
   */
  explicit TcpStream(int fd) noexcept : fd_(fd) {}

  /** @brief 소멸자: 열린 소켓을 자동으로 닫습니다. */
  ~TcpStream() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @brief 복사 생성자 삭제 (Move-only). */
  TcpStream(const TcpStream &) = delete;

  /** @brief 복사 대입 삭제 (Move-only). */
  TcpStream &operator=(const TcpStream &) = delete;

  /** @brief 이동 생성자. */
  TcpStream(TcpStream &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief 이동 대입 연산자. */
  TcpStream &operator=(TcpStream &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── 비동기 연결 ────────────────────────────────────────────────────────

  /**
   * @brief 지정된 주소로 비동기 TCP 연결을 수행합니다.
   *
   * 논블로킹 `connect(2)`를 호출하고 EINPROGRESS 시 Reactor에 쓰기 이벤트를
   * 등록하여 연결 완료를 대기합니다.
   *
   * @param addr 연결할 원격 소켓 주소.
   * @returns 연결 성공 시 TcpStream, 실패 시 에러 코드.
   */
  static Task<Result<TcpStream>> connect(SocketAddr addr) {
    int fd = ::socket(
        addr.family() == SocketAddr::Family::IPv6 ? AF_INET6 : AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }

    sockaddr_storage ss{};
    socklen_t len{};
    auto r = addr.to_sockaddr(ss, len);
    if (!r) {
      ::close(fd);
      co_return unexpected(r.error());
    }

    int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&ss), len);
    if (rc == 0) {
      co_return TcpStream(fd);
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
        if (!reactor) {
          handle.resume();
          return;
        }
        reactor->register_event(fd_, EventType::Write, [handle, this](int f) {
          int so_err = 0;
          socklen_t len = sizeof(so_err);
          ::getsockopt(f, SOL_SOCKET, SO_ERROR, &so_err, &len);
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
    co_return TcpStream(fd);
  }

  // ─── 비동기 I/O ─────────────────────────────────────────────────────────

  /**
   * @brief 버퍼에 데이터를 비동기로 읽습니다.
   *
   * 소켓이 읽기 가능해질 때까지 Reactor 이벤트를 대기합니다.
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
   * 소켓이 쓰기 가능해질 때까지 Reactor 이벤트를 대기합니다.
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

  /**
   * @brief 다중 버퍼(scatter) 비동기 읽기 (readv).
   *
   * 소켓이 읽기 가능해질 때까지 Reactor 이벤트를 대기한 뒤 `readv(2)`를 호출합니다.
   *
   * @param bufs iovec 배열. 각 원소가 독립적인 버퍼를 가리킵니다.
   * @returns 읽은 총 바이트 수. EOF면 0. 에러 시 error_code.
   */
  Task<Result<size_t>> readv(std::span<iovec> bufs) {
    struct ReadvAwaiter {
      int fd_;
      iovec *iov_;
      int iovcnt_;
      ssize_t result_ = -1;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) { handle.resume(); return; }
        reactor->register_event(fd_, EventType::Read, [handle, this](int f) {
          result_ = ::readv(f, iov_, iovcnt_);
          Reactor::current()->unregister_event(f, EventType::Read);
          handle.resume();
        });
      }

      ssize_t await_resume() const noexcept { return result_; }
    };

    ssize_t n = co_await ReadvAwaiter{fd_, bufs.data(),
                                      static_cast<int>(bufs.size())};
    if (n < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief 다중 버퍼(gather) 비동기 쓰기 (writev).
   *
   * 소켓이 쓰기 가능해질 때까지 Reactor 이벤트를 대기한 뒤 `writev(2)`를 호출합니다.
   *
   * @param bufs const iovec 배열. 각 원소가 전송할 버퍼를 가리킵니다.
   * @returns 전송한 총 바이트 수. 에러 시 error_code.
   */
  Task<Result<size_t>> writev(std::span<const iovec> bufs) {
    struct WritevAwaiter {
      int fd_;
      const iovec *iov_;
      int iovcnt_;
      ssize_t result_ = -1;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) { handle.resume(); return; }
        reactor->register_event(fd_, EventType::Write, [handle, this](int f) {
          result_ = ::writev(f, iov_, iovcnt_);
          Reactor::current()->unregister_event(f, EventType::Write);
          handle.resume();
        });
      }

      ssize_t await_resume() const noexcept { return result_; }
    };

    ssize_t n = co_await WritevAwaiter{fd_, bufs.data(),
                                       static_cast<int>(bufs.size())};
    if (n < 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  // ─── 소켓 옵션 ──────────────────────────────────────────────────────────

  /**
   * @brief TCP_NODELAY 옵션을 설정합니다 (Nagle 알고리즘 비활성화).
   *
   * 지연 없이 소규모 패킷을 즉시 전송해야 하는 저지연 애플리케이션에 유용합니다.
   *
   * @param v true이면 TCP_NODELAY 활성화, false이면 비활성화.
   */
  void set_nodelay(bool v) noexcept {
    int flag = v ? 1 : 0;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
  }

  /**
   * @brief SO_KEEPALIVE 옵션을 설정합니다 (TCP keepalive 활성화).
   *
   * 유휴 연결의 생존 여부를 주기적으로 확인합니다.
   *
   * @param v true이면 keepalive 활성화, false이면 비활성화.
   */
  void set_keepalive(bool v) noexcept {
    int flag = v ? 1 : 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  }

  // ─── 접근자 ─────────────────────────────────────────────────────────────

  /**
   * @brief 내부 파일 디스크립터를 반환합니다.
   * @returns 소켓 fd. 유효하지 않으면 -1.
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief 관리 중인 TCP 소켓 파일 디스크립터. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
