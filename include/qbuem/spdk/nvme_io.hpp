#pragma once

/**
 * @file qbuem/spdk/nvme_io.hpp
 * @brief io_uring passthrough 기반 user-space NVMe 직접 I/O.
 * @defgroup qbuem_spdk NVMeIO
 * @ingroup qbuem_storage
 *
 * ## 개요
 * Linux `io_uring IORING_OP_URING_CMD`(io_uring passthrough)와
 * NVMe Character Device(`/dev/ngXnY`)를 통해 user-space에서
 * NVMe 명령을 직접 발행합니다.
 *
 * ## 기술 배경
 * ### io_uring NVMe Passthrough (Linux 6.0+)
 * ```
 * IORING_OP_URING_CMD
 *   └─ NVMe admin/IO command
 *        └─ NVMe 컨트롤러 큐에 직접 제출
 *             └─ completion event → CQE → co_await 재개
 * ```
 *
 * ### SPDK(Storage Performance Development Kit) 비교
 * | 방식 | 레이턴시 | 의존성 | 설명 |
 * |------|---------|-------|------|
 * | io_uring passthrough | ~4µs | 없음 | 커널 NVMe 드라이버 활용 |
 * | SPDK UIO | ~2µs | SPDK lib | DPDK 스타일 완전 user-space |
 * | 블록 레이어 (AIO) | ~20µs | 없음 | 커널 스케줄러 개입 |
 *
 * 이 헤더는 io_uring passthrough를 기본으로 하며,
 * SPDK UIO 백엔드를 플러그인으로 교체할 수 있는 인터페이스를 제공합니다.
 *
 * ## DMA 버퍼
 * NVMe I/O는 물리적으로 연속된 DMA 버퍼가 필요합니다.
 * `NVMeIOContext::alloc_dma()`로 적절히 정렬된 버퍼를 할당합니다.
 *
 * @code
 * auto ctx = NVMeIOContext::open("/dev/ng0n1");
 * auto dma  = ctx->alloc_dma(4096); // 4KB DMA 버퍼
 *
 * // 섹터 0 읽기 (LBA 0, 1개 섹터 = 512B 또는 4096B)
 * auto result = co_await ctx->read(dma.get(), 0, 1);
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace qbuem::spdk {

// ─── NVMe 명령 결과 ───────────────────────────────────────────────────────────

/**
 * @brief NVMe 명령 완료 상태.
 *
 * NVMe 스펙 섹션 4.6 (Completion Queue Entry) 기반.
 */
struct NVMeResult {
    uint32_t result{0};    ///< 명령별 결과 DW0
    uint16_t sq_id{0};     ///< 제출 큐 ID
    uint16_t cmd_id{0};    ///< 명령 ID
    uint16_t status{0};    ///< Status Field (bit 14:1)

    /** @brief 성공 여부 (Status Code == 0). */
    [[nodiscard]] bool ok() const noexcept { return (status >> 1) == 0; }

    /** @brief NVMe Status Code Type. */
    [[nodiscard]] uint8_t sct() const noexcept {
        return static_cast<uint8_t>((status >> 9) & 0x7);
    }

    /** @brief NVMe Status Code. */
    [[nodiscard]] uint8_t sc() const noexcept {
        return static_cast<uint8_t>((status >> 1) & 0xFF);
    }
};

// ─── DMA 버퍼 ────────────────────────────────────────────────────────────────

/**
 * @brief NVMe I/O용 DMA 정렬 버퍼 (RAII).
 *
 * `mmap(MAP_HUGETLB)` 또는 `posix_memalign(4096)` 으로 할당됩니다.
 * NVMe 컨트롤러는 기본적으로 4KB 정렬 버퍼를 요구합니다.
 */
class DMABuffer {
public:
    virtual ~DMABuffer() = default;

    /** @brief 버퍼 user-space 주소. */
    [[nodiscard]] virtual void* data() noexcept = 0;
    [[nodiscard]] virtual const void* data() const noexcept = 0;

    /** @brief 버퍼 크기 (bytes). */
    [[nodiscard]] virtual size_t size() const noexcept = 0;

    /** @brief 버퍼를 0으로 초기화합니다. */
    virtual void zero() noexcept = 0;

    /** @brief byte span 뷰. */
    [[nodiscard]] MutableBufferView span() noexcept {
        return {static_cast<uint8_t*>(data()), size()};
    }
    [[nodiscard]] BufferView span() const noexcept {
        return {static_cast<const uint8_t*>(data()), size()};
    }
};

// ─── 디바이스 정보 ───────────────────────────────────────────────────────────

/**
 * @brief NVMe 네임스페이스/컨트롤러 정보.
 */
struct NVMeDeviceInfo {
    uint64_t  ns_size{0};       ///< 네임스페이스 크기 (LBA 수)
    uint32_t  lba_size{512};    ///< LBA 크기 (bytes, 보통 512 or 4096)
    uint32_t  optimal_io_size{0}; ///< 최적 I/O 크기 (bytes, 0이면 제한 없음)
    uint32_t  max_sectors{0};   ///< 단일 명령 최대 섹터 수
    uint16_t  ctrl_id{0};       ///< 컨트롤러 ID
    uint8_t   ns_id{1};         ///< 네임스페이스 ID
    bool      volatile_write_cache{false}; ///< Write-back 캐시 유무

    /** @brief 총 용량 (bytes). */
    [[nodiscard]] uint64_t total_bytes() const noexcept {
        return ns_size * lba_size;
    }
};

// ─── I/O 통계 ─────────────────────────────────────────────────────────────────

/**
 * @brief NVMe I/O 성능 통계 (per-context).
 */
struct NVMeStats {
    std::atomic<uint64_t> read_ops{0};      ///< 완료된 읽기 작업 수
    std::atomic<uint64_t> write_ops{0};     ///< 완료된 쓰기 작업 수
    std::atomic<uint64_t> read_bytes{0};    ///< 읽은 총 바이트 수
    std::atomic<uint64_t> write_bytes{0};   ///< 쓴 총 바이트 수
    std::atomic<uint64_t> errors{0};        ///< 에러 수
    std::atomic<uint64_t> avg_latency_ns{0};///< 평균 레이턴시 (ns, 지수 이동 평균)
};

// ─── NVMeIOContext ────────────────────────────────────────────────────────────

/**
 * @brief NVMe 직접 I/O 컨텍스트.
 *
 * io_uring passthrough(`IORING_OP_URING_CMD`)를 사용하여
 * 커널 블록 레이어를 우회하고 NVMe 큐에 직접 명령을 제출합니다.
 *
 * ## 전제 조건
 * - Linux 6.0+ (`CONFIG_IO_URING_URING_CMD` 활성화)
 * - `/dev/ngXnY` 디바이스 읽기/쓰기 권한 (`CAP_SYS_RAWIO` 또는 ACL)
 * - NVMe 드라이버 로드 (`modprobe nvme`)
 *
 * ## 동시성 모델
 * 동일한 `NVMeIOContext`를 여러 코루틴이 공유할 수 있습니다.
 * 내부적으로 io_uring SQ/CQ 잠금으로 동시 제출을 직렬화합니다.
 * 성능 최적화를 위해 코어당 독립 `NVMeIOContext`를 사용하세요.
 */
class NVMeIOContext {
public:
    /**
     * @brief NVMe Character Device를 엽니다.
     *
     * @param dev_path   디바이스 경로 (e.g. "/dev/ng0n1").
     * @param queue_depth io_uring 큐 깊이 (기본: 256).
     * @returns 컨텍스트 또는 에러.
     */
    static Result<std::unique_ptr<NVMeIOContext>> open(
        std::string_view dev_path, uint32_t queue_depth = 256) noexcept;

    NVMeIOContext() = default;
    NVMeIOContext(const NVMeIOContext&) = delete;
    NVMeIOContext& operator=(const NVMeIOContext&) = delete;
    virtual ~NVMeIOContext() = default;

    // ── DMA 버퍼 할당 ──────────────────────────────────────────────────────

    /**
     * @brief NVMe I/O용 DMA 정렬 버퍼를 할당합니다.
     *
     * @param size  바이트 크기 (4KB 정렬로 올림).
     * @returns DMABuffer 또는 에러.
     */
    [[nodiscard]] virtual Result<std::unique_ptr<DMABuffer>>
    alloc_dma(size_t size) noexcept = 0;

    // ── NVMe I/O ───────────────────────────────────────────────────────────

    /**
     * @brief NVMe 읽기 (io_uring passthrough).
     *
     * @param buf    DMA 버퍼 (alloc_dma()로 할당).
     * @param lba    시작 LBA.
     * @param nsects 읽을 섹터 수.
     * @returns NVMe 완료 결과.
     */
    virtual Task<Result<NVMeResult>> read(DMABuffer& buf,
                                           uint64_t   lba,
                                           uint32_t   nsects) noexcept = 0;

    /**
     * @brief NVMe 쓰기 (io_uring passthrough).
     *
     * @param buf    DMA 버퍼.
     * @param lba    시작 LBA.
     * @param nsects 쓸 섹터 수.
     * @returns NVMe 완료 결과.
     */
    virtual Task<Result<NVMeResult>> write(const DMABuffer& buf,
                                            uint64_t         lba,
                                            uint32_t         nsects) noexcept = 0;

    /**
     * @brief NVMe Flush (Write Cache Sync).
     *
     * `FUA(Force Unit Access)` 또는 Flush 명령으로 데이터를 비휘발성 스토리지에 동기화합니다.
     */
    virtual Task<Result<NVMeResult>> flush() noexcept = 0;

    /**
     * @brief NVMe Dataset Management (TRIM / DEALLOCATE).
     *
     * @param lba    시작 LBA.
     * @param nsects 섹터 수.
     */
    virtual Task<Result<NVMeResult>> trim(uint64_t lba,
                                           uint32_t nsects) noexcept = 0;

    // ── 벡터 I/O ───────────────────────────────────────────────────────────

    /**
     * @brief 여러 LBA 범위를 한 번에 읽습니다 (scatter-read).
     *
     * 내부적으로 io_uring linked SQE 체인으로 발행합니다.
     *
     * @param ranges {buf, lba, nsects} 범위 배열.
     * @returns 각 범위의 완료 결과.
     */
    struct IORange { DMABuffer* buf; uint64_t lba; uint32_t nsects; };
    virtual Task<Result<size_t>> read_scatter(std::span<IORange> ranges) noexcept = 0;

    // ── 디바이스 정보 ──────────────────────────────────────────────────────

    /** @brief 디바이스 정보 (LBA 크기, 용량 등). */
    [[nodiscard]] virtual const NVMeDeviceInfo& device_info() const noexcept = 0;

    /** @brief I/O 통계 참조. */
    [[nodiscard]] virtual const NVMeStats& stats() const noexcept = 0;

    /** @brief 디바이스 경로. */
    [[nodiscard]] virtual std::string_view device_path() const noexcept = 0;

    // ── Admin 명령 ─────────────────────────────────────────────────────────

    /**
     * @brief NVMe Identify Controller 조회.
     *
     * @param out 결과를 저장할 4096B DMA 버퍼.
     * @returns NVMe 완료 결과.
     */
    virtual Task<Result<NVMeResult>> identify_controller(DMABuffer& out) noexcept = 0;

    /**
     * @brief NVMe Get Log Page (SMART/Health Information).
     *
     * @param out  결과 버퍼.
     * @param lid  Log Page ID (e.g. 0x02 = SMART).
     * @returns NVMe 완료 결과.
     */
    virtual Task<Result<NVMeResult>> get_log_page(DMABuffer& out,
                                                   uint8_t lid) noexcept = 0;
};

} // namespace qbuem::spdk

/** @} */
