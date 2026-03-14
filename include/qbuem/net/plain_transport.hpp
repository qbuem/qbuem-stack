#pragma once

/**
 * @file qbuem/net/plain_transport.hpp
 * @brief TLS 없는 일반 TCP 전송 계층 구현.
 * @ingroup qbuem_net
 *
 * `PlainTransport`는 `ITransport` 인터페이스의 Plain TCP 구현체입니다.
 * `TcpStream`을 래핑하여 TLS 없이 원시 TCP 소켓으로 통신합니다.
 *
 * ### 설계 원칙
 * - `ITransport` 가상 메서드를 `TcpStream`에 그대로 위임 (zero-overhead delegation)
 * - `handshake()`는 즉시 성공을 반환 (TLS 없음)
 * - `close()`는 TCP `shutdown(SHUT_WR)` 후 소켓 닫기
 * - Move-only: `TcpStream`의 소유권을 전달받아 관리
 *
 * ### 사용 예시
 * @code
 * auto stream = co_await TcpStream::connect(addr);
 * if (!stream) { // 에러 처리 }
 *
 * auto transport = std::make_unique<PlainTransport>(std::move(*stream));
 * // Connection 등에 주입
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
 * @brief TLS 없는 Plain TCP `ITransport` 구현체.
 *
 * `TcpStream`을 소유하며 `ITransport`의 모든 가상 메서드를
 * `TcpStream`에 위임합니다.
 *
 * ### handshake()
 * TLS가 없으므로 즉시 `Result<void>::ok()`를 반환합니다.
 *
 * ### close()
 * `shutdown(SHUT_WR)`로 쓰기를 종료한 뒤 소켓을 닫습니다.
 * 이미 닫힌 상태에서 재호출 시 no-op입니다.
 */
class PlainTransport final : public ITransport {
public:
  /**
   * @brief `TcpStream`을 소유권 이전으로 받아 PlainTransport를 구성합니다.
   *
   * @param stream 소유권을 이전할 `TcpStream`. 이동됩니다.
   */
  explicit PlainTransport(TcpStream stream) noexcept
      : stream_(std::move(stream)) {}

  /** @brief 소멸자: `TcpStream` RAII에 의해 소켓이 자동으로 닫힙니다. */
  ~PlainTransport() override = default;

  /** @brief 복사 생성자 삭제 (Move-only). */
  PlainTransport(const PlainTransport &) = delete;

  /** @brief 복사 대입 삭제 (Move-only). */
  PlainTransport &operator=(const PlainTransport &) = delete;

  /** @brief 이동 생성자. */
  PlainTransport(PlainTransport &&) noexcept = default;

  /** @brief 이동 대입 연산자. */
  PlainTransport &operator=(PlainTransport &&) noexcept = default;

  // ─── ITransport 구현 ───────────────────────────────────────────────────

  /**
   * @brief `TcpStream::read()`에 위임하여 데이터를 읽습니다.
   *
   * @param buf 수신 버퍼.
   * @returns 읽은 바이트 수. EOF면 0. 에러 시 error_code.
   */
  Task<Result<size_t>> read(std::span<std::byte> buf) override {
    co_return co_await stream_.read(buf);
  }

  /**
   * @brief `TcpStream::write()`에 위임하여 데이터를 씁니다.
   *
   * @param buf 송신 버퍼.
   * @returns 전송한 바이트 수. 에러 시 error_code.
   */
  Task<Result<size_t>> write(std::span<const std::byte> buf) override {
    co_return co_await stream_.write(buf);
  }

  /**
   * @brief Plain TCP에서는 핸드셰이크가 없으므로 즉시 성공을 반환합니다.
   *
   * @returns 항상 `Result<void>::ok()`.
   */
  Task<Result<void>> handshake() override {
    co_return Result<void>::ok();
  }

  /**
   * @brief `shutdown(SHUT_WR)` 후 소켓을 닫습니다.
   *
   * 이미 닫힌 상태(`fd() < 0`)에서 재호출 시 즉시 ok를 반환합니다.
   *
   * @returns 성공 시 `Result<void>::ok()`. 실패 시 에러 코드.
   */
  Task<Result<void>> close() override {
    int fd = stream_.fd();
    if (fd < 0) {
      co_return Result<void>::ok();
    }
    ::shutdown(fd, SHUT_WR);
    // TcpStream 소멸자가 close(fd)를 호출하도록 이동으로 소멸
    TcpStream tmp = std::move(stream_);
    (void)tmp; // 소멸자에서 ::close(fd) 호출
    co_return Result<void>::ok();
  }

  /**
   * @brief `""`를 반환합니다. PlainTransport는 ALPN 협상을 수행하지 않습니다.
   *
   * @returns 빈 문자열.
   */
  std::string_view negotiated_protocol() const noexcept override {
    return "";
  }

  /**
   * @brief `""`를 반환합니다. PlainTransport에는 인증서가 없습니다.
   *
   * @returns 빈 문자열.
   */
  std::string_view peer_certificate_fingerprint() const noexcept override {
    return "";
  }

  // ─── 확장 접근자 ────────────────────────────────────────────────────────

  /**
   * @brief 내부 `TcpStream`에 대한 참조를 반환합니다.
   *
   * TCP_NODELAY 설정 등 소켓 옵션 조정에 사용합니다.
   *
   * @returns `TcpStream` lvalue 참조.
   */
  TcpStream &stream() noexcept { return stream_; }

  /**
   * @brief 내부 `TcpStream`에 대한 const 참조를 반환합니다.
   *
   * @returns `TcpStream` const lvalue 참조.
   */
  const TcpStream &stream() const noexcept { return stream_; }

  /**
   * @brief 내부 파일 디스크립터를 반환합니다.
   * @returns 소켓 fd. 유효하지 않으면 -1.
   */
  [[nodiscard]] int fd() const noexcept { return stream_.fd(); }

private:
  /** @brief 소유 중인 TCP 스트림. */
  TcpStream stream_;
};

} // namespace qbuem

/** @} */ // end of qbuem_net
