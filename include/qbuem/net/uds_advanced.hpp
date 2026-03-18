#pragma once

/**
 * @file qbuem/net/uds_advanced.hpp
 * @brief Unix Domain Socket advanced features — FD passing (SCM_RIGHTS), SCM_CREDENTIALS.
 * @defgroup qbuem_uds_advanced UDS Advanced
 * @ingroup qbuem_net
 *
 * ## Overview
 * Implements inter-process file descriptor passing (FD passing) using `SCM_RIGHTS`
 * and peer process credential verification via `SCM_CREDENTIALS`.
 *
 * ## Key Features
 * | Feature | Syscall | Purpose |
 * |---------|---------|---------|
 * | FD passing | `sendmsg(SCM_RIGHTS)` | Transfer socket/file/memfd to another process |
 * | Credentials | `sendmsg(SCM_CREDENTIALS)` | Verify peer UID/GID/PID |
 * | Vectored I/O | `sendmsg(iovec[])` | Send FD + data in a single syscall |
 *
 * ## FD Passing Use Cases
 * - Service manager passes SHM segments (memfd) to worker processes
 * - TLS session handover (accept -> worker process)
 * - Socket migration (connection takeover without hot-swap)
 *
 * @code
 * // Sender side (FD passing)
 * int shm_fd = memfd_create("data", MFD_ALLOW_SEALING);
 * auto r = uds::send_fds(sock, {shm_fd}, std::span<const uint8_t>{});
 *
 * // Receiver side (FD receiving)
 * std::array<int, 8> recv_fds;
 * auto r = uds::recv_fds(sock, recv_fds, buf);
 * int received_shm_fd = recv_fds[0]; // newly dup()'d fd
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#if defined(__linux__) || defined(__APPLE__)
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <sys/uio.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <sys/ucred.h>
#  endif
#endif

namespace qbuem::uds {

// ─── Constants ────────────────────────────────────────────────────────────────

/** @brief Maximum number of FDs transferable in a single `sendmsg`. */
inline constexpr size_t kMaxFdsPerMsg = 253; // SCM_MAX_FD (Linux)

// ─── PeerCredentials ─────────────────────────────────────────────────────────

/**
 * @brief Credentials of the UDS peer process.
 *
 * Queried via `SO_PEERCRED` (Linux) or `LOCAL_PEERCRED` (macOS).
 */
struct PeerCredentials {
    pid_t pid{-1};  ///< Peer process ID
    uid_t uid{~0u}; ///< Peer effective user ID
    gid_t gid{~0u}; ///< Peer effective group ID

    /** @brief Returns true if the peer process is running as root. */
    [[nodiscard]] bool is_root() const noexcept { return uid == 0; }
    /** @brief Returns true if the peer UID matches the expected value. */
    [[nodiscard]] bool is_uid(uid_t expected) const noexcept { return uid == expected; }
};

// ─── FD Passing ──────────────────────────────────────────────────────────────

/**
 * @brief Send an array of FDs over a UDS socket (SCM_RIGHTS).
 *
 * Sends FDs as ancillary data and simultaneously transmits up to
 * `iov_size` bytes of regular data. To send FDs only (no data),
 * pass an empty span for `data`.
 *
 * @param sockfd   Sending UDS socket fd (SOCK_STREAM or SOCK_DGRAM).
 * @param fds      Array of FDs to transfer (at most `kMaxFdsPerMsg`).
 * @param data     Regular data to send alongside the FDs (empty span if none).
 * @returns Number of data bytes sent on success (FD count is not included).
 *
 * @note Each received FD is independently `dup()`'d on the receiver side.
 *       The receiver must `close()` the FDs after use.
 */
[[nodiscard]] inline Result<ssize_t> send_fds(
    int                       sockfd,
    std::span<const int>      fds,
    std::span<const uint8_t>  data = {}) noexcept {
#if defined(__linux__) || defined(__APPLE__)
    if (fds.empty() || fds.size() > kMaxFdsPerMsg)
        return unexpected(std::make_error_code(std::errc::invalid_argument));

    // Compute ancillary data buffer size
    const size_t cmsg_space = CMSG_SPACE(fds.size() * sizeof(int));
    // Stack-allocated buffer (max kMaxFdsPerMsg FDs -> up to ~4KB)
    alignas(struct cmsghdr) uint8_t cmsg_buf[CMSG_SPACE(kMaxFdsPerMsg * sizeof(int))]{};

    // iovec: sendmsg on Linux SOCK_STREAM requires at least 1 byte even for FD-only sends
    uint8_t dummy_byte = 0;
    struct iovec iov{};
    if (!data.empty()) {
        iov.iov_base = const_cast<uint8_t*>(data.data());
        iov.iov_len  = data.size();
    } else {
        iov.iov_base = &dummy_byte;
        iov.iov_len  = 1;
    }

    struct msghdr msg{};
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = static_cast<socklen_t>(cmsg_space);

    // Set CMSG header
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = static_cast<socklen_t>(CMSG_LEN(fds.size() * sizeof(int)));
    std::memcpy(CMSG_DATA(cmsg), fds.data(), fds.size() * sizeof(int));

    ssize_t sent = ::sendmsg(sockfd, &msg, MSG_NOSIGNAL);
    if (sent < 0)
        return unexpected(std::error_code{errno, std::system_category()});
    return sent;
#else
    (void)sockfd; (void)fds; (void)data;
    return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

/**
 * @brief Receive an array of FDs over a UDS socket (SCM_RIGHTS).
 *
 * @param sockfd     Receiving UDS socket fd.
 * @param fds_out    Array to store received FDs (at most `kMaxFdsPerMsg`).
 * @param data_buf   Buffer to store regular data received alongside FDs.
 * @returns A {fd_count, data_bytes} pair on success, or an error on failure.
 */
struct RecvFdsResult {
    size_t  fd_count{0};    ///< Number of FDs received
    ssize_t data_bytes{0};  ///< Number of data bytes received
};

[[nodiscard]] inline Result<RecvFdsResult> recv_fds(
    int                  sockfd,
    std::span<int>       fds_out,
    std::span<uint8_t>   data_buf = {}) noexcept {
#if defined(__linux__) || defined(__APPLE__)
    alignas(struct cmsghdr) uint8_t cmsg_buf[CMSG_SPACE(kMaxFdsPerMsg * sizeof(int))]{};

    uint8_t dummy_byte = 0;
    struct iovec iov{};
    if (!data_buf.empty()) {
        iov.iov_base = data_buf.data();
        iov.iov_len  = data_buf.size();
    } else {
        iov.iov_base = &dummy_byte;
        iov.iov_len  = 1;
    }

    struct msghdr msg{};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t n = ::recvmsg(sockfd, &msg, MSG_WAITALL);
    if (n < 0)
        return unexpected(std::error_code{errno, std::system_category()});

    RecvFdsResult result;
    result.data_bytes = data_buf.empty() ? 0 : n;

    // Extract FDs from ancillary data
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t fd_bytes = cmsg->cmsg_len - CMSG_LEN(0);
            size_t count = fd_bytes / sizeof(int);
            size_t to_copy = (count < fds_out.size()) ? count : fds_out.size();
            std::memcpy(fds_out.data(), CMSG_DATA(cmsg), to_copy * sizeof(int));
            result.fd_count = to_copy;
            // Close any excess FDs to prevent leaks
            const int* raw_fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            for (size_t i = to_copy; i < count; ++i) ::close(raw_fds[i]);
            break;
        }
    }

    return result;
#else
    (void)sockfd; (void)fds_out; (void)data_buf;
    return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

// ─── Peer Credentials ────────────────────────────────────────────────────────

/**
 * @brief Query the peer process credentials of a UDS socket.
 *
 * - Linux: `getsockopt(SO_PEERCRED)` -> `struct ucred`
 * - macOS: `getsockopt(LOCAL_PEERCRED)` -> `struct xucred`
 *
 * @param sockfd  Connected UDS SOCK_STREAM socket.
 * @returns Peer credentials or an error.
 */
[[nodiscard]] inline Result<PeerCredentials> get_peer_credentials(int sockfd) noexcept {
#if defined(__linux__)
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(sockfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
        return unexpected(std::error_code{errno, std::system_category()});
    return PeerCredentials{cred.pid, cred.uid, cred.gid};
#elif defined(__APPLE__)
    struct xucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(sockfd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &len) < 0)
        return unexpected(std::error_code{errno, std::system_category()});
    // macOS xucred does not include PID; use SCM_CREDS separately if needed
    return PeerCredentials{-1, cred.cr_uid, cred.cr_gid};
#else
    (void)sockfd;
    return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

// ─── UDS Socket Creation Helpers ─────────────────────────────────────────────

/**
 * @brief Bind a UDS socket to an abstract namespace address.
 *
 * Abstract namespace (Linux only) does not create a file on the filesystem.
 * The name begins with a `\0` prefix and is automatically removed when
 * the process exits.
 *
 * @param name     Socket name (e.g. "qbuem.control"). `\0` prefix is added automatically.
 * @param type     Socket type (`SOCK_STREAM` or `SOCK_DGRAM`).
 * @param listener Listening socket fd (server side).
 * @returns `Result<void>` on success, or an error on failure.
 */
[[nodiscard]] inline Result<void> bind_abstract(
    std::string_view name, int type, int& listener) noexcept {
#if defined(__linux__)
    listener = ::socket(AF_UNIX, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0)
        return unexpected(std::error_code{errno, std::system_category()});

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    // Abstract namespace: sun_path[0] = '\0', followed by the name
    size_t len = name.size() < sizeof(addr.sun_path) - 1
                     ? name.size() : sizeof(addr.sun_path) - 1;
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, name.data(), len);
    socklen_t addrlen = static_cast<socklen_t>(
        offsetof(struct sockaddr_un, sun_path) + 1 + len);

    if (::bind(listener, reinterpret_cast<struct sockaddr*>(&addr), addrlen) < 0) {
        ::close(listener); listener = -1;
        return unexpected(std::error_code{errno, std::system_category()});
    }
    if (type == SOCK_STREAM && ::listen(listener, 128) < 0) {
        ::close(listener); listener = -1;
        return unexpected(std::error_code{errno, std::system_category()});
    }
    return {};
#else
    (void)name; (void)type; (void)listener;
    return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

/**
 * @brief Connect to an abstract namespace UDS address.
 *
 * @param name Socket name (`\0` prefix is added automatically).
 * @param type Socket type.
 * @returns Connected socket fd or an error.
 */
[[nodiscard]] inline Result<int> connect_abstract(
    std::string_view name, int type) noexcept {
#if defined(__linux__)
    int fd = ::socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return unexpected(std::error_code{errno, std::system_category()});

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    size_t len = name.size() < sizeof(addr.sun_path) - 1
                     ? name.size() : sizeof(addr.sun_path) - 1;
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, name.data(), len);
    socklen_t addrlen = static_cast<socklen_t>(
        offsetof(struct sockaddr_un, sun_path) + 1 + len);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), addrlen) < 0) {
        ::close(fd);
        return unexpected(std::error_code{errno, std::system_category()});
    }
    return fd;
#else
    (void)name; (void)type;
    return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

} // namespace qbuem::uds

/** @} */
