#pragma once

/**
 * @file qbuem/io/ktls.hpp
 * @brief Kernel TLS (kTLS) 통합 — `setsockopt(SOL_TLS, TLS_TX/RX)` 지원
 * @defgroup qbuem_ktls Kernel TLS
 * @ingroup qbuem_io
 *
 * kTLS는 TLS 암호화를 커널 공간에서 처리하여 user-space TLS 구현 대비
 * 시스템 콜 횟수와 메모리 복사를 줄입니다.
 *
 * ## 전제 조건
 * - Linux 4.13+ (TLS_TX), Linux 4.17+ (TLS_RX)
 * - `CONFIG_TLS` 커널 빌드 옵션 활성화
 * - 핸드셰이크는 OpenSSL / BoringSSL 등 user-space TLS 라이브러리로 완료한 후
 *   세션 키를 커널에 전달
 *
 * ## 사용 흐름
 * ```
 * 1. TCP 소켓 생성 + TLS 핸드셰이크 (user-space)
 * 2. 세션 키 추출 (tx_key, rx_key, iv, seq)
 * 3. ktls::enable_tx(fd, params) — 송신 암호화를 커널로 이전
 * 4. ktls::enable_rx(fd, params) — 수신 복호화를 커널로 이전
 * 5. 이후 send()/recv() → 커널이 직접 암호화/복호화
 * ```
 *
 * ## sendfile + kTLS 조합
 * kTLS가 활성화된 소켓에서 `sendfile(2)`를 사용하면
 * 정적 파일을 TLS 암호화하면서도 zero-copy로 전송할 수 있습니다.
 *
 * ## 플랫폼 지원
 * - Linux: `SOL_TLS`, `TLS_TX`, `TLS_RX` (linux/tls.h)
 * - 비Linux: 모든 함수가 `errc::not_supported` 반환
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__linux__)
#  include <sys/socket.h>
#  include <netinet/tcp.h>
#  if __has_include(<linux/tls.h>)
#    include <linux/tls.h>
#    define QBUEM_HAS_KTLS 1
#  endif
#endif

namespace qbuem::io {

// ---------------------------------------------------------------------------
// TLS 세션 파라미터
// ---------------------------------------------------------------------------

/**
 * @brief TLS 1.3 AES-128-GCM 세션 파라미터.
 *
 * TLS 핸드셰이크 완료 후 user-space TLS 라이브러리에서 추출한
 * 세션 키 재료를 담습니다. 커널 kTLS 설정에 직접 전달됩니다.
 */
struct KtlsSessionParams {
  std::array<uint8_t, 16> key;     ///< AES-128 암호화 키
  std::array<uint8_t, 12> iv;      ///< IV (implicit nonce, 4바이트 salt + 8바이트 explicit nonce)
  std::array<uint8_t, 8>  seq;     ///< TLS 레코드 시퀀스 번호 (big-endian)
};

/**
 * @brief TLS 1.3 AES-256-GCM 세션 파라미터.
 */
struct KtlsSessionParams256 {
  std::array<uint8_t, 32> key;     ///< AES-256 암호화 키
  std::array<uint8_t, 12> iv;      ///< IV
  std::array<uint8_t, 8>  seq;     ///< TLS 레코드 시퀀스 번호
};

// ---------------------------------------------------------------------------
// kTLS 활성화 함수
// ---------------------------------------------------------------------------

/**
 * @brief 소켓의 송신 경로에 kTLS를 활성화합니다 (TLS 1.3 AES-128-GCM).
 *
 * `setsockopt(fd, SOL_TLS, TLS_TX, ...)` 호출로 이후 `send()/write()`가
 * 커널 내에서 TLS 암호화를 수행합니다.
 *
 * @param sockfd  TLS 핸드셰이크가 완료된 TCP 소켓 fd.
 * @param params  TLS 세션 파라미터 (키, IV, 시퀀스 번호).
 * @returns 성공 시 `Result<void>`, 실패 시 오류 코드.
 */
[[nodiscard]] inline Result<void> enable_tx(
    int sockfd, const KtlsSessionParams &params) noexcept {
#if defined(QBUEM_HAS_KTLS) && defined(TLS_TX)
  struct tls12_crypto_info_aes_gcm_128 crypto_info{};
  crypto_info.info.version    = TLS_1_3_VERSION;
  crypto_info.info.cipher_type = TLS_CIPHER_AES_GCM_128;
  std::memcpy(crypto_info.key, params.key.data(), sizeof(crypto_info.key));
  std::memcpy(crypto_info.iv,  params.iv.data() + 4,
              sizeof(crypto_info.iv));  // explicit nonce (8바이트)
  std::memcpy(crypto_info.salt, params.iv.data(),
              sizeof(crypto_info.salt)); // implicit salt (4바이트)
  std::memcpy(crypto_info.rec_seq, params.seq.data(),
              sizeof(crypto_info.rec_seq));

  if (::setsockopt(sockfd, SOL_TLS, TLS_TX,
                   &crypto_info, sizeof(crypto_info)) < 0) {
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd; (void)params;
  return unexpected(errc::not_supported);
#endif
}

/**
 * @brief 소켓의 수신 경로에 kTLS를 활성화합니다 (TLS 1.3 AES-128-GCM).
 *
 * `setsockopt(fd, SOL_TLS, TLS_RX, ...)` 호출로 이후 `recv()/read()`가
 * 커널 내에서 TLS 복호화를 수행합니다.
 *
 * @param sockfd  TLS 핸드셰이크가 완료된 TCP 소켓 fd.
 * @param params  TLS 세션 파라미터.
 * @returns 성공 시 `Result<void>`, 실패 시 오류 코드.
 */
[[nodiscard]] inline Result<void> enable_rx(
    int sockfd, const KtlsSessionParams &params) noexcept {
#if defined(QBUEM_HAS_KTLS) && defined(TLS_RX)
  struct tls12_crypto_info_aes_gcm_128 crypto_info{};
  crypto_info.info.version    = TLS_1_3_VERSION;
  crypto_info.info.cipher_type = TLS_CIPHER_AES_GCM_128;
  std::memcpy(crypto_info.key, params.key.data(), sizeof(crypto_info.key));
  std::memcpy(crypto_info.iv,  params.iv.data() + 4, sizeof(crypto_info.iv));
  std::memcpy(crypto_info.salt, params.iv.data(), sizeof(crypto_info.salt));
  std::memcpy(crypto_info.rec_seq, params.seq.data(),
              sizeof(crypto_info.rec_seq));

  if (::setsockopt(sockfd, SOL_TLS, TLS_RX,
                   &crypto_info, sizeof(crypto_info)) < 0) {
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd; (void)params;
  return unexpected(errc::not_supported);
#endif
}

/**
 * @brief 소켓에 kTLS (TX + RX 모두)를 한 번에 활성화합니다.
 *
 * @param sockfd   TLS 소켓 fd.
 * @param tx_params 송신 세션 파라미터.
 * @param rx_params 수신 세션 파라미터.
 * @returns 성공 시 `Result<void>`. TX 또는 RX 중 하나라도 실패하면 에러.
 */
[[nodiscard]] inline Result<void> enable_ktls(
    int sockfd,
    const KtlsSessionParams &tx_params,
    const KtlsSessionParams &rx_params) noexcept {
  if (auto r = enable_tx(sockfd, tx_params); !r) return r;
  return enable_rx(sockfd, rx_params);
}

// ---------------------------------------------------------------------------
// kTLS 상태 확인
// ---------------------------------------------------------------------------

/**
 * @brief 소켓에 kTLS TX가 활성화되어 있는지 확인합니다.
 *
 * @param sockfd 확인할 소켓 fd.
 * @returns kTLS TX 활성화 여부.
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
 * @brief 소켓에 kTLS RX가 활성화되어 있는지 확인합니다.
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
// SOL_TLS 소켓 준비
// ---------------------------------------------------------------------------

/**
 * @brief TCP 소켓을 kTLS 사용을 위해 준비합니다.
 *
 * TLS 핸드셰이크 완료 후 kTLS 활성화 전에 호출합니다.
 * 내부적으로 `setsockopt(fd, IPPROTO_TCP, TCP_ULP, "tls", 3)`를 사용합니다.
 *
 * @param sockfd 준비할 TCP 소켓 fd.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> prepare_socket_for_ktls(int sockfd) noexcept {
#if defined(__linux__)
  if (::setsockopt(sockfd, IPPROTO_TCP, TCP_ULP, "tls", sizeof("tls") - 1) < 0) {
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd;
  return unexpected(errc::not_supported);
#endif
}

} // namespace qbuem::io

/** @} */
