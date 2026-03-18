#pragma once

/**
 * @file qbuem/io/socket_opts.hpp
 * @brief Advanced Linux socket option utilities for high-performance network I/O.
 * @defgroup qbuem_socket_opts Socket Options
 * @ingroup qbuem_io
 *
 * This header exposes advanced Linux kernel socket features through a
 * `Result<void>`-based interface. No exceptions are used; each function
 * calls `::setsockopt()` or `::getsockopt()` directly.
 *
 * ### Available Functions
 * | Function                       | Kernel Option                | Min Version   |
 * |--------------------------------|------------------------------|---------------|
 * | `set_incoming_cpu()`           | `SO_INCOMING_CPU`            | Linux 3.19+   |
 * | `set_reuseport_cbpf()`         | `SO_ATTACH_REUSEPORT_CBPF`   | Linux 4.5+    |
 * | `enable_tcp_migrate_req()`     | `TCP_MIGRATE_REQ`            | Linux 5.14+   |
 * | `set_tcp_fastopen()`           | `TCP_FASTOPEN`               | Linux 3.7+    |
 * | `set_zerocopy()`               | `SO_ZEROCOPY`                | Linux 4.14+   |
 *
 * ### Non-Linux Platforms
 * All platform-specific implementations are guarded by `#if defined(__linux__)`.
 * On non-Linux platforms, or when the kernel version is too old and
 * `ENOPROTOOPT` is returned, `errc::not_supported` is returned.
 *
 * ### Usage Example
 * @code
 * int fd = ::socket(AF_INET, SOCK_STREAM, 0);
 *
 * // Accept connections only from CPU 0
 * auto r1 = qbuem::io::set_incoming_cpu(fd, 0);
 * if (!r1) {
 *     // Kernel version too old or unsupported environment — ignore and continue
 * }
 *
 * // Enable TCP Fast Open (queue length 16)
 * if (auto r = qbuem::io::set_tcp_fastopen(fd, 16); !r) {
 *     // Handle fallback on failure
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(__linux__)
#  include <sys/socket.h>
#  include <netinet/tcp.h>
#  include <netinet/in.h>
// SO_INCOMING_CPU
#  ifndef SO_INCOMING_CPU
#    define SO_INCOMING_CPU 49
#  endif
// SO_ZEROCOPY
#  ifndef SO_ZEROCOPY
#    define SO_ZEROCOPY 60
#  endif
// TCP_FASTOPEN
#  ifndef TCP_FASTOPEN
#    define TCP_FASTOPEN 23
#  endif
// TCP_MIGRATE_REQ
#  ifndef TCP_MIGRATE_REQ
#    define TCP_MIGRATE_REQ 34
#  endif
// SO_ATTACH_REUSEPORT_CBPF
#  ifndef SO_ATTACH_REUSEPORT_CBPF
#    define SO_ATTACH_REUSEPORT_CBPF 51
#  endif
#  include <linux/filter.h>
#endif

namespace qbuem::io {

// ---------------------------------------------------------------------------
// SO_INCOMING_CPU
// ---------------------------------------------------------------------------

/**
 * @brief Binds a socket to accept connections only from the specified CPU.
 *        (`SO_INCOMING_CPU`, Linux 3.19+)
 *
 * Within a `SO_REUSEPORT` socket group, pins a specific socket to a specific
 * CPU/NUMA node to maximize CPU cache utilization and reduce cross-CPU
 * interrupts.
 *
 * The kernel detects the packet receive CPU via NIC IRQ affinity and
 * distributes connections to the matching socket.
 *
 * ### Notes
 * - Returns `errc::not_supported` on kernels older than 3.19 or non-Linux platforms.
 * - `ENOPROTOOPT` errors are also converted to `errc::not_supported`.
 * - Must be used together with IRQ affinity (`/proc/irq/<n>/smp_affinity`) for best effect.
 *
 * ### Usage Example
 * @code
 * // Map each SO_REUSEPORT socket 1:1 to a CPU number
 * for (int cpu = 0; cpu < num_cpus; ++cpu) {
 *     int fd = create_reuseport_socket();
 *     auto r = qbuem::io::set_incoming_cpu(fd, cpu);
 *     if (!r) {
 *         // Kernel does not support this — fall back to default distribution
 *     }
 * }
 * @endcode
 *
 * @param sockfd  Server socket fd belonging to a SO_REUSEPORT group.
 * @param cpu_id  CPU number to accept from (logical CPU, 0-based).
 * @returns `Result<void>::ok()` on success.
 *          `errc::not_supported` on non-Linux or `ENOPROTOOPT`.
 *          `std::system_category()` error_code for other errors.
 */
[[nodiscard]] inline Result<void> set_incoming_cpu(
    int sockfd, int cpu_id) noexcept {
#if defined(__linux__)
  if (::setsockopt(sockfd, SOL_SOCKET, SO_INCOMING_CPU,
                   &cpu_id, sizeof(cpu_id)) < 0) {
    int err = errno;
    if (err == ENOPROTOOPT)
      return unexpected(std::make_error_code(std::errc::not_supported));
    return unexpected(std::error_code{err, std::system_category()});
  }
  return {};
#else
  (void)sockfd; (void)cpu_id;
  return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

// ---------------------------------------------------------------------------
// SO_ATTACH_REUSEPORT_CBPF
// ---------------------------------------------------------------------------

/**
 * @brief Attaches a Classic BPF program to a `SO_REUSEPORT` socket group.
 *        (`SO_ATTACH_REUSEPORT_CBPF`, Linux 4.5+)
 *
 * Replaces the default `SO_REUSEPORT` distribution strategy (4-tuple hash)
 * by installing a Classic BPF program that selects a socket based on the
 * receive queue number (`skb->queue_mapping`).
 *
 * ### BPF Program Logic (internal implementation)
 * ```
 * A = skb->queue_mapping   // Load NIC receive queue number
 * A = A % group_size       // Determine socket index
 * return A                 // Route to the socket at that index
 * ```
 *
 * ### Usage Example
 * @code
 * // Create a SO_REUSEPORT group of 4 sockets
 * constexpr int kGroupSize = 4;
 * int fds[kGroupSize];
 * for (auto& fd : fds) {
 *     fd = create_reuseport_socket();
 * }
 * // Attaching to the first socket applies to the entire group
 * auto r = qbuem::io::set_reuseport_cbpf(fds[0], kGroupSize);
 * if (!r) {
 *     // Linux < 4.5 or no BPF support — fall back to default hash distribution
 * }
 * @endcode
 *
 * @param sockfd     Socket file descriptor to attach the cBPF program to.
 *                   Any socket in the `SO_REUSEPORT` group can be used.
 * @param group_size Number of sockets in the `SO_REUSEPORT` group. Used as modulus.
 * @returns `Result<void>::ok()` on success.
 *          `errc::not_supported` on non-Linux or `ENOPROTOOPT`.
 *          `std::system_category()` error_code for other errors.
 */
[[nodiscard]] inline Result<void> set_reuseport_cbpf(
    [[maybe_unused]] int sockfd,
    [[maybe_unused]] int group_size) noexcept {
#if defined(__linux__) && defined(BPF_LD)
  // cBPF: return skb->queue_mapping mod group_size as the socket index
  // SKF_AD_QUEUE (24): ancillary data offset for the skb receive queue number
  struct sock_filter bpf_prog[] = {
    // A = skb->queue_mapping (SKF_AD_QUEUE ancillary data)
    BPF_STMT(BPF_LD  | BPF_W | BPF_ABS,
             static_cast<uint32_t>(SKF_AD_OFF + SKF_AD_QUEUE)),
    // A = A % group_size
    BPF_STMT(BPF_ALU | BPF_MOD | BPF_K,
             static_cast<uint32_t>(group_size)),
    // return A
    BPF_STMT(BPF_RET | BPF_A, 0),
  };
  struct sock_fprog prog{
      .len    = static_cast<unsigned short>(std::size(bpf_prog)),
      .filter = bpf_prog,
  };
  if (::setsockopt(sockfd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF,
                   &prog, sizeof(prog)) < 0) {
    int err = errno;
    if (err == ENOPROTOOPT)
      return unexpected(std::make_error_code(std::errc::not_supported));
    return unexpected(std::error_code{err, std::system_category()});
  }
  return {};
#else
  return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

// ---------------------------------------------------------------------------
// TCP_MIGRATE_REQ
// ---------------------------------------------------------------------------

/**
 * @brief Enables TCP connection migration within a `SO_REUSEPORT` group.
 *        (`TCP_MIGRATE_REQ`, Linux 5.14+)
 *
 * When `TCP_MIGRATE_REQ` is set, if a socket in a `SO_REUSEPORT` group is
 * closed, pending connection requests in that socket's SYN backlog are
 * transparently migrated to another socket in the group.
 *
 * This allows maintaining service continuity during rolling upgrades or
 * worker restarts without TCP connection loss.
 *
 * ### Requirements
 * - The socket must be a member of a `SO_REUSEPORT` group.
 * - Linux 5.14 or newer.
 * - `ENOPROTOOPT`: unsupported kernel version — returns `errc::not_supported`.
 *
 * ### Usage Example
 * @code
 * int fd = create_reuseport_socket();
 * auto r = qbuem::io::enable_tcp_migrate_req(fd);
 * if (!r) {
 *     // May fail on Linux < 5.14 or when SO_REUSEPORT is not set — ignore
 * }
 * @endcode
 *
 * @param sockfd TCP socket file descriptor to enable migration on.
 * @returns `Result<void>::ok()` on success.
 *          `errc::not_supported` on non-Linux or `ENOPROTOOPT`.
 *          `std::system_category()` error_code for other errors.
 */
[[nodiscard]] inline Result<void> enable_tcp_migrate_req(int sockfd) noexcept {
#if defined(__linux__)
  int val = 1;
  if (::setsockopt(sockfd, IPPROTO_TCP, TCP_MIGRATE_REQ,
                   &val, sizeof(val)) < 0) {
    int err = errno;
    // ENOPROTOOPT: kernel does not support this (Linux < 5.14)
    if (err == ENOPROTOOPT)
      return unexpected(std::make_error_code(std::errc::not_supported));
    return unexpected(std::error_code{err, std::system_category()});
  }
  return {};
#else
  (void)sockfd;
  return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

// ---------------------------------------------------------------------------
// TCP_FASTOPEN
// ---------------------------------------------------------------------------

/**
 * @brief Enables TCP Fast Open (TFO).
 *        (`TCP_FASTOPEN`, Linux 3.7+)
 *
 * TCP Fast Open allows data to be sent in the first SYN packet, performing
 * the TCP 3-way handshake and data transfer simultaneously, saving one RTT
 * of connection setup latency.
 *
 * Must be set on the server socket before calling `listen()`.
 * `queue_len` specifies the maximum size of the TFO cookie queue.
 *
 * ### System-level Activation Required
 * ```
 * echo 3 > /proc/sys/net/ipv4/tcp_fastopen
 * # 1 = client TFO, 2 = server TFO, 3 = both
 * ```
 *
 * ### Usage Example
 * @code
 * int fd = ::socket(AF_INET, SOCK_STREAM, 0);
 * // ... bind() ...
 * if (auto r = qbuem::io::set_tcp_fastopen(fd, 16); !r) {
 *     // ENOPROTOOPT: kernel version < 3.7 or system setting not enabled
 * }
 * ::listen(fd, SOMAXCONN);
 * @endcode
 *
 * @param sockfd    TCP socket file descriptor to configure TFO on.
 * @param queue_len Maximum TFO pending connection queue length (default: 10).
 * @returns `Result<void>::ok()` on success.
 *          `errc::not_supported` on non-Linux or `ENOPROTOOPT`.
 *          `std::system_category()` error_code for other errors.
 */
[[nodiscard]] inline Result<void> set_tcp_fastopen(
    int sockfd, int queue_len = 10) noexcept {
#if defined(__linux__)
  if (::setsockopt(sockfd, IPPROTO_TCP, TCP_FASTOPEN,
                   &queue_len, sizeof(queue_len)) < 0) {
    int err = errno;
    if (err == ENOPROTOOPT)
      return unexpected(std::make_error_code(std::errc::not_supported));
    return unexpected(std::error_code{err, std::system_category()});
  }
  return {};
#else
  (void)sockfd; (void)queue_len;
  return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

// ---------------------------------------------------------------------------
// SO_ZEROCOPY
// ---------------------------------------------------------------------------

/**
 * @brief Enables zero-copy send (`SO_ZEROCOPY`) on a socket.
 *        (`SO_ZEROCOPY`, Linux 4.14+)
 *
 * After enabling, calling `send(fd, buf, len, MSG_ZEROCOPY)` causes the
 * kernel to reference the userspace buffer directly and pass it to the
 * network stack without copying. Eliminates memory copy overhead for
 * large buffer transmissions.
 *
 * ### Completion Notification
 * When transmission is complete, the kernel posts a `sock_extended_err`
 * message to the socket's errqueue.
 * Call `recvmsg(fd, ..., MSG_ERRQUEUE)` to receive the notification before
 * reusing the buffer.
 * (See `qbuem::zero_copy::wait_zerocopy()`)
 *
 * ### Performance Considerations
 * - For small data (<4KB), regular `send()` may be faster.
 * - Best suited for large file or streaming transfers.
 * - The buffer must not be modified until the kernel sends a completion notification.
 *
 * ### Usage Example
 * @code
 * int fd = ::socket(AF_INET, SOCK_STREAM, 0);
 * // ... connect() ...
 *
 * if (auto r = qbuem::io::set_zerocopy(fd); !r) {
 *     // SO_ZEROCOPY not supported — fall back to regular send()
 * } else {
 *     // Can now use send() with MSG_ZEROCOPY flag
 *     ::send(fd, large_buf.data(), large_buf.size(), MSG_ZEROCOPY);
 *     // Wait for completion: co_await qbuem::zero_copy::wait_zerocopy(fd);
 * }
 * @endcode
 *
 * @param sockfd Socket file descriptor to enable zero-copy on.
 * @returns `Result<void>::ok()` on success.
 *          `errc::not_supported` on non-Linux or `ENOPROTOOPT`.
 *          `std::system_category()` error_code for other errors.
 */
[[nodiscard]] inline Result<void> set_zerocopy(int sockfd) noexcept {
#if defined(__linux__)
  int val = 1;
  if (::setsockopt(sockfd, SOL_SOCKET, SO_ZEROCOPY,
                   &val, sizeof(val)) < 0) {
    int err = errno;
    if (err == ENOPROTOOPT)
      return unexpected(std::make_error_code(std::errc::not_supported));
    return unexpected(std::error_code{err, std::system_category()});
  }
  return {};
#else
  (void)sockfd;
  return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

// ---------------------------------------------------------------------------
// SO_REUSEPORT / SO_REUSEADDR enable utilities
// ---------------------------------------------------------------------------

/**
 * @brief Enables `SO_REUSEPORT`.
 *
 * Allows multiple sockets to share the same port; the kernel automatically
 * distributes incoming connections.
 * Use together with `SO_INCOMING_CPU` / `SO_ATTACH_REUSEPORT_CBPF`.
 *
 * @param sockfd Server socket file descriptor.
 * @returns `Result<void>::ok()` on success.
 *          `errc::not_supported` on unsupported platforms.
 *          `std::system_category()` error_code for other errors.
 */
[[nodiscard]] inline Result<void> set_reuseport(int sockfd) noexcept {
#if defined(__linux__) || defined(__APPLE__)
  int val = 1;
  if (::setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT,
                   &val, sizeof(val)) < 0) {
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd;
  return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

/**
 * @brief Enables `SO_REUSEADDR`.
 *
 * Allows immediate reuse of a port in the TIME_WAIT state.
 * Prevents `Address already in use` errors on server restart.
 *
 * @param sockfd Socket file descriptor.
 * @returns `Result<void>::ok()` on success.
 *          `std::system_category()` error_code on failure.
 */
[[nodiscard]] inline Result<void> set_reuseaddr(int sockfd) noexcept {
  int val = 1;
  if (::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
                   &val, sizeof(val)) < 0) {
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
}

} // namespace qbuem::io

/** @} */
