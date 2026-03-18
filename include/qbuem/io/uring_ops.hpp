#pragma once

/**
 * @file qbuem/io/uring_ops.hpp
 * @brief io_uring advanced async I/O operations — ACCEPT_MULTISHOT, RECV_MULTISHOT,
 *        Fixed Files, Linked SQEs, SOCKET+CONNECT
 * @defgroup qbuem_uring_ops io_uring Advanced Operations
 * @ingroup qbuem_io
 *
 * Use this header to directly control high-performance io_uring operations
 * beyond the basic POLL_ADD approach used by `IOUringReactor`.
 *
 * ## Linux kernel version requirements per operation
 * | Operation | Minimum Kernel |
 * |-----------|----------------|
 * | IORING_OP_ACCEPT_MULTISHOT | 5.19+ |
 * | IORING_OP_RECV_MULTISHOT   | 5.19+ |
 * | io_uring Fixed Files        | 5.1+  |
 * | Linked SQEs (IOSQE_IO_LINK) | 5.3+ |
 * | IORING_OP_SOCKET + CONNECT  | 5.19+ |
 * | IORING_OP_FUTEX_WAIT/WAKE   | 6.7+  |
 *
 * ## Usage Example
 * ```cpp
 * // Register Fixed Files
 * std::vector<int> fds = {server_fd, client_fd};
 * auto r = qbuem::io::uring_register_files(ring, fds);
 *
 * // ACCEPT_MULTISHOT (accept multiple connections with 1 SQE)
 * qbuem::io::uring_accept_multishot(ring, server_fd, on_accept);
 * ```
 *
 * @note This header is designed for use alongside liburing (`io_uring_sqe`, `io_uring_cqe`).
 *       A real implementation is provided only when `QBUEM_HAS_IOURING` is defined;
 *       otherwise an `errc::not_supported` fallback is used.
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
 * @brief Register io_uring Fixed Files.
 *
 * Registered fds can be referenced by file index with the `IOSQE_FIXED_FILE`
 * flag in SQEs, eliminating the per-SQE fd lookup overhead.
 *
 * @param ring      Target io_uring instance.
 * @param fds       List of fds to register. -1 reserves a slot.
 * @returns `Result<void>` on success.
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
 * @brief Unregister previously registered Fixed Files.
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
 * @brief Update Fixed Files slots (replace fds).
 *
 * Replaces individual slot fds without unregistering the existing registration.
 *
 * @param ring       Target io_uring.
 * @param offset     Starting slot index for the update.
 * @param fds        New list of fds.
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
 * @brief Submit an ACCEPT_MULTISHOT SQE — accept multiple connections with a single SQE.
 *
 * Unlike `IORING_OP_ACCEPT` which requires one SQE per connection,
 * MULTISHOT continuously generates CQEs from a single SQE to accept multiple connections.
 *
 * @param ring         Target io_uring.
 * @param listen_fd    Listening server socket fd.
 * @param user_data    CQE user_data tag (for callback dispatch).
 * @returns `Result<void>` on success.
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
 * @brief Submit a RECV_MULTISHOT SQE — receive multiple packets with a single SQE.
 *
 * Used together with an io_uring Buffer Ring to compose a fully zero-copy recv.
 *
 * @param ring        Target io_uring.
 * @param fd          Socket fd to receive on.
 * @param bgid        Buffer Group ID (see io_uring_register_buf_ring).
 * @param user_data   CQE user_data tag.
 * @returns `Result<void>` on success.
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
 * @brief Set the IOSQE_IO_LINK flag on an SQE.
 *
 * The next SQE executes after the current one completes (soft link).
 * If the current SQE fails, the chain is cancelled.
 *
 * @param sqe Pointer to the SQE to link (void* — minimizes liburing dependency).
 */
inline void uring_link_sqe([[maybe_unused]] void* sqe) noexcept {
#if defined(QBUEM_HAS_IOURING)
  auto* s = static_cast<struct io_uring_sqe*>(sqe);
  s->flags |= IOSQE_IO_LINK;
#endif
}

/**
 * @brief Set the IOSQE_IO_HARDLINK flag on an SQE.
 *
 * The next SQE executes even if the current SQE fails (hard link).
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
 * @brief IORING_OP_SOCKET — create a socket asynchronously via io_uring.
 *
 * Submits the `socket(2)` syscall through io_uring.
 *
 * @param ring      Target io_uring.
 * @param domain    Socket domain (AF_INET, AF_INET6, ...).
 * @param type      Socket type (SOCK_STREAM, SOCK_DGRAM, ...).
 * @param protocol  Protocol number.
 * @param user_data CQE user_data tag.
 * @returns `Result<void>` on success.
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
 * @brief IORING_OP_FUTEX_WAIT — wait on a futex via io_uring.
 *
 * Replaces eventfd-based wakeups.
 * Waits until the value at the futex address differs from `val`.
 *
 * @param ring      Target io_uring.
 * @param uaddr     Futex address to watch (uint32_t*).
 * @param val       Expected value.
 * @param user_data CQE user_data tag.
 * @returns `Result<void>` on success.
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
 * @brief IORING_OP_FUTEX_WAKE — wake waiters on a futex via io_uring.
 *
 * Wakes up to `nr_waiters` threads waiting at the specified futex address.
 *
 * @param ring        Target io_uring.
 * @param uaddr       Futex address.
 * @param nr_waiters  Number of threads to wake.
 * @param user_data   CQE user_data tag.
 * @returns `Result<void>` on success.
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
 * The io_uring equivalent of `MSG_ZEROCOPY`. Transmits the user buffer without copying.
 * Two CQEs are generated on completion: one for transmit done, one for buffer return notification.
 *
 * @param ring       Target io_uring.
 * @param fd         Socket fd to send on.
 * @param msg        Pointer to an msghdr struct.
 * @param flags      sendmsg flags.
 * @param user_data  CQE user_data tag.
 * @returns `Result<void>` on success.
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
