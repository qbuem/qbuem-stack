#pragma once

/**
 * @file qbuem/xdp/umem.hpp
 * @brief AF_XDP UMEM (User Memory) 버퍼 풀.
 * @defgroup qbuem_xdp AF_XDP eXpress Data Path
 * @ingroup qbuem_xdp
 *
 * UMEM은 커널과 유저스페이스가 공유하는 메모리 영역입니다.
 * `mmap()`으로 할당한 연속 메모리를 `xsk_umem__create()`로 커널에 등록하면
 * 패킷 데이터가 이 영역에 직접 쓰여집니다 — 커널 → 유저 복사 없음.
 *
 * ### 의존성
 * - Linux 4.18+ (`AF_XDP` 소켓 지원)
 * - Linux 5.4+ (xsk_socket__create_shared, NEED_WAKEUP 플래그)
 * - libbpf (선택적: `QBUEM_XDP_LIBBPF=ON` CMake 옵션 활성화 시)
 *
 * ### CMake 사용법
 * ```cmake
 * find_package(qbuem-stack REQUIRED COMPONENTS xdp)
 * target_link_libraries(myapp PRIVATE qbuem-stack::xdp)
 * ```
 *
 * ### 주의사항
 * - UMEM 청크 크기는 반드시 2의 거듭제곱 (기본: 4096바이트)
 * - `O_DIRECT` 파일 I/O처럼 4096 바이트 정렬 필수
 * - RLIMIT_MEMLOCK 제한 상향 필요 (`ulimit -l unlimited`)
 *
 * @{
 */

#ifdef QBUEM_HAS_XDP

#include <qbuem/common.hpp>
#include <qbuem/core/arena.hpp>

#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

// libbpf AF_XDP 헤더 (QBUEM_XDP_LIBBPF=ON 일 때만 사용)
#ifdef QBUEM_XDP_LIBBPF
#  include <xdp/xsk.h>
#endif

namespace qbuem::xdp {

// ─── 상수 ─────────────────────────────────────────────────────────────────

/** @brief UMEM 기본 프레임 크기 (4 KiB). */
inline constexpr size_t kDefaultFrameSize = 4096;

/** @brief Fill Ring / Completion Ring 기본 크기. */
inline constexpr uint32_t kDefaultRingSize = 2048;

// ─── UmemConfig ───────────────────────────────────────────────────────────

/**
 * @brief UMEM 생성 설정.
 */
struct UmemConfig {
    /** @brief 전체 프레임 수. 메모리 = frame_count × frame_size. */
    uint32_t frame_count  = 4096;

    /** @brief 프레임 당 바이트 수. 반드시 2^n. */
    uint32_t frame_size   = kDefaultFrameSize;

    /** @brief Fill Ring 크기 (2^n). */
    uint32_t fill_size    = kDefaultRingSize;

    /** @brief Completion Ring 크기 (2^n). */
    uint32_t comp_size    = kDefaultRingSize;

    /** @brief Huge Pages 사용 여부 (TLB miss 감소). */
    bool     use_hugepages = false;
};

// ─── UmemFrame ────────────────────────────────────────────────────────────

/**
 * @brief UMEM 내 단일 패킷 프레임 참조.
 *
 * UMEM 메모리 기준 오프셋(`addr`)과 데이터 길이(`len`)로 구성됩니다.
 * 실제 데이터 포인터는 `Umem::data(frame)` 으로 얻습니다.
 */
struct UmemFrame {
    /** @brief UMEM 기준 오프셋 (바이트). */
    uint64_t addr;

    /** @brief 유효 데이터 길이 (바이트). */
    uint32_t len;

    /** @brief XDP 옵션 플래그 (XDP_OPTIONS_ZEROCOPY 등). */
    uint32_t options;
};

// ─── Umem ─────────────────────────────────────────────────────────────────

/**
 * @brief AF_XDP UMEM 버퍼 풀.
 *
 * `mmap()`으로 연속 메모리를 할당하고 커널에 UMEM으로 등록합니다.
 * 패킷 수신 시 커널이 이 메모리에 직접 데이터를 기록하므로
 * 커널 → 유저 복사가 완전히 제거됩니다.
 *
 * ### 사용 예시
 * @code
 * qbuem::xdp::UmemConfig cfg{
 *     .frame_count  = 4096,
 *     .frame_size   = 4096,
 *     .fill_size    = 2048,
 *     .comp_size    = 2048,
 *     .use_hugepages = true,
 * };
 * auto umem = qbuem::xdp::Umem::create(cfg);
 * if (!umem) { /* 에러 처리 */ }
 *
 * // Fill Ring에 프레임 등록 (커널이 이 주소에 패킷 기록)
 * umem->fill_frames(batch_size);
 * @endcode
 */
class Umem {
public:
    Umem(const Umem&)            = delete;
    Umem& operator=(const Umem&) = delete;

    Umem(Umem&& other) noexcept
        : cfg_(other.cfg_)
        , mem_(other.mem_)
        , mem_size_(other.mem_size_)
#ifdef QBUEM_XDP_LIBBPF
        , umem_(other.umem_)
        , fill_ring_(other.fill_ring_)
        , comp_ring_(other.comp_ring_)
#endif
        , next_free_frame_(other.next_free_frame_)
    {
        other.mem_  = MAP_FAILED;
        other.mem_size_ = 0;
#ifdef QBUEM_XDP_LIBBPF
        other.umem_ = nullptr;
#endif
    }

    ~Umem() { destroy(); }

    // ── 팩토리 ──────────────────────────────────────────────────────────

    /**
     * @brief UMEM을 생성하고 커널에 등록합니다.
     *
     * @param cfg  UMEM 설정.
     * @returns 성공 시 Umem, 실패 시 error_code.
     */
    static Result<Umem> create(const UmemConfig& cfg) {
        Umem u(cfg);

        // 1. mmap으로 정렬된 연속 메모리 할당
        const size_t total = static_cast<size_t>(cfg.frame_count) * cfg.frame_size;
        int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (cfg.use_hugepages) {
#ifdef MAP_HUGETLB
            mmap_flags |= MAP_HUGETLB;
#endif
        }
        void* mem = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
        if (mem == MAP_FAILED) {
            return unexpected(std::error_code(errno, std::system_category()));
        }
        u.mem_      = static_cast<uint8_t*>(mem);
        u.mem_size_ = total;

#ifdef QBUEM_XDP_LIBBPF
        // 2. libbpf로 커널에 UMEM 등록
        xsk_umem_config xsk_cfg{};
        xsk_cfg.fill_size      = cfg.fill_size;
        xsk_cfg.comp_size      = cfg.comp_size;
        xsk_cfg.frame_size     = cfg.frame_size;
        xsk_cfg.frame_headroom = 0;
        xsk_cfg.flags          = 0;

        int ret = xsk_umem__create(&u.umem_, mem, total,
                                   &u.fill_ring_, &u.comp_ring_, &xsk_cfg);
        if (ret != 0) {
            ::munmap(mem, total);
            u.mem_ = static_cast<uint8_t*>(MAP_FAILED);
            return unexpected(std::error_code(-ret, std::system_category()));
        }
#endif
        return u;
    }

    // ── 접근자 ──────────────────────────────────────────────────────────

    /**
     * @brief 프레임 오프셋에 해당하는 실제 데이터 포인터를 반환합니다.
     * @param frame UmemFrame (addr 필드 사용).
     * @returns UMEM 내 데이터 포인터.
     */
    [[nodiscard]] uint8_t* data(const UmemFrame& frame) noexcept {
        return mem_ + frame.addr;
    }

    /** @copydoc data(const UmemFrame&) */
    [[nodiscard]] const uint8_t* data(const UmemFrame& frame) const noexcept {
        return mem_ + frame.addr;
    }

    /** @brief 총 할당된 메모리 크기 (바이트). */
    [[nodiscard]] size_t mem_size() const noexcept { return mem_size_; }

    /** @brief 설정을 반환합니다. */
    [[nodiscard]] const UmemConfig& config() const noexcept { return cfg_; }

    // ── Fill Ring 관리 ───────────────────────────────────────────────────

    /**
     * @brief Fill Ring에 `n`개 프레임을 채워 커널에 수신 준비를 알립니다.
     *
     * 커널은 등록된 주소에 패킷을 기록하고 Rx Ring에 완료를 통보합니다.
     *
     * @param n 채울 프레임 수. 가용 프레임보다 많으면 가용 수만큼 채웁니다.
     * @returns 실제로 채운 프레임 수.
     */
    uint32_t fill_frames(uint32_t n) noexcept {
#ifdef QBUEM_XDP_LIBBPF
        uint32_t idx = 0;
        uint32_t reserved = xsk_ring_prod__reserve(&fill_ring_, n, &idx);
        for (uint32_t i = 0; i < reserved; ++i) {
            *xsk_ring_prod__fill_addr(&fill_ring_, idx++) =
                static_cast<uint64_t>(next_free_frame_++) * cfg_.frame_size;
            if (next_free_frame_ >= cfg_.frame_count) {
                next_free_frame_ = 0; // 순환
            }
        }
        xsk_ring_prod__submit(&fill_ring_, reserved);
        return reserved;
#else
        (void)n;
        return 0;
#endif
    }

    /**
     * @brief Completion Ring에서 전송 완료된 프레임을 회수합니다.
     *
     * `send()` 후 커널이 전송을 완료하면 이 링에 프레임 주소를 기록합니다.
     * 회수한 프레임은 재사용할 수 있습니다.
     *
     * @param frames  회수된 프레임 주소를 저장할 배열.
     * @param max_n   최대 회수 개수.
     * @returns 실제로 회수한 프레임 수.
     */
    uint32_t reclaim_tx(uint64_t* frames, uint32_t max_n) noexcept {
#ifdef QBUEM_XDP_LIBBPF
        uint32_t idx = 0;
        uint32_t completed = xsk_ring_cons__peek(&comp_ring_, max_n, &idx);
        for (uint32_t i = 0; i < completed; ++i) {
            frames[i] = *xsk_ring_cons__comp_addr(&comp_ring_, idx++);
        }
        xsk_ring_cons__release(&comp_ring_, completed);
        return completed;
#else
        (void)frames; (void)max_n;
        return 0;
#endif
    }

#ifdef QBUEM_XDP_LIBBPF
    /** @brief 내부 xsk_umem* 핸들 (libbpf API 직접 사용 시). */
    [[nodiscard]] xsk_umem* handle() noexcept { return umem_; }

    /** @brief Fill Ring (xsk_ring_prod*) 참조. */
    [[nodiscard]] xsk_ring_prod* fill_ring() noexcept { return &fill_ring_; }

    /** @brief Completion Ring (xsk_ring_cons*) 참조. */
    [[nodiscard]] xsk_ring_cons* comp_ring() noexcept { return &comp_ring_; }
#endif

private:
    explicit Umem(const UmemConfig& cfg) noexcept
        : cfg_(cfg)
        , mem_(static_cast<uint8_t*>(MAP_FAILED))
        , mem_size_(0)
#ifdef QBUEM_XDP_LIBBPF
        , umem_(nullptr)
        , fill_ring_{}
        , comp_ring_{}
#endif
        , next_free_frame_(0)
    {}

    void destroy() noexcept {
#ifdef QBUEM_XDP_LIBBPF
        if (umem_) {
            xsk_umem__delete(umem_);
            umem_ = nullptr;
        }
#endif
        if (mem_ != static_cast<uint8_t*>(MAP_FAILED) && mem_size_ > 0) {
            ::munmap(mem_, mem_size_);
            mem_      = static_cast<uint8_t*>(MAP_FAILED);
            mem_size_ = 0;
        }
    }

    UmemConfig  cfg_;
    uint8_t*    mem_;
    size_t      mem_size_;

#ifdef QBUEM_XDP_LIBBPF
    xsk_umem*      umem_;
    xsk_ring_prod  fill_ring_;
    xsk_ring_cons  comp_ring_;
#endif

    uint32_t    next_free_frame_;
};

} // namespace qbuem::xdp

#endif // QBUEM_HAS_XDP

/** @} */ // end of qbuem_xdp
