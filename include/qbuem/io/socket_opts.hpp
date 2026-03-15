#pragma once

/**
 * @file qbuem/io/socket_opts.hpp
 * @brief 고성능 네트워크 IO를 위한 고급 Linux 소켓 옵션 설정 유틸리티.
 * @defgroup qbuem_socket_opts Socket Options
 * @ingroup qbuem_io
 *
 * 이 헤더는 Linux 커널의 고급 소켓 기능을 `Result<void>` 기반 인터페이스로
 * 제공합니다. 예외를 사용하지 않으며, 각 함수는 `::setsockopt()` 또는
 * `::getsockopt()`를 직접 호출합니다.
 *
 * ### 제공 함수
 * | 함수                           | 커널 옵션                    | 최소 버전     |
 * |--------------------------------|------------------------------|---------------|
 * | `set_incoming_cpu()`           | `SO_INCOMING_CPU`            | Linux 3.19+   |
 * | `set_reuseport_cbpf()`         | `SO_ATTACH_REUSEPORT_CBPF`   | Linux 4.5+    |
 * | `enable_tcp_migrate_req()`     | `TCP_MIGRATE_REQ`            | Linux 5.14+   |
 * | `set_tcp_fastopen()`           | `TCP_FASTOPEN`               | Linux 3.7+    |
 * | `set_zerocopy()`               | `SO_ZEROCOPY`                | Linux 4.14+   |
 *
 * ### 비Linux 플랫폼
 * 모든 플랫폼별 구현은 `#if defined(__linux__)` 가드로 보호됩니다.
 * 비Linux 환경에서 호출하거나 커널 버전이 낮아 `ENOPROTOOPT`가 반환되면
 * `errc::not_supported`를 반환합니다.
 *
 * ### 사용 예시
 * @code
 * int fd = ::socket(AF_INET, SOCK_STREAM, 0);
 *
 * // CPU 0 번에서만 연결 수신
 * auto r1 = qbuem::io::set_incoming_cpu(fd, 0);
 * if (!r1) {
 *     // 커널 버전이 낮거나 지원하지 않는 환경 → 무시하고 계속
 * }
 *
 * // TCP Fast Open 활성화 (대기열 길이 16)
 * if (auto r = qbuem::io::set_tcp_fastopen(fd, 16); !r) {
 *     // 실패 시 폴백 처리
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
 * @brief 소켓이 지정된 CPU에서 온 연결만 수신하도록 바인딩합니다.
 *        (`SO_INCOMING_CPU`, Linux 3.19+)
 *
 * `SO_REUSEPORT` 소켓 그룹 내에서 특정 소켓을 특정 CPU/NUMA 노드에 귀속시켜
 * CPU 캐시 활용도를 극대화하고 크로스-CPU 인터럽트를 줄입니다.
 *
 * 커널이 NIC IRQ affinity를 통해 패킷 수신 CPU를 감지하고,
 * 해당 CPU와 일치하는 소켓으로 연결을 분배합니다.
 *
 * ### 주의사항
 * - 커널 3.19 미만 또는 비Linux 환경에서는 `errc::not_supported` 반환.
 * - `ENOPROTOOPT` 에러도 `errc::not_supported`로 변환합니다.
 * - IRQ affinity(`/proc/irq/<n>/smp_affinity`)와 함께 사용해야 최대 효과.
 *
 * ### 사용 예시
 * @code
 * // SO_REUSEPORT 그룹의 각 소켓을 CPU 번호와 1:1 매핑
 * for (int cpu = 0; cpu < num_cpus; ++cpu) {
 *     int fd = create_reuseport_socket();
 *     auto r = qbuem::io::set_incoming_cpu(fd, cpu);
 *     if (!r) {
 *         // 커널이 지원하지 않으면 무시하고 기본 분배 사용
 *     }
 * }
 * @endcode
 *
 * @param sockfd  SO_REUSEPORT 그룹에 속한 서버 소켓 fd.
 * @param cpu_id  수신을 허용할 CPU 번호 (logical CPU, 0-based).
 * @returns 성공 시 `Result<void>::ok()`.
 *          비Linux 또는 `ENOPROTOOPT` 시 `errc::not_supported`.
 *          기타 에러 시 `std::system_category()` error_code.
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
 * @brief `SO_REUSEPORT` 소켓 그룹에 Classic BPF 프로그램을 부착합니다.
 *        (`SO_ATTACH_REUSEPORT_CBPF`, Linux 4.5+)
 *
 * 기본 `SO_REUSEPORT` 분배 전략(4-tuple 해시)을 대체하여
 * 수신 큐 번호(`skb->queue_mapping`)를 기반으로 소켓을 선택하는
 * Classic BPF 프로그램을 설치합니다.
 *
 * ### BPF 프로그램 로직 (내부 구현)
 * ```
 * A = skb->queue_mapping   // NIC 수신 큐 번호 로드
 * A = A % group_size       // 소켓 인덱스 결정
 * return A                 // 해당 인덱스 소켓으로 라우팅
 * ```
 *
 * ### 사용 예시
 * @code
 * // 4개 소켓의 SO_REUSEPORT 그룹 생성
 * constexpr int kGroupSize = 4;
 * int fds[kGroupSize];
 * for (auto& fd : fds) {
 *     fd = create_reuseport_socket();
 * }
 * // 첫 번째 소켓에 부착하면 그룹 전체에 적용됨
 * auto r = qbuem::io::set_reuseport_cbpf(fds[0], kGroupSize);
 * if (!r) {
 *     // Linux < 4.5 또는 BPF 지원 없음 → 기본 해시 분배로 폴백
 * }
 * @endcode
 *
 * @param sockfd     cBPF를 부착할 소켓 파일 디스크립터.
 *                   `SO_REUSEPORT` 그룹 내의 임의 소켓에 부착하면 됩니다.
 * @param group_size `SO_REUSEPORT` 그룹 내 소켓 수. 모듈로 연산에 사용됩니다.
 * @returns 성공 시 `Result<void>::ok()`.
 *          비Linux 또는 `ENOPROTOOPT` 시 `errc::not_supported`.
 *          기타 에러 시 `std::system_category()` error_code.
 */
[[nodiscard]] inline Result<void> set_reuseport_cbpf(
    [[maybe_unused]] int sockfd,
    [[maybe_unused]] int group_size) noexcept {
#if defined(__linux__) && defined(BPF_LD)
  // cBPF: skb->queue_mapping mod group_size → 소켓 인덱스 반환
  // SKF_AD_QUEUE (24): skb 수신 큐 번호에 대한 ancillary data 오프셋
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
 * @brief `SO_REUSEPORT` 그룹 내 TCP 연결 마이그레이션을 활성화합니다.
 *        (`TCP_MIGRATE_REQ`, Linux 5.14+)
 *
 * `TCP_MIGRATE_REQ`를 설정하면 `SO_REUSEPORT` 소켓 그룹에서
 * 한 소켓이 닫히는 경우 해당 소켓의 SYN 백로그에 있는 연결 요청을
 * 그룹 내 다른 소켓으로 투명하게 마이그레이션합니다.
 *
 * 롤링 업데이트(rolling upgrade) 또는 워커 재시작 중
 * TCP 연결 손실 없이 서비스 연속성을 유지할 수 있습니다.
 *
 * ### 요구 조건
 * - 소켓이 `SO_REUSEPORT`를 사용하는 그룹 멤버여야 합니다.
 * - Linux 5.14 이상.
 * - `ENOPROTOOPT`: 커널 버전 미지원 → `errc::not_supported` 반환.
 *
 * ### 사용 예시
 * @code
 * int fd = create_reuseport_socket();
 * auto r = qbuem::io::enable_tcp_migrate_req(fd);
 * if (!r) {
 *     // Linux < 5.14 또는 SO_REUSEPORT 미설정 시 실패 가능 → 무시
 * }
 * @endcode
 *
 * @param sockfd 마이그레이션을 활성화할 TCP 소켓 파일 디스크립터.
 * @returns 성공 시 `Result<void>::ok()`.
 *          비Linux 또는 `ENOPROTOOPT` 시 `errc::not_supported`.
 *          기타 에러 시 `std::system_category()` error_code.
 */
[[nodiscard]] inline Result<void> enable_tcp_migrate_req(int sockfd) noexcept {
#if defined(__linux__)
  int val = 1;
  if (::setsockopt(sockfd, IPPROTO_TCP, TCP_MIGRATE_REQ,
                   &val, sizeof(val)) < 0) {
    int err = errno;
    // ENOPROTOOPT: 커널이 지원하지 않음 (Linux < 5.14)
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
 * @brief TCP Fast Open(TFO)을 활성화합니다.
 *        (`TCP_FASTOPEN`, Linux 3.7+)
 *
 * TCP Fast Open은 TCP 3-way 핸드셰이크와 데이터 전송을 동시에 수행하여
 * 연결 설정 지연(RTT)을 1번 절감합니다.
 *
 * 서버 소켓에서 `listen()` 호출 전에 설정해야 합니다.
 * `queue_len`은 TFO 쿠키 대기열의 최대 크기를 지정합니다.
 *
 * ### 시스템 수준 활성화 필요
 * ```
 * echo 3 > /proc/sys/net/ipv4/tcp_fastopen
 * # 1 = 클라이언트 TFO, 2 = 서버 TFO, 3 = 양방향
 * ```
 *
 * ### 사용 예시
 * @code
 * int fd = ::socket(AF_INET, SOCK_STREAM, 0);
 * // ... bind() ...
 * if (auto r = qbuem::io::set_tcp_fastopen(fd, 16); !r) {
 *     // ENOPROTOOPT: 커널 버전 < 3.7 또는 시스템 설정 미활성화
 * }
 * ::listen(fd, SOMAXCONN);
 * @endcode
 *
 * @param sockfd    TFO를 설정할 TCP 소켓 파일 디스크립터.
 * @param queue_len TFO 보류 연결 대기열 최대 길이 (기본값: 10).
 * @returns 성공 시 `Result<void>::ok()`.
 *          비Linux 또는 `ENOPROTOOPT` 시 `errc::not_supported`.
 *          기타 에러 시 `std::system_category()` error_code.
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
 * @brief 소켓에 제로 카피 송신(`SO_ZEROCOPY`)을 활성화합니다.
 *        (`SO_ZEROCOPY`, Linux 4.14+)
 *
 * 활성화 후 `send(fd, buf, len, MSG_ZEROCOPY)` 호출 시 커널이 유저스페이스
 * 버퍼를 직접 참조하여 네트워크 스택으로 전달합니다.
 * 대용량 버퍼 전송 시 메모리 복사 오버헤드를 제거합니다.
 *
 * ### 완료 알림
 * 전송이 완료되면 커널이 소켓 errqueue에 `sock_extended_err` 메시지를 게시합니다.
 * `recvmsg(fd, ..., MSG_ERRQUEUE)`로 알림을 수신해야 버퍼를 재사용할 수 있습니다.
 * (`qbuem::zero_copy::wait_zerocopy()` 참조)
 *
 * ### 성능 고려사항
 * - 소량(<4KB) 데이터에서는 일반 `send()`가 더 빠를 수 있습니다.
 * - 대용량 파일 또는 스트리밍 전송에 적합합니다.
 * - 버퍼는 커널이 완료 알림을 보낼 때까지 수정하면 안 됩니다.
 *
 * ### 사용 예시
 * @code
 * int fd = ::socket(AF_INET, SOCK_STREAM, 0);
 * // ... connect() ...
 *
 * if (auto r = qbuem::io::set_zerocopy(fd); !r) {
 *     // SO_ZEROCOPY 비지원 환경 → 일반 send() 사용
 * } else {
 *     // MSG_ZEROCOPY 플래그와 함께 send() 사용 가능
 *     ::send(fd, large_buf.data(), large_buf.size(), MSG_ZEROCOPY);
 *     // 완료 대기: co_await qbuem::zero_copy::wait_zerocopy(fd);
 * }
 * @endcode
 *
 * @param sockfd 제로 카피를 활성화할 소켓 파일 디스크립터.
 * @returns 성공 시 `Result<void>::ok()`.
 *          비Linux 또는 `ENOPROTOOPT` 시 `errc::not_supported`.
 *          기타 에러 시 `std::system_category()` error_code.
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
// SO_REUSEPORT / SO_REUSEADDR 활성화 유틸리티
// ---------------------------------------------------------------------------

/**
 * @brief `SO_REUSEPORT`를 활성화합니다.
 *
 * 여러 소켓이 동일한 포트를 공유하여 커널이 연결을 자동 분배합니다.
 * `SO_INCOMING_CPU` / `SO_ATTACH_REUSEPORT_CBPF`와 함께 사용합니다.
 *
 * @param sockfd 서버 소켓 파일 디스크립터.
 * @returns 성공 시 `Result<void>::ok()`.
 *          지원하지 않는 플랫폼에서 `errc::not_supported`.
 *          기타 에러 시 `std::system_category()` error_code.
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
 * @brief `SO_REUSEADDR`를 활성화합니다.
 *
 * TIME_WAIT 상태의 포트를 즉시 재사용할 수 있게 합니다.
 * 서버 재시작 시 `Address already in use` 오류를 방지합니다.
 *
 * @param sockfd 소켓 파일 디스크립터.
 * @returns 성공 시 `Result<void>::ok()`.
 *          에러 시 `std::system_category()` error_code.
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
