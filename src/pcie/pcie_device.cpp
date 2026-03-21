/**
 * @file src/pcie/pcie_device.cpp
 * @brief PCIeDevice VFIO implementation — Linux user-space PCIe control.
 *
 * Provides the out-of-line implementation for qbuem::pcie::PCIeDevice declared
 * in <qbuem/pcie/pcie_device.hpp>.
 *
 * ## VFIO Hierarchy
 * ```
 * /dev/vfio/vfio         ← VFIO container (IOMMU domain)
 *   └─ /dev/vfio/<group> ← VFIO group (IOMMU group)
 *        └─ device fd    ← VFIO device (PCIe function)
 * ```
 *
 * ## Prerequisites
 * 1. Kernel built with CONFIG_VFIO=y, CONFIG_VFIO_PCI=y.
 * 2. Device unbound from its native driver and bound to vfio-pci:
 *    ```
 *    echo <vendor>:<device> > /sys/bus/pci/drivers/vfio-pci/new_id
 *    echo <BDF>             > /sys/bus/pci/drivers/<native>/unbind
 *    ```
 * 3. IOMMU enabled in firmware (VT-d on Intel, AMD-Vi on AMD).
 */

#include <qbuem/pcie/pcie_device.hpp>
#include <qbuem/common.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__) && defined(QBUEM_HAS_VFIO)

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/vfio.h>

namespace qbuem::pcie {

// ─── Private helpers ─────────────────────────────────────────────────────────

Result<int> PCIeDevice::find_iommu_group(BDF bdf) noexcept {
    // Resolve the IOMMU group via the sysfs symlink:
    //   /sys/bus/pci/devices/<BDF>/iommu_group -> ../../../kernel/iommu_groups/<N>
    std::array<char, 256> path{};
    std::snprintf(path.data(), path.size(),
                  "/sys/bus/pci/devices/%.*s/iommu_group",
                  static_cast<int>(bdf.size()), bdf.data());

    std::array<char, 256> resolved{};
    ssize_t n = ::readlink(path.data(), resolved.data(), resolved.size() - 1);
    if (n < 0)
        return std::unexpected(std::error_code{errno, std::system_category()});
    resolved[static_cast<size_t>(n)] = '\0';

    // Extract the group number from the last path component.
    const char* last_slash = std::strrchr(resolved.data(), '/');
    if (last_slash == nullptr)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    const int group = std::atoi(last_slash + 1);
    return group;
}

Result<int> PCIeDevice::open_vfio_container() noexcept {
    int container_fd = ::open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
    if (container_fd < 0)
        return std::unexpected(std::error_code{errno, std::system_category()});

    // Verify VFIO API version.
    if (::ioctl(container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
        ::close(container_fd);
        return std::unexpected(std::make_error_code(std::errc::not_supported));
    }

    // Enable the IOMMU type 1 driver (required for DMA mapping).
    if (::ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
        ::close(container_fd);
        return std::unexpected(std::make_error_code(std::errc::not_supported));
    }

    return container_fd;
}

Result<int> PCIeDevice::open_vfio_group(int container_fd, int group_num) noexcept {
    std::array<char, 64> path{};
    std::snprintf(path.data(), path.size(), "/dev/vfio/%d", group_num);

    int group_fd = ::open(path.data(), O_RDWR | O_CLOEXEC);
    if (group_fd < 0)
        return std::unexpected(std::error_code{errno, std::system_category()});

    // Verify the group is viable.
    struct vfio_group_status gs{};
    gs.argsz = sizeof(gs);
    if (::ioctl(group_fd, VFIO_GROUP_GET_STATUS, &gs) < 0) {
        ::close(group_fd);
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    if ((gs.flags & VFIO_GROUP_FLAGS_VIABLE) == 0u) {
        ::close(group_fd);
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    // Attach group to container.
    if (::ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd) < 0) {
        ::close(group_fd);
        return std::unexpected(std::error_code{errno, std::system_category()});
    }

    // Enable IOMMU on the container now that a group is attached.
    if (::ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
        // EBUSY means IOMMU was already set — that's fine.
        if (errno != EBUSY) {
            ::close(group_fd);
            return std::unexpected(std::error_code{errno, std::system_category()});
        }
    }

    return group_fd;
}

// ─── PCIeDevice constructor / destructor ─────────────────────────────────────

PCIeDevice::PCIeDevice(int container_fd, int group_fd, int device_fd,
                        int iommu_group, std::string_view bdf) noexcept
    : container_fd_(container_fd)
    , group_fd_(group_fd)
    , device_fd_(device_fd)
    , iommu_group_(iommu_group)
{
    const size_t n = std::min(bdf.size(), bdf_.size() - 1);
    std::memcpy(bdf_.data(), bdf.data(), n);
    bdf_[n] = '\0';
}

PCIeDevice::~PCIeDevice() {
    if (bar0_vaddr_ != nullptr && bar0_size_ != 0u)
        ::munmap(bar0_vaddr_, bar0_size_);
    if (device_fd_    >= 0) ::close(device_fd_);
    if (group_fd_     >= 0) ::close(group_fd_);
    if (container_fd_ >= 0) ::close(container_fd_);
}

// ─── PCIeDevice::open ────────────────────────────────────────────────────────

Result<std::unique_ptr<PCIeDevice>> PCIeDevice::open(BDF bdf) noexcept {
    // 1. Find the IOMMU group for this BDF.
    auto group_res = find_iommu_group(bdf);
    if (!group_res) return std::unexpected(group_res.error());
    const int group_num = *group_res;

    // 2. Open the VFIO container.
    auto container_res = open_vfio_container();
    if (!container_res) return std::unexpected(container_res.error());
    const int container_fd = *container_res;

    // 3. Open the VFIO group and attach it to the container.
    auto group_res2 = open_vfio_group(container_fd, group_num);
    if (!group_res2) {
        ::close(container_fd);
        return std::unexpected(group_res2.error());
    }
    const int group_fd = *group_res2;

    // 4. Open the device fd within the group.
    std::array<char, 17> bdf_cstr{};
    const size_t n = std::min(bdf.size(), bdf_cstr.size() - 1);
    std::memcpy(bdf_cstr.data(), bdf.data(), n);

    int device_fd = ::ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, bdf_cstr.data());
    if (device_fd < 0) {
        ::close(group_fd);
        ::close(container_fd);
        return std::unexpected(std::error_code{errno, std::system_category()});
    }

    // 5. Read vendor/device IDs from config space (offset 0x00, 0x02).
    auto dev = std::unique_ptr<PCIeDevice>(
        new PCIeDevice(container_fd, group_fd, device_fd, group_num, bdf));

    struct vfio_region_info cfg{};
    cfg.argsz = sizeof(cfg);
    cfg.index = VFIO_PCI_CONFIG_REGION_INDEX;
    if (::ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg) == 0) {
        std::array<uint16_t, 2> ids{};
        [[maybe_unused]] auto n = ::pread(device_fd, ids.data(), ids.size() * sizeof(uint16_t), static_cast<off_t>(cfg.offset));
        dev->vendor_id_ = ids[0];
        dev->device_id_ = ids[1];
    }

    return dev;
}

// ─── map_bar ────────────────────────────────────────────────────────────────

Result<BarMapping> PCIeDevice::map_bar(uint8_t bar_idx) noexcept {
    if (bar_idx > 5)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    struct vfio_region_info ri{};
    ri.argsz = sizeof(ri);
    ri.index = static_cast<uint32_t>(VFIO_PCI_BAR0_REGION_INDEX) + bar_idx;

    if (::ioctl(device_fd_, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0)
        return std::unexpected(std::error_code{errno, std::system_category()});

    if (ri.size == 0)
        return std::unexpected(std::make_error_code(std::errc::no_such_device));

    void* vaddr = ::mmap(nullptr, ri.size,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         device_fd_, static_cast<off_t>(ri.offset));
    if (vaddr == MAP_FAILED)
        return std::unexpected(std::error_code{errno, std::system_category()});

    // Cache BAR 0 for direct read_mmio32/write_mmio32 access.
    if (bar_idx == 0) {
        bar0_vaddr_ = vaddr;
        bar0_size_  = ri.size;
    }

    BarMapping m;
    m.vaddr   = vaddr;
    m.size    = ri.size;
    m.bar_idx = bar_idx;
    return m;
}

// ─── MMIO helpers ────────────────────────────────────────────────────────────

uint32_t PCIeDevice::read_mmio32(size_t offset) const noexcept {
    return *reinterpret_cast<const volatile uint32_t*>(
        static_cast<const uint8_t*>(bar0_vaddr_) + offset);
}

void PCIeDevice::write_mmio32(size_t offset, uint32_t val) noexcept {
    *reinterpret_cast<volatile uint32_t*>(
        static_cast<uint8_t*>(bar0_vaddr_) + offset) = val;
}

// ─── alloc_dma_buffer ────────────────────────────────────────────────────────

Result<DmaBuffer> PCIeDevice::alloc_dma_buffer(size_t size,
                                                 uint64_t iova_hint) noexcept {
    // Round up to page boundary.
    const size_t page = static_cast<size_t>(::getpagesize());
    const size_t alloc_size = (size + page - 1) & ~(page - 1);

    void* vaddr = ::mmap(nullptr, alloc_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    if (vaddr == MAP_FAILED)
        return std::unexpected(std::error_code{errno, std::system_category()});

    // Assign IOVA: use hint if provided, otherwise auto-increment.
    const uint64_t iova = (iova_hint != 0)
        ? iova_hint
        : next_iova_.fetch_add(alloc_size, std::memory_order_relaxed);

    // Map the memory into the IOMMU.
    struct vfio_iommu_type1_dma_map dma_map{};
    dma_map.argsz  = sizeof(dma_map);
    dma_map.flags  = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
    dma_map.vaddr  = reinterpret_cast<uint64_t>(vaddr);
    dma_map.iova   = iova;
    dma_map.size   = alloc_size;

    if (::ioctl(container_fd_, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
        ::munmap(vaddr, alloc_size);
        return std::unexpected(std::error_code{errno, std::system_category()});
    }

    DmaBuffer buf;
    buf.vaddr        = vaddr;
    buf.iova         = iova;
    buf.size         = alloc_size;
    buf.container_fd = container_fd_;
    return buf;
}

// ─── enumerate_pcie_devices ──────────────────────────────────────────────────

template <size_t N>
size_t enumerate_pcie_devices(std::array<std::array<char, 16>, N>& out) noexcept {
    DIR* dir = ::opendir("/sys/bus/pci/devices");
    if (dir == nullptr) return 0;

    size_t count = 0;
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(dir)) != nullptr && count < N) {
        if (ent->d_name[0] == '.') continue;
        const size_t len = std::min(std::strlen(ent->d_name), size_t{15});
        std::memcpy(out[count].data(), ent->d_name, len);
        out[count][len] = '\0';
        ++count;
    }
    ::closedir(dir);
    return count;
}

// Explicit instantiation for the default N=64.
template size_t enumerate_pcie_devices<64>(std::array<std::array<char, 16>, 64>&) noexcept;

// ─── bdf_to_iommu_group ──────────────────────────────────────────────────────

Result<int> bdf_to_iommu_group(std::string_view bdf) noexcept {
    // Resolve the IOMMU group via the sysfs symlink directly.
    std::array<char, 256> path{};
    std::snprintf(path.data(), path.size(),
                  "/sys/bus/pci/devices/%.*s/iommu_group",
                  static_cast<int>(bdf.size()), bdf.data());
    std::array<char, 256> resolved{};
    ssize_t n = ::readlink(path.data(), resolved.data(), resolved.size() - 1);
    if (n < 0)
        return std::unexpected(std::error_code{errno, std::system_category()});
    resolved[static_cast<size_t>(n)] = '\0';
    const char* last_slash = std::strrchr(resolved.data(), '/');
    if (last_slash == nullptr)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    return std::atoi(last_slash + 1);
}

} // namespace qbuem::pcie

#else // !defined(__linux__) || !defined(QBUEM_HAS_VFIO)

// Stub implementations for non-Linux or builds without VFIO headers.
#include <qbuem/pcie/pcie_device.hpp>
namespace qbuem::pcie {

Result<std::unique_ptr<PCIeDevice>> PCIeDevice::open(BDF) noexcept {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
}
Result<BarMapping> PCIeDevice::map_bar(uint8_t) noexcept {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
}
uint32_t PCIeDevice::read_mmio32(size_t) const noexcept { return 0; }
void     PCIeDevice::write_mmio32(size_t, uint32_t) noexcept {}
Result<DmaBuffer> PCIeDevice::alloc_dma_buffer(size_t, uint64_t) noexcept {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
}
PCIeDevice::~PCIeDevice() {}
PCIeDevice::PCIeDevice(int, int, int, int, std::string_view) noexcept {}
Result<int> PCIeDevice::find_iommu_group(BDF) noexcept {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
}
Result<int> PCIeDevice::open_vfio_container() noexcept {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
}
Result<int> PCIeDevice::open_vfio_group(int, int) noexcept {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
}
template <size_t N>
size_t enumerate_pcie_devices(std::array<std::array<char, 16>, N>&) noexcept { return 0; }
template size_t enumerate_pcie_devices<64>(std::array<std::array<char, 16>, 64>&) noexcept;
Result<int> bdf_to_iommu_group(std::string_view) noexcept {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace qbuem::pcie

#endif
