#pragma once

/**
 * @file qbuem/pcie/pcie_device.hpp
 * @brief qbuem::PCIeDevice — Linux VFIO 기반 user-space PCIe 제어.
 * @defgroup qbuem_pcie PCIeDevice
 * @ingroup qbuem_embedded
 *
 * ## 개요
 * `PCIeDevice`는 Linux VFIO(Virtual Function I/O) 프레임워크를 통해
 * user-space에서 PCIe 디바이스를 직접 제어합니다.
 *
 * ## VFIO 계층 구조
 * ```
 * /dev/vfio/vfio         ← VFIO 컨테이너 (IOMMU 도메인)
 *   └─ /dev/vfio/<group> ← VFIO 그룹 (IOMMU 그룹)
 *        └─ 디바이스 fd  ← VFIO 디바이스 (PCIe 함수)
 * ```
 *
 * ## 사용 흐름
 * ```
 * 1. IOMMU 그룹 번호 확인:
 *    ls -la /sys/bus/pci/devices/<BDF>/iommu_group → ../../../kernel/iommu_groups/<N>
 * 2. 디바이스를 VFIO 드라이버에 바인딩:
 *    echo <BDF> > /sys/bus/pci/drivers/<driver>/unbind
 *    echo <vendor>:<device> > /sys/bus/pci/drivers/vfio-pci/new_id
 * 3. PCIeDevice::open("0000:03:00.0")으로 열기
 * 4. map_bar(0)으로 MMIO 영역 매핑
 * 5. read_mmio32/write_mmio32으로 레지스터 접근
 * ```
 *
 * ## DMA 안전 사용
 * `alloc_dma_buffer()`는 IOMMU를 통해 연속 물리 메모리를 할당합니다.
 * `iova`(I/O Virtual Address)는 디바이스가 DMA 전송에 사용합니다.
 *
 * @code
 * auto dev = PCIeDevice::open("0000:03:00.0");
 * if (!dev) return;
 * auto bar0 = dev->map_bar(0);
 * uint32_t status = dev->read_mmio32(0x00);
 * dev->write_mmio32(0x04, 0x1);
 *
 * auto dma = dev->alloc_dma_buffer(4096);
 * // dma.vaddr: user-space 주소, dma.iova: 디바이스 DMA 주소
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

// Linux VFIO 헤더
#if defined(__linux__)
#  include <sys/ioctl.h>
#  include <sys/mman.h>
#  include <fcntl.h>
#  include <unistd.h>
#  if __has_include(<linux/vfio.h>)
#    include <linux/vfio.h>
#    define QBUEM_HAS_VFIO 1
#  endif
#endif

namespace qbuem::pcie {

// ─── BAR (Base Address Register) 매핑 ────────────────────────────────────────

/**
 * @brief PCIe BAR 매핑 결과 (RAII).
 *
 * 소멸 시 `munmap()`으로 자동 해제됩니다.
 */
struct BarMapping {
    void*    vaddr{nullptr};  ///< user-space 가상 주소
    size_t   size{0};         ///< 매핑 크기 (bytes)
    uint8_t  bar_idx{0};      ///< BAR 인덱스 (0-5)

    BarMapping() = default;
    BarMapping(const BarMapping&) = delete;
    BarMapping& operator=(const BarMapping&) = delete;

    BarMapping(BarMapping&& o) noexcept
        : vaddr(o.vaddr), size(o.size), bar_idx(o.bar_idx) {
        o.vaddr = nullptr; o.size = 0;
    }
    BarMapping& operator=(BarMapping&& o) noexcept {
        if (this != &o) {
            unmap();
            vaddr = o.vaddr; size = o.size; bar_idx = o.bar_idx;
            o.vaddr = nullptr; o.size = 0;
        }
        return *this;
    }
    ~BarMapping() { unmap(); }

    [[nodiscard]] bool valid() const noexcept { return vaddr != nullptr; }

    /** @brief 32-bit 레지스터 읽기 (MMIO volatile). */
    [[nodiscard]] uint32_t read32(size_t offset) const noexcept {
        return *reinterpret_cast<const volatile uint32_t*>(
            static_cast<const uint8_t*>(vaddr) + offset);
    }
    /** @brief 64-bit 레지스터 읽기. */
    [[nodiscard]] uint64_t read64(size_t offset) const noexcept {
        return *reinterpret_cast<const volatile uint64_t*>(
            static_cast<const uint8_t*>(vaddr) + offset);
    }
    /** @brief 32-bit 레지스터 쓰기 (MMIO volatile). */
    void write32(size_t offset, uint32_t val) noexcept {
        *reinterpret_cast<volatile uint32_t*>(
            static_cast<uint8_t*>(vaddr) + offset) = val;
    }
    /** @brief 64-bit 레지스터 쓰기. */
    void write64(size_t offset, uint64_t val) noexcept {
        *reinterpret_cast<volatile uint64_t*>(
            static_cast<uint8_t*>(vaddr) + offset) = val;
    }

private:
    void unmap() noexcept {
#if defined(__linux__)
        if (vaddr && size) ::munmap(vaddr, size);
#endif
        vaddr = nullptr; size = 0;
    }
};

// ─── DMA 버퍼 ─────────────────────────────────────────────────────────────────

/**
 * @brief IOMMU-매핑된 DMA 연속 버퍼 (RAII).
 *
 * - `vaddr`: user-space에서 읽기/쓰기용 가상 주소
 * - `iova`:  디바이스가 DMA 전송 시 사용하는 I/O 가상 주소
 */
struct DmaBuffer {
    void*    vaddr{nullptr}; ///< user-space 가상 주소
    uint64_t iova{0};        ///< IOMMU I/O 가상 주소 (디바이스 DMA용)
    size_t   size{0};        ///< 버퍼 크기 (bytes)
    int      container_fd{-1}; ///< VFIO 컨테이너 fd (해제용)

    DmaBuffer() = default;
    DmaBuffer(const DmaBuffer&) = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;
    DmaBuffer(DmaBuffer&& o) noexcept
        : vaddr(o.vaddr), iova(o.iova), size(o.size), container_fd(o.container_fd) {
        o.vaddr = nullptr; o.size = 0; o.container_fd = -1;
    }
    ~DmaBuffer() { free(); }

    [[nodiscard]] bool valid() const noexcept { return vaddr != nullptr; }

    /** @brief DMA 버퍼를 0으로 초기화합니다. */
    void zero() noexcept {
        if (vaddr && size) __builtin_memset(vaddr, 0, size);
    }

private:
    void free() noexcept;
};

// ─── PCIeDevice ──────────────────────────────────────────────────────────────

/**
 * @brief Linux VFIO 기반 user-space PCIe 디바이스 핸들.
 *
 * ## thread-safety
 * `map_bar()`, `alloc_dma_buffer()`는 초기화 단계에서만 호출하세요.
 * MMIO 읽기/쓰기는 동시 접근 시 호출자가 동기화해야 합니다.
 */
class PCIeDevice {
public:
    /** @brief PCI BDF 주소 형식 (e.g. "0000:03:00.0"). */
    using BDF = std::string_view;

    /**
     * @brief VFIO를 통해 PCIe 디바이스를 엽니다.
     *
     * @param bdf  PCI BDF 주소 (e.g. "0000:03:00.0").
     * @returns 성공 시 디바이스, 실패 시 에러.
     *
     * @pre 디바이스가 `vfio-pci` 드라이버에 바인딩되어 있어야 합니다.
     * @pre 실행 프로세스가 `/dev/vfio/vfio` 접근 권한을 가져야 합니다.
     */
    static Result<std::unique_ptr<PCIeDevice>> open(BDF bdf) noexcept;

    ~PCIeDevice();

    PCIeDevice(const PCIeDevice&) = delete;
    PCIeDevice& operator=(const PCIeDevice&) = delete;

    // ── MMIO (BAR) 접근 ────────────────────────────────────────────────────

    /**
     * @brief BAR 영역을 MMIO로 매핑합니다.
     *
     * @param bar_idx BAR 인덱스 (0–5).
     * @returns 성공 시 BarMapping, 실패 시 에러.
     */
    [[nodiscard]] Result<BarMapping> map_bar(uint8_t bar_idx) noexcept;

    /**
     * @brief BAR 0을 통한 직접 32-bit MMIO 읽기 (map_bar 없이).
     *
     * @param offset BAR 0 기준 바이트 오프셋.
     * @returns 레지스터 값.
     */
    [[nodiscard]] uint32_t read_mmio32(size_t offset) const noexcept;

    /**
     * @brief BAR 0을 통한 직접 32-bit MMIO 쓰기.
     *
     * @param offset BAR 0 기준 바이트 오프셋.
     * @param val    쓸 값.
     */
    void write_mmio32(size_t offset, uint32_t val) noexcept;

    // ── DMA ───────────────────────────────────────────────────────────────

    /**
     * @brief IOMMU-매핑된 DMA 버퍼를 할당합니다.
     *
     * 내부적으로 `mmap(MAP_SHARED|MAP_ANONYMOUS)` + `VFIO_IOMMU_MAP_DMA`를 사용합니다.
     * `iova`는 디바이스 BARx 서술자 링에 기록합니다.
     *
     * @param size  버퍼 크기 (bytes, 페이지 경계로 올림).
     * @param iova_hint 선호 IOVA (0이면 자동 할당).
     * @returns 할당된 DmaBuffer.
     */
    [[nodiscard]] Result<DmaBuffer> alloc_dma_buffer(
        size_t size, uint64_t iova_hint = 0) noexcept;

    // ── 디바이스 정보 ──────────────────────────────────────────────────────

    /** @brief PCI Vendor ID. */
    [[nodiscard]] uint16_t vendor_id() const noexcept { return vendor_id_; }

    /** @brief PCI Device ID. */
    [[nodiscard]] uint16_t device_id() const noexcept { return device_id_; }

    /** @brief IOMMU 그룹 번호. */
    [[nodiscard]] int iommu_group() const noexcept { return iommu_group_; }

    /** @brief VFIO 디바이스 fd (내부용). */
    [[nodiscard]] int device_fd() const noexcept { return device_fd_; }

    /** @brief BDF 주소 문자열. */
    [[nodiscard]] std::string_view bdf() const noexcept { return bdf_; }

private:
    explicit PCIeDevice(int container_fd, int group_fd, int device_fd,
                         int iommu_group, std::string_view bdf) noexcept;

    static Result<int> find_iommu_group(BDF bdf) noexcept;
    static Result<int> open_vfio_container() noexcept;
    static Result<int> open_vfio_group(int container_fd, int group_num) noexcept;

    int  container_fd_{-1};
    int  group_fd_{-1};
    int  device_fd_{-1};
    int  iommu_group_{-1};
    char bdf_[16]{};

    uint16_t vendor_id_{0};
    uint16_t device_id_{0};

    // BAR 0 직접 접근용 캐시 (map_bar(0) 호출 후 설정)
    void*  bar0_vaddr_{nullptr};
    size_t bar0_size_{0};

    // IOVA 할당 카운터 (단순 증가 방식, 4GB 기준)
    std::atomic<uint64_t> next_iova_{0x100000000ULL}; // 4GB base
};

// ─── PCIe 유틸리티 ───────────────────────────────────────────────────────────

/**
 * @brief 시스템에 설치된 PCIe 디바이스 목록을 열거합니다.
 *
 * `/sys/bus/pci/devices/`를 스캔하여 BDF 목록을 반환합니다.
 *
 * @tparam N 최대 열거 개수.
 * @param out  BDF 문자열 출력 배열 (char[16][N]).
 * @returns 발견된 디바이스 수.
 */
template <size_t N = 64>
size_t enumerate_pcie_devices(char (&out)[N][16]) noexcept;

/**
 * @brief PCI BDF를 IOMMU 그룹 번호로 변환합니다.
 *
 * @param bdf PCI BDF 주소.
 * @returns IOMMU 그룹 번호 또는 에러.
 */
[[nodiscard]] Result<int> bdf_to_iommu_group(std::string_view bdf) noexcept;

} // namespace qbuem::pcie

/** @} */
