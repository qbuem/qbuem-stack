#pragma once

/**
 * @file qbuem/net/uds_advanced.hpp
 * @brief Unix Domain Socket 고급 기능 — FD passing (SCM_RIGHTS), SCM_CREDENTIALS.
 * @defgroup qbuem_uds_advanced UDS Advanced
 * @ingroup qbuem_net
 *
 * ## 개요
 * `SCM_RIGHTS`를 활용한 프로세스 간 파일 디스크립터 전달(FD passing)과
 * `SCM_CREDENTIALS`를 통한 peer 프로세스 자격증명 검증을 구현합니다.
 *
 * ## 핵심 기능
 * | 기능 | 시스템 콜 | 용도 |
 * |------|-----------|------|
 * | FD passing | `sendmsg(SCM_RIGHTS)` | 소켓/파일/memfd를 타 프로세스에 전달 |
 * | 자격증명 | `sendmsg(SCM_CREDENTIALS)` | peer UID/GID/PID 검증 |
 * | Vectored I/O | `sendmsg(iovec[])` | FD + 데이터를 단일 syscall로 전송 |
 *
 * ## FD Passing 활용 시나리오
 * - SHM 세그먼트(memfd)를 서비스 매니저가 워커에게 전달
 * - TLS 세션 핸들오버 (accept → worker 프로세스)
 * - 소켓 마이그레이션 (hot-swap 없이 연결 인계)
 *
 * @code
 * // 송신 측 (FD 전달)
 * int shm_fd = memfd_create("data", MFD_ALLOW_SEALING);
 * auto r = uds::send_fds(sock, {shm_fd}, std::span<const uint8_t>{});
 *
 * // 수신 측 (FD 수신)
 * std::array<int, 8> recv_fds;
 * auto r = uds::recv_fds(sock, recv_fds, buf);
 * int received_shm_fd = recv_fds[0]; // dup()된 새로운 fd
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
#endif

namespace qbuem::uds {

// ─── 상수 ─────────────────────────────────────────────────────────────────────

/** @brief 단일 `sendmsg`로 전달 가능한 최대 FD 수. */
inline constexpr size_t kMaxFdsPerMsg = 253; // SCM_MAX_FD (Linux)

// ─── PeerCredentials ─────────────────────────────────────────────────────────

/**
 * @brief UDS peer 프로세스 자격증명.
 *
 * `SO_PEERCRED` (Linux) 또는 `LOCAL_PEERCRED` (macOS)로 조회합니다.
 */
struct PeerCredentials {
    pid_t pid{-1};  ///< peer 프로세스 ID
    uid_t uid{~0u}; ///< peer 유효 사용자 ID
    gid_t gid{~0u}; ///< peer 유효 그룹 ID

    /** @brief root 프로세스 여부. */
    [[nodiscard]] bool is_root() const noexcept { return uid == 0; }
    /** @brief 특정 UID 인지 확인합니다. */
    [[nodiscard]] bool is_uid(uid_t expected) const noexcept { return uid == expected; }
};

// ─── FD Passing ──────────────────────────────────────────────────────────────

/**
 * @brief UDS 소켓을 통해 FD 배열을 전달합니다 (SCM_RIGHTS).
 *
 * 보조 데이터(ancillary data)로 FD를 전달하며,
 * 동시에 최대 `iov_size` 바이트의 일반 데이터도 전송합니다.
 * 데이터 없이 FD만 전송하려면 `data`를 빈 span으로 전달하세요.
 *
 * @param sockfd   보내는 UDS 소켓 fd (SOCK_STREAM 또는 SOCK_DGRAM).
 * @param fds      전달할 FD 배열 (최대 `kMaxFdsPerMsg`개).
 * @param data     함께 전송할 일반 데이터 (없으면 빈 span).
 * @returns 성공 시 전송된 데이터 바이트 수 (FD는 별도 계산 없음).
 *
 * @note 수신 측에서 각 FD는 독립적으로 `dup()`됩니다.
 *       수신 후 반드시 `close()`해야 합니다.
 */
[[nodiscard]] inline Result<ssize_t> send_fds(
    int                       sockfd,
    std::span<const int>      fds,
    std::span<const uint8_t>  data = {}) noexcept {
#if defined(__linux__) || defined(__APPLE__)
    if (fds.empty() || fds.size() > kMaxFdsPerMsg)
        return unexpected(std::make_error_code(std::errc::invalid_argument));

    // 보조 데이터 버퍼 계산
    const size_t cmsg_space = CMSG_SPACE(fds.size() * sizeof(int));
    // 스택 할당 (최대 kMaxFdsPerMsg FDs → 최대 ~4KB)
    alignas(struct cmsghdr) uint8_t cmsg_buf[CMSG_SPACE(kMaxFdsPerMsg * sizeof(int))]{};

    // iovec: 빈 페이로드여도 sendmsg는 1바이트 이상 필요 (Linux SOCK_STREAM 예외)
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

    // CMSG 헤더 설정
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
 * @brief UDS 소켓에서 FD 배열을 수신합니다 (SCM_RIGHTS).
 *
 * @param sockfd     수신 UDS 소켓 fd.
 * @param fds_out    수신된 FD를 저장할 배열 (최대 `kMaxFdsPerMsg`개).
 * @param data_buf   함께 수신된 데이터를 저장할 버퍼.
 * @returns {수신된_fd_수, 데이터_바이트_수} 쌍. 실패 시 에러.
 */
struct RecvFdsResult {
    size_t  fd_count{0};    ///< 수신된 FD 수
    ssize_t data_bytes{0};  ///< 수신된 데이터 바이트 수
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

    // 보조 데이터에서 FD 추출
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t fd_bytes = cmsg->cmsg_len - CMSG_LEN(0);
            size_t count = fd_bytes / sizeof(int);
            size_t to_copy = (count < fds_out.size()) ? count : fds_out.size();
            std::memcpy(fds_out.data(), CMSG_DATA(cmsg), to_copy * sizeof(int));
            result.fd_count = to_copy;
            // 넘친 FD는 즉시 닫기 (누수 방지)
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
 * @brief UDS 소켓의 peer 프로세스 자격증명을 조회합니다.
 *
 * - Linux: `getsockopt(SO_PEERCRED)` → `struct ucred`
 * - macOS: `getsockopt(LOCAL_PEERCRED)` → `struct xucred`
 *
 * @param sockfd  연결된 UDS SOCK_STREAM 소켓.
 * @returns peer 자격증명 또는 에러.
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
    // macOS xucred에는 PID 없음 → getpid() 대신 별도 SCM_CREDS 사용 필요
    return PeerCredentials{-1, cred.cr_uid, cred.cr_gid};
#else
    (void)sockfd;
    return unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

// ─── UDS 소켓 생성 헬퍼 ──────────────────────────────────────────────────────

/**
 * @brief 추상 네임스페이스 UDS 소켓 쌍을 생성합니다.
 *
 * 추상 네임스페이스(Linux 전용)는 파일시스템에 파일을 생성하지 않습니다.
 * 이름은 `\0` 접두사로 시작하며, 프로세스 종료 시 자동 삭제됩니다.
 *
 * @param name     소켓 이름 (e.g. "qbuem.control"). `\0` 접두사 자동 추가.
 * @param type     소켓 타입 (`SOCK_STREAM` 또는 `SOCK_DGRAM`).
 * @param listener 리스닝 소켓 fd (서버 측).
 * @returns 성공 시 `Result<void>`, 실패 시 에러.
 */
[[nodiscard]] inline Result<void> bind_abstract(
    std::string_view name, int type, int& listener) noexcept {
#if defined(__linux__)
    listener = ::socket(AF_UNIX, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0)
        return unexpected(std::error_code{errno, std::system_category()});

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    // 추상 네임스페이스: sun_path[0] = '\0', 이후 이름
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
 * @brief 추상 네임스페이스 UDS에 연결합니다.
 *
 * @param name 소켓 이름 (`\0` 접두사 자동 추가).
 * @param type 소켓 타입.
 * @returns 연결된 소켓 fd 또는 에러.
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
