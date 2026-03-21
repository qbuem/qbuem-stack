#pragma once

/**
 * @file qbuem/pcie/pcie_device.hpp
 * @brief qbuem::PCIeDevice — Linux VFIO-based user-space PCIe control.
 * @defgroup qbuem_pcie PCIeDevice
 * @ingroup qbuem_embedded
 *
 * ## Overview
 * `PCIeDevice` controls a PCIe device directly from user-space via the Linux
 * VFIO (Virtual Function I/O) framework.
 *
 * ## VFIO hierarchy
 * ```
 * /dev/vfio/vfio         ← VFIO container (IOMMU domain)
 *   └─ /dev/vfio/<group> ← VFIO group (IOMMU group)
 *        └─ device fd    ← VFIO device (PCIe function)
 * ```
 *
 * ## Usage flow
 * ```
 * 1. Find the IOMMU group number:
 *    ls -la /sys/bus/pci/devices/<BDF>/iommu_group → ../../../kernel/iommu_groups/<N>
 * 2. Bind the device to the VFIO driver:
 *    echo <BDF> > /sys/bus/pci/drivers/<driver>/unbind
 *    echo <vendor>:<device> > /sys/bus/pci/drivers/vfio-pci/new_id
 * 3. Open with PCIeDevice::open("0000:03:00.0")
 * 4. Map the MMIO region with map_bar(0)
 * 5. Access registers with read_mmio32/write_mmio32
 * ```
 *
 * ## Safe DMA usage
 * `alloc_dma_buffer()` allocates contiguous physical memory through the IOMMU.
 * The `iova` (I/O Virtual Address) is used by the device for DMA transfers.
 *
 * @code
 * auto dev = PCIeDevice::open("0000:03:00.0");
 * if (!dev) return;
 * auto bar0 = dev->map_bar(0);
 * uint32_t status = dev->read_mmio32(0x00);
 * dev->write_mmio32(0x04, 0x1);
 *
 * auto dma = dev->alloc_dma_buffer(4096);
 * // dma.vaddr: user-space address, dma.iova: device DMA address
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

// Linux VFIO headers
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

// ─── BAR (Base Address Register) mapping ─────────────────────────────────────

/**
 * @brief PCIe BAR mapping result (RAII).
 *
 * Automatically released via `munmap()` on destruction.
 */
struct BarMapping {
    void*    vaddr{nullptr};  ///< User-space virtual address
    size_t   size{0};         ///< Mapping size (bytes)
    uint8_t  bar_idx{0};      ///< BAR index (0-5)

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

    /** @brief 32-bit register read (MMIO volatile). */
    [[nodiscard]] uint32_t read32(size_t offset) const noexcept {
        return *reinterpret_cast<const volatile uint32_t*>(
            static_cast<const uint8_t*>(vaddr) + offset);
    }
    /** @brief 64-bit register read. */
    [[nodiscard]] uint64_t read64(size_t offset) const noexcept {
        return *reinterpret_cast<const volatile uint64_t*>(
            static_cast<const uint8_t*>(vaddr) + offset);
    }
    /** @brief 32-bit register write (MMIO volatile). */
    void write32(size_t offset, uint32_t val) const noexcept {
        *reinterpret_cast<volatile uint32_t*>(
            static_cast<uint8_t*>(vaddr) + offset) = val;
    }
    /** @brief 64-bit register write. */
    void write64(size_t offset, uint64_t val) const noexcept {
        *reinterpret_cast<volatile uint64_t*>(
            static_cast<uint8_t*>(vaddr) + offset) = val;
    }

private:
    void unmap() noexcept {
#if defined(__linux__)
        if (vaddr != nullptr && size != 0u) ::munmap(vaddr, size);
#endif
        vaddr = nullptr; size = 0;
    }
};

// ─── DMA buffer ───────────────────────────────────────────────────────────────

/**
 * @brief IOMMU-mapped contiguous DMA buffer (RAII).
 *
 * - `vaddr`: virtual address for user-space reads and writes
 * - `iova`:  I/O virtual address used by the device for DMA transfers
 */
struct DmaBuffer {
    void*    vaddr{nullptr}; ///< User-space virtual address
    uint64_t iova{0};        ///< IOMMU I/O virtual address (for device DMA)
    size_t   size{0};        ///< Buffer size (bytes)
    int      container_fd{-1}; ///< VFIO container fd (used for release)

    DmaBuffer() = default;
    DmaBuffer(const DmaBuffer&) = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;
    DmaBuffer(DmaBuffer&& o) noexcept
        : vaddr(o.vaddr), iova(o.iova), size(o.size), container_fd(o.container_fd) {
        o.vaddr = nullptr; o.size = 0; o.container_fd = -1;
    }
    ~DmaBuffer() { free(); }

    [[nodiscard]] bool valid() const noexcept { return vaddr != nullptr; }

    /** @brief Zero-initializes the DMA buffer. */
    void zero() const noexcept {
        if (vaddr != nullptr && size != 0u) __builtin_memset(vaddr, 0, size);
    }

private:
    void free() noexcept {
        if (vaddr != nullptr && size != 0u) {
#if defined(__linux__)
            ::munmap(vaddr, size);
#endif
            vaddr = nullptr;
            size  = 0;
        }
    }
};

// ─── PCIeDevice ──────────────────────────────────────────────────────────────

/**
 * @brief Linux VFIO-based user-space PCIe device handle.
 *
 * ## Thread safety
 * `map_bar()` and `alloc_dma_buffer()` should only be called during initialization.
 * Concurrent MMIO reads and writes must be synchronized by the caller.
 */
class PCIeDevice {
public:
    /** @brief PCI BDF address format (e.g. "0000:03:00.0"). */
    using BDF = std::string_view;

    /**
     * @brief Opens a PCIe device via VFIO.
     *
     * @param bdf  PCI BDF address (e.g. "0000:03:00.0").
     * @returns Device on success, error on failure.
     *
     * @pre The device must be bound to the `vfio-pci` driver.
     * @pre The calling process must have access to `/dev/vfio/vfio`.
     */
    static Result<std::unique_ptr<PCIeDevice>> open(BDF bdf) noexcept;

    ~PCIeDevice();

    PCIeDevice(const PCIeDevice&) = delete;
    PCIeDevice& operator=(const PCIeDevice&) = delete;

    // ── MMIO (BAR) access ──────────────────────────────────────────────────

    /**
     * @brief Maps a BAR region as MMIO.
     *
     * @param bar_idx BAR index (0–5).
     * @returns BarMapping on success, error on failure.
     */
    [[nodiscard]] Result<BarMapping> map_bar(uint8_t bar_idx) noexcept;

    /**
     * @brief Direct 32-bit MMIO read through BAR 0 (without calling map_bar).
     *
     * @param offset Byte offset from BAR 0 base.
     * @returns Register value.
     */
    [[nodiscard]] uint32_t read_mmio32(size_t offset) const noexcept;

    /**
     * @brief Direct 32-bit MMIO write through BAR 0.
     *
     * @param offset Byte offset from BAR 0 base.
     * @param val    Value to write.
     */
    void write_mmio32(size_t offset, uint32_t val) noexcept;

    // ── DMA ───────────────────────────────────────────────────────────────

    /**
     * @brief Allocates an IOMMU-mapped DMA buffer.
     *
     * Internally uses `mmap(MAP_SHARED|MAP_ANONYMOUS)` + `VFIO_IOMMU_MAP_DMA`.
     * The `iova` is written into the device BARx descriptor ring.
     *
     * @param size       Buffer size (bytes, rounded up to page boundary).
     * @param iova_hint  Preferred IOVA (0 = auto-assign).
     * @returns Allocated DmaBuffer.
     */
    [[nodiscard]] Result<DmaBuffer> alloc_dma_buffer(
        size_t size, uint64_t iova_hint = 0) noexcept;

    // ── Device information ─────────────────────────────────────────────────

    /** @brief PCI Vendor ID. */
    [[nodiscard]] uint16_t vendor_id() const noexcept { return vendor_id_; }

    /** @brief PCI Device ID. */
    [[nodiscard]] uint16_t device_id() const noexcept { return device_id_; }

    /** @brief IOMMU group number. */
    [[nodiscard]] int iommu_group() const noexcept { return iommu_group_; }

    /** @brief VFIO device fd (internal use). */
    [[nodiscard]] int device_fd() const noexcept { return device_fd_; }

    /** @brief BDF address string. */
    [[nodiscard]] std::string_view bdf() const noexcept { return bdf_.data(); }

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
    std::array<char, 16> bdf_{};

    uint16_t vendor_id_{0};
    uint16_t device_id_{0};

    // Cached BAR 0 direct-access pointers (set after map_bar(0))
    void*  bar0_vaddr_{nullptr};
    size_t bar0_size_{0};

    // IOVA allocation counter (simple monotonic increment, 4 GB base)
    std::atomic<uint64_t> next_iova_{0x100000000ULL}; // 4GB base
};

// ─── PCIe utilities ───────────────────────────────────────────────────────────

/**
 * @brief Enumerates PCIe devices installed in the system.
 *
 * Scans `/sys/bus/pci/devices/` and returns a list of BDF strings.
 *
 * @tparam N Maximum number of devices to enumerate.
 * @param out  Output array of BDF strings (char[16][N]).
 * @returns Number of devices found.
 */
template <size_t N = 64>
size_t enumerate_pcie_devices(std::array<std::array<char, 16>, N>& out) noexcept;

/**
 * @brief Converts a PCI BDF to an IOMMU group number.
 *
 * @param bdf PCI BDF address.
 * @returns IOMMU group number or an error.
 */
[[nodiscard]] Result<int> bdf_to_iommu_group(std::string_view bdf) noexcept;

} // namespace qbuem::pcie

/** @} */
