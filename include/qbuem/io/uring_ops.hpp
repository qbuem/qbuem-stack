#pragma once

/**
 * @file qbuem/io/uring_ops.hpp
 * @brief io_uring 고급 비동기 I/O 연산 — ACCEPT_MULTISHOT, RECV_MULTISHOT,
 *        Fixed Files, Linked SQEs, SOCKET+CONNECT
 * @defgroup qbuem_uring_ops io_uring Advanced Operations
 * @ingroup qbuem_io
 *
 * `IOUringReactor`의 기본 POLL_ADD 방식을 넘어서는 고성능 io_uring 연산을
 * 직접 제어할 때 사용합니다.
 *
 * ## 연산별 Linux 버전 요구사항
 * | 연산 | 최소 커널 |
 * |------|-----------|
 * | IORING_OP_ACCEPT_MULTISHOT | 5.19+ |
 * | IORING_OP_RECV_MULTISHOT   | 5.19+ |
 * | io_uring Fixed Files        | 5.1+  |
 * | Linked SQEs (IOSQE_IO_LINK) | 5.3+ |
 * | IORING_OP_SOCKET + CONNECT  | 5.19+ |
 * | IORING_OP_FUTEX_WAIT/WAKE   | 6.7+  |
 *
 * ## 사용 예시
 * ```cpp
 * // Fixed Files 등록
 * std::vector<int> fds = {server_fd, client_fd};
 * auto r = qbuem::io::uring_register_files(ring, fds);
 *
 * // ACCEPT_MULTISHOT (1 SQE로 다중 연결 수락)
 * qbuem::io::uring_accept_multishot(ring, server_fd, on_accept);
 * ```
 *
 * @note 이 헤더는 liburing (`io_uring_sqe`, `io_uring_cqe`)과 함께
 *       사용하도록 설계되었습니다. `QBUEM_HAS_IOURING`이 정의된 경우에만
 *       실제 구현이 제공되며, 그 외에는 `errc::not_supported` fallback.
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#if defined(QBUEM_HAS_IOURING)
#  include <liburing.h>
#endif

namespace qbuem::io {

// ---------------------------------------------------------------------------
// Fixed Files
// ---------------------------------------------------------------------------

/**
 * @brief io_uring Fixed Files 등록.
 *
 * 등록된 fd는 SQE에서 `IOSQE_FIXED_FILE` 플래그와 함께 파일 인덱스로
 * 참조할 수 있어 매 SQE마다 fd 조회 오버헤드를 제거합니다.
 *
 * @param ring      대상 io_uring 인스턴스.
 * @param fds       등록할 fd 목록. -1은 슬롯 예약으로 사용.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> uring_register_files(
    [[maybe_unused]] void* ring,
    [[maybe_unused]] const std::vector<int>& fds) noexcept {
#if defined(QBUEM_HAS_IOURING)
  auto* r = static_cast<struct io_uring*>(ring);
  int ret = ::io_uring_register_files(
      r,
      fds.data(),
      static_cast<unsigned>(fds.size()));
  if (ret < 0)
    return unexpected(std::error_code{-ret, std::system_category()});
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

/**
 * @brief 등록된 Fixed Files를 해제합니다.
 */
[[nodiscard]] inline Result<void> uring_unregister_files(
    [[maybe_unused]] void* ring) noexcept {
#if defined(QBUEM_HAS_IOURING)
  auto* r = static_cast<struct io_uring*>(ring);
  int ret = ::io_uring_unregister_files(r);
  if (ret < 0)
    return unexpected(std::error_code{-ret, std::system_category()});
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

/**
 * @brief Fixed Files 슬롯을 갱신합니다 (fd 교체).
 *
 * 기존 등록을 해제하지 않고 개별 슬롯의 fd를 교체합니다.
 *
 * @param ring       대상 io_uring.
 * @param offset     갱신 시작 슬롯 인덱스.
 * @param fds        새 fd 목록.
 */
[[nodiscard]] inline Result<void> uring_update_files(
    [[maybe_unused]] void* ring,
    [[maybe_unused]] unsigned offset,
    [[maybe_unused]] const std::vector<int>& fds) noexcept {
#if defined(QBUEM_HAS_IOURING)
  auto* r = static_cast<struct io_uring*>(ring);
  int ret = ::io_uring_register_files_update(
      r, offset,
      fds.data(),
      static_cast<unsigned>(fds.size()));
  if (ret < 0)
    return unexpected(std::error_code{-ret, std::system_category()});
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// IORING_OP_ACCEPT_MULTISHOT (Linux 5.19+)
// ---------------------------------------------------------------------------

/**
 * @brief ACCEPT_MULTISHOT SQE 제출 — SQE 1회로 다중 연결 수락.
 *
 * 기존 `IORING_OP_ACCEPT`는 연결 1개당 SQE 1개가 필요하지만,
 * MULTISHOT은 SQE 1개로 CQE를 계속 발생시켜 다중 연결을 수락합니다.
 *
 * @param ring         대상 io_uring.
 * @param listen_fd    리슨 중인 서버 소켓 fd.
 * @param user_data    CQE user_data 태그 (콜백 디스패치용).
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> uring_accept_multishot(
    [[maybe_unused]] void* ring,
    [[maybe_unused]] int listen_fd,
    [[maybe_unused]] uint64_t user_data) noexcept {
#if defined(QBUEM_HAS_IOURING) && defined(IORING_ACCEPT_MULTISHOT)
  auto* r = static_cast<struct io_uring*>(ring);
  struct io_uring_sqe* sqe = ::io_uring_get_sqe(r);
  if (!sqe)
    return unexpected(errc::resource_unavailable_try_again);
  ::io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr, 0);
  ::io_uring_sqe_set_data64(sqe, user_data);
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// IORING_OP_RECV_MULTISHOT (Linux 5.19+)
// ---------------------------------------------------------------------------

/**
 * @brief RECV_MULTISHOT SQE 제출 — SQE 1회로 다중 패킷 수신.
 *
 * io_uring Buffer Ring과 함께 사용하여 완전한 zero-copy recv를 구성합니다.
 *
 * @param ring        대상 io_uring.
 * @param fd          수신할 소켓 fd.
 * @param bgid        Buffer Group ID (io_uring_register_buf_ring 참조).
 * @param user_data   CQE user_data 태그.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> uring_recv_multishot(
    [[maybe_unused]] void* ring,
    [[maybe_unused]] int fd,
    [[maybe_unused]] uint16_t bgid,
    [[maybe_unused]] uint64_t user_data) noexcept {
#if defined(QBUEM_HAS_IOURING) && defined(IORING_RECV_MULTISHOT)
  auto* r = static_cast<struct io_uring*>(ring);
  struct io_uring_sqe* sqe = ::io_uring_get_sqe(r);
  if (!sqe)
    return unexpected(errc::resource_unavailable_try_again);
  ::io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
  ::io_uring_sqe_set_buf_group(sqe, bgid);
  ::io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
  ::io_uring_sqe_set_data64(sqe, user_data);
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// Linked SQEs (IOSQE_IO_LINK / IOSQE_IO_HARDLINK)
// ---------------------------------------------------------------------------

/**
 * @brief SQE에 IOSQE_IO_LINK 플래그를 설정합니다.
 *
 * 현재 SQE가 완료된 후 다음 SQE가 실행됩니다 (소프트 링크).
 * 현재 SQE가 실패하면 체인이 중단됩니다.
 *
 * @param sqe 링크를 설정할 SQE 포인터 (void* — liburing 의존성 최소화).
 */
inline void uring_link_sqe([[maybe_unused]] void* sqe) noexcept {
#if defined(QBUEM_HAS_IOURING)
  auto* s = static_cast<struct io_uring_sqe*>(sqe);
  s->flags |= IOSQE_IO_LINK;
#endif
}

/**
 * @brief SQE에 IOSQE_IO_HARDLINK 플래그를 설정합니다.
 *
 * 현재 SQE가 실패해도 다음 SQE가 실행됩니다 (하드 링크).
 */
inline void uring_hardlink_sqe([[maybe_unused]] void* sqe) noexcept {
#if defined(QBUEM_HAS_IOURING)
  auto* s = static_cast<struct io_uring_sqe*>(sqe);
  s->flags |= IOSQE_IO_HARDLINK;
#endif
}

// ---------------------------------------------------------------------------
// IORING_OP_SOCKET + IORING_OP_CONNECT (Linux 5.19+)
// ---------------------------------------------------------------------------

/**
 * @brief IORING_OP_SOCKET — io_uring으로 소켓을 비동기 생성합니다.
 *
 * `socket(2)` 시스템 콜을 io_uring을 통해 제출합니다.
 *
 * @param ring      대상 io_uring.
 * @param domain    소켓 도메인 (AF_INET, AF_INET6, ...).
 * @param type      소켓 타입 (SOCK_STREAM, SOCK_DGRAM, ...).
 * @param protocol  프로토콜 번호.
 * @param user_data CQE user_data 태그.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> uring_socket(
    [[maybe_unused]] void* ring,
    [[maybe_unused]] int domain,
    [[maybe_unused]] int type,
    [[maybe_unused]] int protocol,
    [[maybe_unused]] uint64_t user_data) noexcept {
#if defined(QBUEM_HAS_IOURING) && defined(IORING_OP_SOCKET)
  auto* r = static_cast<struct io_uring*>(ring);
  struct io_uring_sqe* sqe = ::io_uring_get_sqe(r);
  if (!sqe)
    return unexpected(errc::resource_unavailable_try_again);
  ::io_uring_prep_socket(sqe, domain, type, protocol, 0);
  ::io_uring_sqe_set_data64(sqe, user_data);
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// IORING_OP_FUTEX_WAIT / IORING_OP_FUTEX_WAKE (Linux 6.7+)
// ---------------------------------------------------------------------------

/**
 * @brief IORING_OP_FUTEX_WAIT — io_uring으로 futex 대기.
 *
 * `eventfd` 기반 wakeup을 대체합니다.
 * futex 주소의 값이 `val`과 다를 때까지 대기합니다.
 *
 * @param ring      대상 io_uring.
 * @param uaddr     감시할 futex 주소 (uint32_t*).
 * @param val       기대 값.
 * @param user_data CQE user_data 태그.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> uring_futex_wait(
    [[maybe_unused]] void* ring,
    [[maybe_unused]] uint32_t* uaddr,
    [[maybe_unused]] uint64_t val,
    [[maybe_unused]] uint64_t user_data) noexcept {
#if defined(QBUEM_HAS_IOURING) && defined(IORING_OP_FUTEX_WAIT)
  auto* r = static_cast<struct io_uring*>(ring);
  struct io_uring_sqe* sqe = ::io_uring_get_sqe(r);
  if (!sqe)
    return unexpected(errc::resource_unavailable_try_again);
  ::io_uring_prep_futex_wait(sqe, uaddr, val, FUTEX_BITSET_MATCH_ANY,
                              FUTEX_PRIVATE_FLAG, 0);
  ::io_uring_sqe_set_data64(sqe, user_data);
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

/**
 * @brief IORING_OP_FUTEX_WAKE — io_uring으로 futex 깨우기.
 *
 * 지정한 futex 주소에서 대기 중인 최대 `nr_waiters` 개의 스레드를 깨웁니다.
 *
 * @param ring        대상 io_uring.
 * @param uaddr       futex 주소.
 * @param nr_waiters  깨울 스레드 수.
 * @param user_data   CQE user_data 태그.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> uring_futex_wake(
    [[maybe_unused]] void* ring,
    [[maybe_unused]] uint32_t* uaddr,
    [[maybe_unused]] uint32_t nr_waiters,
    [[maybe_unused]] uint64_t user_data) noexcept {
#if defined(QBUEM_HAS_IOURING) && defined(IORING_OP_FUTEX_WAKE)
  auto* r = static_cast<struct io_uring*>(ring);
  struct io_uring_sqe* sqe = ::io_uring_get_sqe(r);
  if (!sqe)
    return unexpected(errc::resource_unavailable_try_again);
  ::io_uring_prep_futex_wake(sqe, uaddr, nr_waiters, FUTEX_BITSET_MATCH_ANY,
                              FUTEX_PRIVATE_FLAG, 0);
  ::io_uring_sqe_set_data64(sqe, user_data);
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// IORING_OP_SENDMSG_ZC (Linux 6.0+)
// ---------------------------------------------------------------------------

/**
 * @brief IORING_OP_SENDMSG_ZC — io_uring zero-copy sendmsg.
 *
 * `MSG_ZEROCOPY`의 io_uring 버전. 사용자 버퍼를 복사 없이 전송합니다.
 * 완료 시 CQE가 2회 발생: 하나는 전송 완료, 하나는 버퍼 반환 알림.
 *
 * @param ring       대상 io_uring.
 * @param fd         전송할 소켓 fd.
 * @param msg        msghdr 구조체 포인터.
 * @param flags      sendmsg 플래그.
 * @param user_data  CQE user_data 태그.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> uring_sendmsg_zc(
    [[maybe_unused]] void* ring,
    [[maybe_unused]] int fd,
    [[maybe_unused]] void* msg,  // struct msghdr*
    [[maybe_unused]] unsigned flags,
    [[maybe_unused]] uint64_t user_data) noexcept {
#if defined(QBUEM_HAS_IOURING) && defined(IORING_OP_SENDMSG_ZC)
  auto* r = static_cast<struct io_uring*>(ring);
  struct io_uring_sqe* sqe = ::io_uring_get_sqe(r);
  if (!sqe)
    return unexpected(errc::resource_unavailable_try_again);
  ::io_uring_prep_sendmsg_zc(sqe, fd,
                              static_cast<struct msghdr*>(msg), flags);
  ::io_uring_sqe_set_data64(sqe, user_data);
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

} // namespace qbuem::io

/** @} */
