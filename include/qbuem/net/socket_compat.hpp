#pragma once

/**
 * @file qbuem/net/socket_compat.hpp
 * @brief Portable non-blocking + close-on-exec socket primitives (Linux + macOS).
 * @ingroup qbuem_net
 *
 * Linux exposes the `SOCK_NONBLOCK` / `SOCK_CLOEXEC` `socket()` type flags and
 * `accept4(2)`. macOS / BSD do not, so the equivalent descriptor state must be
 * applied with `fcntl(2)` immediately after creation. This header centralizes
 * the portable path so every socket-creation site in qbuem-stack is identical,
 * and supporting a new platform only requires touching this one file.
 *
 * All factories return a descriptor that is **non-blocking + close-on-exec**
 * (the qbuem-stack reactor invariant), except `make_socket_blocking_cloexec`,
 * which is for intentionally-synchronous handoff sockets (e.g. SCM_RIGHTS FD
 * passing).
 */

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace qbuem::net {

/**
 * @brief Apply `O_NONBLOCK` + `FD_CLOEXEC` to an existing descriptor.
 * @return 0 on success, -1 on failure (errno set).
 */
[[nodiscard]] inline int set_nonblock_cloexec(int fd) noexcept {
  int fl = ::fcntl(fd, F_GETFL, 0);
  if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
  int fdfl = ::fcntl(fd, F_GETFD, 0);
  if (fdfl < 0 || ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0) return -1;
  return 0;
}

/**
 * @brief Apply `FD_CLOEXEC` only (leaves the descriptor blocking).
 * @return 0 on success, -1 on failure (errno set).
 */
[[nodiscard]] inline int set_cloexec(int fd) noexcept {
  int fdfl = ::fcntl(fd, F_GETFD, 0);
  if (fdfl < 0 || ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0) return -1;
  return 0;
}

/**
 * @brief Create a non-blocking, close-on-exec socket on both Linux and macOS.
 * @param domain   Address family (AF_INET, AF_INET6, AF_UNIX, ...).
 * @param type     Base socket type WITHOUT NONBLOCK/CLOEXEC (e.g. SOCK_STREAM).
 * @param protocol Protocol (usually 0).
 * @return descriptor on success, or -1 with errno set.
 */
[[nodiscard]] inline int make_socket(int domain, int type, int protocol) noexcept {
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  return ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
#else
  int fd = ::socket(domain, type, protocol);
  if (fd < 0) return fd;
  if (set_nonblock_cloexec(fd) < 0) {
    int e = errno;
    ::close(fd);
    errno = e;
    return -1;
  }
  return fd;
#endif
}

/**
 * @brief Create a blocking, close-on-exec socket on both Linux and macOS.
 *
 * For handoff/setup sockets that are intentionally synchronous.
 * @return descriptor on success, or -1 with errno set.
 */
[[nodiscard]] inline int make_socket_blocking_cloexec(int domain, int type,
                                                      int protocol) noexcept {
#if defined(SOCK_CLOEXEC)
  return ::socket(domain, type | SOCK_CLOEXEC, protocol);
#else
  int fd = ::socket(domain, type, protocol);
  if (fd < 0) return fd;
  if (set_cloexec(fd) < 0) {
    int e = errno;
    ::close(fd);
    errno = e;
    return -1;
  }
  return fd;
#endif
}

/**
 * @brief `accept()` that yields a non-blocking, close-on-exec client fd.
 *
 * Uses `accept4(2)` on Linux; falls back to `accept(2)` + `fcntl(2)` on
 * macOS / BSD. Drop-in for `::accept`/`::accept4`.
 * @return client descriptor on success, or -1 with errno set.
 */
[[nodiscard]] inline int accept_nonblock_cloexec(int listen_fd, sockaddr* addr,
                                                 socklen_t* len) noexcept {
#if defined(__linux__) && defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  return ::accept4(listen_fd, addr, len, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
  int fd = ::accept(listen_fd, addr, len);
  if (fd < 0) return fd;
  if (set_nonblock_cloexec(fd) < 0) {
    int e = errno;
    ::close(fd);
    errno = e;
    return -1;
  }
  return fd;
#endif
}

} // namespace qbuem::net
