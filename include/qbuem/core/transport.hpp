#pragma once

/**
 * @file qbuem/core/transport.hpp
 * @brief TLS 계층 주입점 — ITransport 추상 인터페이스
 * @defgroup qbuem_transport Transport
 * @ingroup qbuem_core
 *
 * qbuem-stack은 TLS를 직접 구현하지 않습니다.
 * OpenSSL, mbedTLS, BoringSSL 등의 구현체는 서비스에서 이 인터페이스를 구현해
 * `Connection`에 주입합니다.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace qbuem {

/**
 * @brief 양방향 스트림 전송 계층 추상 인터페이스.
 *
 * 기본 TCP 연결 또는 TLS 래퍼 모두 이 인터페이스로 표현됩니다.
 * 모든 I/O 메서드는 논블로킹 코루틴으로, Reactor 스레드에서
 * co_await 없이 대기합니다.
 *
 * ### 수명 규칙
 * - `close()`를 호출한 뒤에는 `read()`/`write()` 호출 금지.
 * - 소멸자 내에서 `close()`를 보장할 필요는 없음 — 명시적 호출 권장.
 *
 * ### 구현 가이드
 * - `read()`: EAGAIN/EWOULDBLOCK 시 Reactor에 읽기 이벤트 등록 후 co_await.
 * - `write()`: 송신 버퍼 포화 시 Reactor에 쓰기 이벤트 등록 후 co_await.
 * - TLS: `handshake()` 완료 전 `read()`/`write()` 호출은 에러 반환.
 */
class ITransport {
public:
  virtual ~ITransport() = default;

  /**
   * @brief 데이터를 읽어 `buf`에 씁니다.
   *
   * @param buf 수신 버퍼. 크기가 0이면 즉시 0 반환.
   * @returns 읽은 바이트 수. EOS(EOF/연결 종료)면 0.
   *          에러 시 `errc::*` 코드.
   */
  virtual Task<Result<size_t>> read(std::span<std::byte> buf) = 0;

  /**
   * @brief `buf`의 데이터를 모두 전송합니다.
   *
   * @param buf 송신 버퍼. 비어 있으면 즉시 0 반환.
   * @returns 전송한 바이트 수.
   */
  virtual Task<Result<size_t>> write(std::span<const std::byte> buf) = 0;

  /**
   * @brief TLS 핸드셰이크를 수행합니다.
   *
   * 일반 TCP 구현체에서는 no-op으로 즉시 `ok()`를 반환합니다.
   * TLS 구현체에서는 핸드셰이크가 완료되어야 `read()`/`write()` 가능.
   *
   * @returns 성공 시 `ok()`. 실패 시 TLS 알림 코드.
   */
  virtual Task<Result<void>> handshake() = 0;

  /**
   * @brief 전송 계층을 닫습니다.
   *
   * TLS 구현체는 `close_notify` 알림을 전송합니다.
   * TCP 구현체는 소켓을 `shutdown(SHUT_WR)` 후 닫습니다.
   *
   * 이미 닫힌 상태에서 호출 시 no-op.
   */
  virtual Task<Result<void>> close() = 0;

  /**
   * @brief ALPN으로 협상된 프로토콜 이름을 반환합니다.
   *
   * TLS 핸드셰이크 성공 후에만 유효합니다.
   *
   * @returns `"h2"` (HTTP/2), `"http/1.1"` (HTTP/1.1), 또는 `""` (협상 없음).
   */
  virtual std::string_view negotiated_protocol() const noexcept { return ""; }

  /**
   * @brief 피어의 X.509 인증서 핑거프린트를 반환합니다 (선택적).
   *
   * mTLS 환경에서 클라이언트 인증에 사용합니다.
   * 지원하지 않으면 빈 문자열 반환.
   */
  virtual std::string_view peer_certificate_fingerprint() const noexcept {
    return "";
  }
};

} // namespace qbuem

/** @} */
