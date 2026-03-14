#pragma once

/**
 * @file qbuem/io/socket_opts.hpp
 * @brief 고성능 네트워크 IO를 위한 고급 Linux 소켓 옵션
 * @defgroup qbuem_socket_opts Socket Options
 * @ingroup qbuem_io
 *
 * ## 제공 옵션
 * | 함수 | 소켓 옵션 | 최소 커널 |
 * |------|-----------|-----------|
 * | set_incoming_cpu()     | SO_INCOMING_CPU     | 3.19+ |
 * | set_reuseport_cbpf()   | SO_ATTACH_REUSEPORT_CBPF | 4.5+ |
 * | enable_tcp_migrate_req() | TCP_MIGRATE_REQ   | 5.14+ |
 * | set_tcp_fastopen()     | TCP_FASTOPEN        | 3.7+  |
 * | set_zerocopy()         | SO_ZEROCOPY         | 4.14+ |
 *
 * ## 사용 예시
 * ```cpp
 * // 연결을 CPU 3의 reactor에 고정
 * auto r = qbuem::io::set_incoming_cpu(server_fd, 3);
 *
 * // SO_REUSEPORT 그룹에서 BPF로 연결 분배
 * qbuem::io::set_reuseport_cbpf(server_fd, 4);  // 4개 소켓 그룹
 * ```
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>

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
 * @brief 소켓을 지정한 CPU에서 오는 연결만 수락하도록 바인딩합니다.
 *
 * `SO_INCOMING_CPU` (Linux 3.19+)를 사용하여 인바운드 연결을 특정 CPU에
 * 고정합니다. `SO_REUSEPORT` 그룹의 각 소켓에 서로 다른 CPU를 지정하면
 * 연결이 해당 CPU의 reactor에 자동으로 라우팅됩니다.
 *
 * 결합 권장: `SO_REUSEPORT` + `SO_INCOMING_CPU` + NUMA 바인딩
 *
 * @param sockfd  SO_REUSEPORT 그룹에 속한 서버 소켓 fd.
 * @param cpu_id  연결을 수락할 CPU ID (logical CPU, 0-based).
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> set_incoming_cpu(
    int sockfd, int cpu_id) noexcept {
#if defined(__linux__)
  if (::setsockopt(sockfd, SOL_SOCKET, SO_INCOMING_CPU,
                   &cpu_id, sizeof(cpu_id)) < 0) {
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd; (void)cpu_id;
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// SO_ATTACH_REUSEPORT_CBPF
// ---------------------------------------------------------------------------

/**
 * @brief SO_REUSEPORT 소켓 그룹에 cBPF 프로그램을 부착합니다.
 *
 * `SO_ATTACH_REUSEPORT_CBPF` (Linux 4.5+)를 사용하여 연결을
 * 소켓 그룹 내 특정 소켓으로 라우팅하는 BPF 필터를 설정합니다.
 *
 * 기본 구현: 소켓 인덱스 = 4-tuple 해시 mod group_size.
 * 이 방식은 CPU 친화성 없이 단순 로드 밸런싱에 적합합니다.
 * CPU-aware 분배는 `SO_INCOMING_CPU`를 사용하세요.
 *
 * @param sockfd      SO_REUSEPORT 그룹의 소켓 fd.
 * @param group_size  그룹 내 소켓 수.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> set_reuseport_cbpf(
    [[maybe_unused]] int sockfd,
    [[maybe_unused]] int group_size) noexcept {
#if defined(__linux__) && defined(BPF_LD)
  // 간단한 cBPF: skb->hash mod group_size
  // 실제 프로덕션에서는 더 정교한 BPF 프로그램을 사용하세요.
  struct sock_filter bpf_prog[] = {
    // A = skb->napi_id (연결 해시로 사용)
    { BPF_LD  | BPF_W | BPF_ABS, 0, 0, static_cast<uint32_t>(__builtin_offsetof(struct sk_buff, hash)) },
    // X = group_size
    { BPF_LD  | BPF_W | BPF_IMM, 0, 0, static_cast<uint32_t>(group_size) },
    // A = A mod X
    { BPF_ALU | BPF_MOD | BPF_X, 0, 0, 0 },
    // return A
    { BPF_RET | BPF_A,            0, 0, 0 },
  };
  struct sock_fprog prog{
      .len    = static_cast<unsigned short>(std::size(bpf_prog)),
      .filter = bpf_prog,
  };
  if (::setsockopt(sockfd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF,
                   &prog, sizeof(prog)) < 0) {
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// TCP_MIGRATE_REQ
// ---------------------------------------------------------------------------

/**
 * @brief SO_REUSEPORT 그룹 내 연결 마이그레이션을 활성화합니다.
 *
 * `TCP_MIGRATE_REQ` (Linux 5.14+)를 설정하면 연결 수락 전에
 * 소켓이 닫혀도 SYN 큐의 요청이 동일 그룹 내 다른 소켓으로 이동합니다.
 * 무중단 서버 재시작(graceful restart)에 필수적입니다.
 *
 * @param sockfd  TCP 서버 소켓 fd.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> enable_tcp_migrate_req(int sockfd) noexcept {
#if defined(__linux__)
  int val = 1;
  if (::setsockopt(sockfd, IPPROTO_TCP, TCP_MIGRATE_REQ,
                   &val, sizeof(val)) < 0) {
    // ENOPROTOOPT: 커널이 지원하지 않음 (Linux < 5.14)
    if (errno == ENOPROTOOPT)
      return unexpected(errc::not_supported);
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd;
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// TCP_FASTOPEN
// ---------------------------------------------------------------------------

/**
 * @brief TCP Fast Open을 활성화합니다.
 *
 * `TCP_FASTOPEN` (Linux 3.7+) — 3-way handshake 중 데이터 전송으로
 * 첫 연결의 RTT를 줄입니다. 서버 소켓에 설정합니다.
 *
 * @param sockfd     TCP 리스닝 소켓 fd.
 * @param queue_len  TFO 요청 큐 길이 (기본 10).
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> set_tcp_fastopen(
    int sockfd, int queue_len = 10) noexcept {
#if defined(__linux__)
  if (::setsockopt(sockfd, IPPROTO_TCP, TCP_FASTOPEN,
                   &queue_len, sizeof(queue_len)) < 0) {
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd; (void)queue_len;
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// SO_ZEROCOPY
// ---------------------------------------------------------------------------

/**
 * @brief 소켓에 zero-copy 송신을 활성화합니다.
 *
 * `SO_ZEROCOPY` (Linux 4.14+) — 이후 `send(..., MSG_ZEROCOPY)`를 사용하여
 * 사용자 버퍼를 커널 공간으로 복사하지 않고 직접 전송합니다.
 * 완료 확인은 `recvmsg(MSG_ERRQUEUE)` + `sock_extended_err`로 수행합니다.
 *
 * @param sockfd  TCP/UDP 소켓 fd.
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] inline Result<void> set_zerocopy(int sockfd) noexcept {
#if defined(__linux__)
  int val = 1;
  if (::setsockopt(sockfd, SOL_SOCKET, SO_ZEROCOPY,
                   &val, sizeof(val)) < 0) {
    return unexpected(std::error_code{errno, std::system_category()});
  }
  return {};
#else
  (void)sockfd;
  return unexpected(errc::not_supported);
#endif
}

// ---------------------------------------------------------------------------
// SO_REUSEPORT 활성화 유틸리티
// ---------------------------------------------------------------------------

/**
 * @brief SO_REUSEPORT를 활성화합니다.
 *
 * 여러 소켓이 동일한 포트를 공유하여 커널이 연결을 자동 분배합니다.
 * `SO_INCOMING_CPU` / `SO_ATTACH_REUSEPORT_CBPF`와 함께 사용합니다.
 *
 * @param sockfd  서버 소켓 fd.
 * @returns 성공 시 `Result<void>`.
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
  return unexpected(errc::not_supported);
#endif
}

/**
 * @brief SO_REUSEADDR를 활성화합니다.
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
