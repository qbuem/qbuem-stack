#pragma once

/**
 * @file qbuem/xdp/xsk.hpp
 * @brief AF_XDP 소켓 (XSK) 추상화 — zero-copy 패킷 송수신.
 * @ingroup qbuem_xdp
 *
 * `XskSocket`은 AF_XDP 소켓을 감싸는 RAII 래퍼입니다.
 * UMEM과 결합하여 NIC ↔ 유저스페이스 간 커널 네트워크 스택을 완전히 우회하는
 * 제로 카피 패킷 I/O를 제공합니다.
 *
 * ### 전제 조건
 * - Linux 4.18+ (`AF_XDP`)
 * - Linux 5.4+ (`XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD` 없이도 동작)
 * - libbpf 활성화: `cmake -DQBUEM_XDP_LIBBPF=ON ..`
 * - CAP_NET_ADMIN 또는 루트 권한
 * - NIC 드라이버의 XDP 지원 (native mode: i40e, mlx5, ixgbe 등)
 *
 * ### 동작 방식
 * ```
 *  NIC (native XDP)
 *      │ zero-copy DMA
 *      ▼
 *  UMEM (mmap shared memory)
 *      │
 *  ┌───┴──────────────────────┐
 *  │  XskSocket               │
 *  │  Rx Ring → recv()        │
 *  │  Tx Ring ← send()        │
 *  │  Fill Ring → 커널에 빈 프레임 공급
 *  │  Completion Ring ← TX 완료
 *  └──────────────────────────┘
 * ```
 *
 * ### 사용 예시
 * @code
 * // 1. UMEM 생성
 * auto umem = qbuem::xdp::Umem::create({.frame_count = 4096});
 *
 * // 2. XSK 소켓 생성
 * auto xsk = qbuem::xdp::XskSocket::create("eth0", 0, *umem, {});
 * if (!xsk) { /* 에러 처리 */ }
 *
 * // 3. Fill Ring 준비
 * umem->fill_frames(2048);
 *
 * // 4. 수신 루프
 * while (true) {
 *     qbuem::xdp::UmemFrame frames[64];
 *     uint32_t n = xsk->recv(frames, 64);
 *     for (uint32_t i = 0; i < n; ++i) {
 *         auto* pkt = umem->data(frames[i]);
 *         process_packet(pkt, frames[i].len);
 *     }
 *     umem->fill_frames(n); // 소비한 만큼 재공급
 * }
 * @endcode
 *
 * @{
 */

#ifdef QBUEM_HAS_XDP

#include <qbuem/xdp/umem.hpp>

#include <cstdint>
#include <string_view>

#if defined(QBUEM_XDP_LIBBPF) || defined(QBUEM_HAS_XDP)
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#ifdef QBUEM_XDP_LIBBPF
#  include <xdp/xsk.h>
#  include <net/if.h>
#endif

namespace qbuem::xdp {

// ─── XskConfig ────────────────────────────────────────────────────────────

/**
 * @brief XSK 소켓 생성 설정.
 */
struct XskConfig {
    /** @brief Rx Ring 크기 (2^n). */
    uint32_t rx_size = kDefaultRingSize;

    /** @brief Tx Ring 크기 (2^n). */
    uint32_t tx_size = kDefaultRingSize;

    /** @brief XDP 로드 모드. */
    enum class Mode : uint8_t {
        Native  = 0, ///< Native mode (NIC 드라이버 내 XDP hook, 최고 성능)
        Skb     = 1, ///< SKB mode (generic fallback, 모든 NIC 지원)
        Offload = 2, ///< Offload mode (NIC 하드웨어에서 XDP 실행)
    } mode = Mode::Native;

    /** @brief NEED_WAKEUP 플래그 사용 (Linux 5.3+, 불필요한 syscall 감소). */
    bool need_wakeup = true;

    /** @brief Zero-copy 모드 강제 활성화 (Native mode + 드라이버 지원 필수). */
    bool force_zerocopy = false;

    /** @brief Shared UMEM (동일 UMEM을 여러 큐에서 공유). */
    bool shared_umem = false;
};

// ─── XskSocket ────────────────────────────────────────────────────────────

/**
 * @brief AF_XDP 소켓 — 커널 네트워크 스택 우회 zero-copy I/O.
 *
 * Move-only RAII 래퍼. 소멸 시 소켓과 XDP 프로그램이 자동으로 해제됩니다.
 *
 * ### 성능 특성
 * | 모드              | 대역폭       | CPU 사용률 | 비고                    |
 * |-------------------|-------------|-----------|-------------------------|
 * | Native XDP        | ~100 Gbps   | 매우 낮음  | 드라이버 지원 필수       |
 * | SKB (generic)     | ~20 Gbps    | 낮음       | 모든 NIC 동작            |
 * | Offload           | 선 속도      | ~0%        | 일부 SmartNIC 전용       |
 */
class XskSocket {
public:
    XskSocket(const XskSocket&)            = delete;
    XskSocket& operator=(const XskSocket&) = delete;

    XskSocket(XskSocket&& other) noexcept
#ifdef QBUEM_XDP_LIBBPF
        : xsk_(other.xsk_)
        , rx_(other.rx_)
        , tx_(other.tx_)
        , fd_(other.fd_)
#else
        : fd_(-1)
#endif
    {
#ifdef QBUEM_XDP_LIBBPF
        other.xsk_ = nullptr;
        other.fd_  = -1;
#endif
    }

    ~XskSocket() { destroy(); }

    // ── 팩토리 ──────────────────────────────────────────────────────────

    /**
     * @brief XSK 소켓을 생성하고 인터페이스/큐에 바인딩합니다.
     *
     * @param ifname   네트워크 인터페이스 이름 (예: "eth0").
     * @param queue_id NIC 수신 큐 번호 (0부터 시작). 멀티큐: 큐당 1개 소켓.
     * @param umem     연결할 UMEM 인스턴스 (참조, 수명 관리 사용자 책임).
     * @param cfg      소켓 설정.
     * @returns 성공 시 XskSocket, 실패 시 error_code.
     */
    static Result<XskSocket> create(std::string_view ifname,
                                    uint32_t         queue_id,
                                    Umem&            umem,
                                    const XskConfig& cfg = {}) {
#ifdef QBUEM_XDP_LIBBPF
        XskSocket s;

        xsk_socket_config xsk_cfg{};
        xsk_cfg.rx_size      = cfg.rx_size;
        xsk_cfg.tx_size      = cfg.tx_size;
        xsk_cfg.libbpf_flags = 0;

        switch (cfg.mode) {
            case XskConfig::Mode::Native:
                xsk_cfg.xdp_flags = XDP_FLAGS_DRV_MODE;
                break;
            case XskConfig::Mode::Skb:
                xsk_cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
                break;
            case XskConfig::Mode::Offload:
                xsk_cfg.xdp_flags = XDP_FLAGS_HW_MODE;
                break;
        }
        if (cfg.force_zerocopy) {
            xsk_cfg.bind_flags = XDP_ZEROCOPY;
        } else {
            xsk_cfg.bind_flags = XDP_COPY; // 폴백 허용
        }
        if (cfg.need_wakeup) {
            xsk_cfg.bind_flags |= XDP_USE_NEED_WAKEUP;
        }

        // null-terminate ifname
        char ifbuf[IFNAMSIZ]{};
        ifname.copy(ifbuf, std::min(ifname.size(), size_t{IFNAMSIZ - 1}));

        int ret;
        if (cfg.shared_umem) {
            ret = xsk_socket__create_shared(&s.xsk_, ifbuf, queue_id,
                                            umem.handle(),
                                            &s.rx_, &s.tx_,
                                            umem.fill_ring(), umem.comp_ring(),
                                            &xsk_cfg);
        } else {
            ret = xsk_socket__create(&s.xsk_, ifbuf, queue_id,
                                     umem.handle(),
                                     &s.rx_, &s.tx_,
                                     &xsk_cfg);
        }
        if (ret != 0) {
            return unexpected(std::error_code(-ret, std::system_category()));
        }
        s.fd_ = xsk_socket__fd(s.xsk_);
        return s;
#else
        (void)ifname; (void)queue_id; (void)umem; (void)cfg;
        return unexpected(std::make_error_code(std::errc::not_supported));
#endif
    }

    // ── 수신 ────────────────────────────────────────────────────────────

    /**
     * @brief Rx Ring에서 패킷을 최대 `max_n`개 폴링합니다 (non-blocking).
     *
     * `recv()` 후 소비한 프레임 수만큼 `Umem::fill_frames()`를 호출해
     * Fill Ring을 보충해야 합니다.
     *
     * @param[out] frames  수신된 프레임 정보 배열.
     * @param      max_n   최대 수신 개수.
     * @returns 실제로 수신한 프레임 수.
     */
    uint32_t recv(UmemFrame* frames, uint32_t max_n) noexcept {
#ifdef QBUEM_XDP_LIBBPF
        uint32_t idx = 0;
        uint32_t rcvd = xsk_ring_cons__peek(&rx_, max_n, &idx);
        for (uint32_t i = 0; i < rcvd; ++i) {
            const xdp_desc* desc = xsk_ring_cons__rx_desc(&rx_, idx++);
            frames[i].addr    = desc->addr;
            frames[i].len     = desc->len;
            frames[i].options = desc->options;
        }
        xsk_ring_cons__release(&rx_, rcvd);
        return rcvd;
#else
        (void)frames; (void)max_n;
        return 0;
#endif
    }

    // ── 송신 ────────────────────────────────────────────────────────────

    /**
     * @brief Tx Ring에 `n`개 프레임을 큐잉하고 커널에 전송을 요청합니다.
     *
     * @param frames  전송할 프레임 배열 (addr + len 설정 필수).
     * @param n       전송할 프레임 수.
     * @returns 실제로 큐잉된 프레임 수. Tx Ring 포화 시 n보다 적을 수 있음.
     */
    uint32_t send(const UmemFrame* frames, uint32_t n) noexcept {
#ifdef QBUEM_XDP_LIBBPF
        uint32_t idx = 0;
        uint32_t reserved = xsk_ring_prod__reserve(&tx_, n, &idx);
        for (uint32_t i = 0; i < reserved; ++i) {
            xdp_desc* desc = xsk_ring_prod__tx_desc(&tx_, idx++);
            desc->addr    = frames[i].addr;
            desc->len     = frames[i].len;
            desc->options = frames[i].options;
        }
        xsk_ring_prod__submit(&tx_, reserved);

        // NEED_WAKEUP: 커널 wakeup 필요 시에만 sendto() 호출
        if (xsk_ring_prod__needs_wakeup(&tx_)) {
            ::sendto(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
        }
        return reserved;
#else
        (void)frames; (void)n;
        return 0;
#endif
    }

    // ── 팩토리 (런타임 감지) ──────────────────────────────────────────

    /**
     * @brief 이 커널/플랫폼에서 AF_XDP가 지원되는지 런타임에 감지합니다.
     *
     * `socket(AF_XDP, SOCK_RAW, 0)`을 시험 생성하여 지원 여부를 확인합니다.
     * 소켓은 즉시 닫힙니다 (부작용 없음).
     *
     * ### 반환값
     * - `true`  : AF_XDP 소켓 생성 가능 (Linux 4.18+, AF_XDP 활성화)
     * - `false` : 미지원 커널, 빌드 플래그 누락, 또는 권한 부족
     *
     * @code
     * if (!qbuem::xdp::XskSocket::is_supported()) {
     *     // AF_XDP 미지원 — UdpSocket 폴백 사용
     * }
     * @endcode
     */
    [[nodiscard]] static bool is_supported() noexcept {
#ifdef QBUEM_HAS_XDP
#  ifdef QBUEM_XDP_LIBBPF
      // libbpf 빌드: AF_XDP 소켓 직접 감지
      int fd = ::socket(AF_XDP, SOCK_RAW, 0);
      if (fd < 0) return false;
      ::close(fd);
      return true;
#  else
      // 스텁 빌드: libbpf 없이 컴파일됐으므로 런타임 지원 불가
      return false;
#  endif
#else
      return false;
#endif
    }

    // ── 접근자 ──────────────────────────────────────────────────────────

    /** @brief 소켓 파일 디스크립터. epoll 등록에 사용. */
    [[nodiscard]] int fd() const noexcept { return fd_; }

    /** @brief 소켓이 유효한지 확인. */
    [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }

private:
    XskSocket() noexcept
#ifdef QBUEM_XDP_LIBBPF
        : xsk_(nullptr), rx_{}, tx_{}, fd_(-1)
#else
        : fd_(-1)
#endif
    {}

    void destroy() noexcept {
#ifdef QBUEM_XDP_LIBBPF
        if (xsk_) {
            xsk_socket__delete(xsk_);
            xsk_ = nullptr;
            fd_  = -1;
        }
#endif
    }

#ifdef QBUEM_XDP_LIBBPF
    xsk_socket*   xsk_;
    xsk_ring_cons rx_;
    xsk_ring_prod tx_;
#endif
    int fd_;
};

} // namespace qbuem::xdp

#endif // QBUEM_HAS_XDP

/** @} */ // end of qbuem_xdp
