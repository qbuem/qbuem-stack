#pragma once

/**
 * @file qbuem/transport/plain_transport.hpp
 * @brief 평문 TCP 연결을 위한 ITransport 구체 구현.
 * @defgroup qbuem_transport_impl Transport Implementations
 * @ingroup qbuem_transport
 *
 * `PlainTransport`는 TLS 없이 일반 TCP fd를 사용하는
 * `ITransport` 구체 구현체입니다.
 *
 * ### 설계 원칙
 * - `ITransport` 인터페이스를 완전히 구현
 * - `AsyncRead` / `AsyncWrite` awaiter를 통한 Reactor 기반 비동기 I/O
 * - `handshake()`: 평문 TCP이므로 즉시 ok() 반환 (no-op)
 * - `close()`: `shutdown(SHUT_WR)` 후 fd 닫기
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
 * @brief 평문 TCP 연결을 위한 ITransport 구체 구현.
 *
 * 논블로킹 TCP 소켓 fd를 래핑하고 `ITransport` 인터페이스를 구현합니다.
 * TLS 처리 없이 소켓에 직접 읽기/쓰기를 수행합니다.
 *
 * ### 일반적인 사용 패턴
 * @code
 * // TcpListener::accept()로 얻은 fd를 PlainTransport에 주입
 * auto transport = std::make_unique<PlainTransport>(client_fd);
 * // Connection 등에 주입하여 사용
 * @endcode
 *
 * @note 소멸자는 fd를 닫지 않습니다. 명시적으로 `close()`를 호출하거나
 *       fd 소유권이 있는 TcpStream이 소멸할 때 닫힙니다.
 *       fd 수명을 외부에서 관리하는 경우에 이 클래스를 사용하세요.
 */
class PlainTransport : public ITransport {
public:
  /**
   * @brief 지정된 fd로 PlainTransport를 구성합니다.
   *
   * @param fd 논블로킹 TCP 소켓 파일 디스크립터.
   *           반드시 `O_NONBLOCK`이 설정된 상태여야 합니다.
   */
  explicit PlainTransport(int fd) noexcept : fd_(fd) {}

  /**
   * @brief 소멸자.
   *
   * fd를 닫지 않습니다. fd의 수명은 호출자가 관리합니다.
   * fd를 소유해야 한다면 `close()`를 명시적으로 호출하세요.
   */
  ~PlainTransport() override = default;

  // ─── ITransport 구현 ────────────────────────────────────────────────────

  /**
   * @brief 소켓에서 데이터를 비동기로 읽습니다.
   *
   * `AsyncRead` awaiter를 통해 소켓이 읽기 가능해질 때까지 Reactor에 등록합니다.
   *
   * @param buf 수신 데이터를 저장할 버퍼. 비어 있으면 즉시 0 반환.
   * @returns 읽은 바이트 수. EOF/연결 종료면 0. 에러 시 error_code.
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
   * @brief 소켓으로 데이터를 비동기로 씁니다.
   *
   * `AsyncWrite` awaiter를 통해 소켓이 쓰기 가능해질 때까지 Reactor에 등록합니다.
   *
   * @param buf 전송할 데이터 버퍼. 비어 있으면 즉시 0 반환.
   * @returns 전송한 바이트 수. 에러 시 error_code.
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
   * @brief TLS 핸드셰이크 (평문 TCP이므로 no-op).
   *
   * 평문 TCP 연결에서는 핸드셰이크가 필요 없으므로 즉시 ok()를 반환합니다.
   *
   * @returns 항상 `Result<void>::ok()`.
   */
  Task<Result<void>> handshake() override {
    co_return Result<void>::ok();
  }

  /**
   * @brief 전송 계층을 닫습니다.
   *
   * `shutdown(SHUT_WR)`으로 쓰기를 종료한 뒤 fd를 닫습니다.
   * 이미 닫힌 상태에서 호출 시 no-op입니다.
   *
   * @returns 성공 시 `Result<void>::ok()`, 실패 시 에러 코드.
   */
  Task<Result<void>> close() override {
    if (fd_ < 0) {
      co_return Result<void>::ok();
    }
    int fd = fd_;
    fd_ = -1;
    // 쓰기 종료로 FIN 전송 (수신은 계속 가능)
    ::shutdown(fd, SHUT_WR);
    if (::close(fd) != 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return Result<void>::ok();
  }

  /**
   * @brief ALPN 협상 프로토콜 반환 (평문 TCP이므로 빈 문자열).
   * @returns 항상 `""`.
   */
  std::string_view negotiated_protocol() const noexcept override {
    return "";
  }

  // ─── 접근자 ─────────────────────────────────────────────────────────────

  /**
   * @brief 내부 파일 디스크립터를 반환합니다.
   * @returns 소켓 fd. close() 후에는 -1.
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief 관리 중인 TCP 소켓 파일 디스크립터. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_transport_impl
