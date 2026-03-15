#pragma once

/**
 * @file qbuem/pcie/msix_reactor.hpp
 * @brief MSI-X 인터럽트 → Reactor 브리지.
 * @defgroup qbuem_pcie_msix MSIXReactor
 * @ingroup qbuem_embedded
 *
 * ## 개요
 * PCIe MSI-X 인터럽트를 `eventfd`(2) 신호로 변환하여
 * `IReactor`(`epoll`/`io_uring`/`kqueue`)에서 비동기 처리합니다.
 *
 * ## 동작 원리
 * ```
 * PCIe NIC/GPU             kernel VFIO           user-space
 *   MSI-X IRQ ──────────► eventfd increment ──► epoll_wait / io_uring
 *   (하드웨어 인터럽트)      (커널 → user)          (IReactor에서 co_await)
 * ```
 *
 * ## VFIO IRQ 설정 흐름
 * 1. `VFIO_DEVICE_GET_IRQ_INFO`로 MSI-X 벡터 수 확인.
 * 2. eventfd를 벡터별로 생성 (`eventfd(0, EFD_NONBLOCK)`).
 * 3. `VFIO_DEVICE_SET_IRQS`로 eventfd를 MSI-X 벡터에 바인딩.
 * 4. epoll/io_uring에 eventfd 등록 → 인터럽트 발생 시 wakeup.
 *
 * ## 사용 예시
 * @code
 * auto dev = PCIeDevice::open("0000:03:00.0");
 * MSIXReactor msix(*dev, reactor);
 *
 * auto result = msix.setup(32); // 32개 MSI-X 벡터 활성화
 *
 * // 벡터 0에서 인터럽트 대기
 * while (true) {
 *     co_await msix.wait(0); // 인터럽트 발생까지 대기
 *     process_completion_queue();
 * }
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pcie/pcie_device.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#if defined(__linux__)
#  include <sys/eventfd.h>
#  include <sys/epoll.h>
#endif

namespace qbuem::pcie {

// ─── 상수 ─────────────────────────────────────────────────────────────────────

/** @brief 지원하는 최대 MSI-X 벡터 수. */
inline constexpr size_t kMaxMSIXVectors = 2048;

// ─── IRQ 통계 ────────────────────────────────────────────────────────────────

/**
 * @brief 벡터별 IRQ 통계.
 *
 * cache-line 정렬로 false-sharing 방지.
 */
struct alignas(64) VectorStats {
    std::atomic<uint64_t> irq_count{0};    ///< 인터럽트 수신 횟수
    std::atomic<uint64_t> missed{0};        ///< 처리 지연으로 손실된 인터럽트 수
    std::atomic<uint64_t> latency_ns{0};    ///< 마지막 인터럽트 레이턴시 (ns)
};

// ─── MSIXReactor ─────────────────────────────────────────────────────────────

/**
 * @brief PCIe MSI-X 인터럽트 ↔ IReactor 비동기 브리지.
 *
 * ## 설계 원칙
 * - **Zero Thread**: 인터럽트 핸들러는 Reactor 스레드에서 직접 실행.
 * - **Per-vector eventfd**: 벡터마다 독립 eventfd로 IRQ affinity 지원.
 * - **Lock-free dispatch**: IRQ 수신 → co_await 재개가 lock 없이 동작.
 */
class MSIXReactor {
public:
    /**
     * @brief MSIXReactor를 생성합니다.
     *
     * @param dev     VFIO PCIe 디바이스 핸들.
     * @param reactor 이벤트를 dispatch할 Reactor.
     */
    MSIXReactor(PCIeDevice& dev, Reactor& reactor) noexcept;
    ~MSIXReactor();

    MSIXReactor(const MSIXReactor&) = delete;
    MSIXReactor& operator=(const MSIXReactor&) = delete;

    /**
     * @brief MSI-X 벡터를 활성화하고 eventfd를 바인딩합니다.
     *
     * @param num_vectors 활성화할 벡터 수 (1 ~ kMaxMSIXVectors).
     * @returns 성공 시 `Result<void>`.
     *
     * @note 한 번만 호출해야 합니다. 중복 호출 시 에러.
     */
    [[nodiscard]] Result<void> setup(size_t num_vectors) noexcept;

    /**
     * @brief 특정 MSI-X 벡터에서 인터럽트가 발생할 때까지 co_await 대기합니다.
     *
     * epoll/io_uring으로 eventfd를 모니터링합니다.
     * 인터럽트 발생 시 eventfd 카운터를 읽어 소비합니다.
     *
     * @param vector_idx MSI-X 벡터 인덱스 (0 ~ num_vectors-1).
     * @returns 소비된 IRQ 카운트 (1 이상).
     */
    Task<uint64_t> wait(size_t vector_idx) noexcept;

    /**
     * @brief 인터럽트를 논블로킹으로 소비합니다.
     *
     * @param vector_idx 벡터 인덱스.
     * @returns IRQ 카운트, 또는 0 (IRQ 없음).
     */
    uint64_t try_consume(size_t vector_idx) noexcept;

    /**
     * @brief 특정 벡터의 IRQ 마스킹 (hardware mask).
     *
     * `VFIO_DEVICE_SET_IRQS`의 MASK 액션으로 하드웨어 수준에서 인터럽트 차단.
     *
     * @param vector_idx 벡터 인덱스.
     */
    [[nodiscard]] Result<void> mask(size_t vector_idx) noexcept;

    /**
     * @brief 특정 벡터의 IRQ 마스킹 해제.
     */
    [[nodiscard]] Result<void> unmask(size_t vector_idx) noexcept;

    /** @brief 활성화된 벡터 수. */
    [[nodiscard]] size_t num_vectors() const noexcept { return num_vectors_; }

    /** @brief 벡터 통계 참조. */
    [[nodiscard]] const VectorStats& stats(size_t vec) const noexcept {
        return stats_[vec < kMaxMSIXVectors ? vec : 0];
    }

    /** @brief 전체 IRQ 수신 횟수 합계. */
    [[nodiscard]] uint64_t total_irqs() const noexcept;

private:
    /**
     * @brief Reactor 이벤트 루프에 eventfd를 등록합니다.
     *
     * `epoll_ctl(EPOLL_CTL_ADD, EPOLLIN | EPOLLET)` 또는
     * `io_uring` POLL_ADD SQE를 사용합니다.
     */
    Result<void> register_eventfd(size_t vec_idx, int efd) noexcept;

    /**
     * @brief VFIO SET_IRQS ioctl로 eventfd를 MSI-X 벡터에 바인딩합니다.
     */
    Result<void> bind_eventfds_to_device(size_t num_vectors) noexcept;

    PCIeDevice& dev_;
    Reactor&    reactor_;
    size_t      num_vectors_{0};

    // 벡터별 eventfd 배열 (동적 할당, 최대 kMaxMSIXVectors)
    int*        eventfds_{nullptr};   // [num_vectors_]

    // 벡터별 통계
    VectorStats stats_[kMaxMSIXVectors];

    std::atomic<bool> initialized_{false};
};

// ─── IRQ Affinity 헬퍼 ───────────────────────────────────────────────────────

/**
 * @brief MSI-X 벡터를 특정 CPU 코어의 Reactor에 바인딩합니다.
 *
 * `/proc/irq/<irq_num>/smp_affinity`를 수정하여 특정 CPU에서만
 * 하드웨어 인터럽트를 처리합니다.
 *
 * @param irq_num   Linux IRQ 번호 (`/proc/interrupts`에서 확인).
 * @param cpu_mask  CPU 비트마스크 (e.g. 0x1 = CPU 0, 0x3 = CPU 0,1).
 * @returns 성공 시 `Result<void>`.
 */
[[nodiscard]] Result<void> set_irq_affinity(int irq_num,
                                              uint64_t cpu_mask) noexcept;

/**
 * @brief MSI-X 벡터 번호에서 Linux IRQ 번호를 조회합니다.
 *
 * `/sys/bus/pci/devices/<BDF>/msi_irqs/`를 스캔합니다.
 *
 * @param bdf       PCI BDF 주소.
 * @param vec_idx   MSI-X 벡터 인덱스.
 * @returns Linux IRQ 번호 또는 에러.
 */
[[nodiscard]] Result<int> get_msix_irq_number(std::string_view bdf,
                                               size_t vec_idx) noexcept;

} // namespace qbuem::pcie

/** @} */
