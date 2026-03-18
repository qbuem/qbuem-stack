#pragma once

/**
 * @file qbuem/pcie/msix_reactor.hpp
 * @brief MSI-X interrupt → Reactor bridge.
 * @defgroup qbuem_pcie_msix MSIXReactor
 * @ingroup qbuem_embedded
 *
 * ## Overview
 * Converts PCIe MSI-X interrupts into `eventfd`(2) signals and processes them
 * asynchronously inside `IReactor` (`epoll`/`io_uring`/`kqueue`).
 *
 * ## How it works
 * ```
 * PCIe NIC/GPU             kernel VFIO           user-space
 *   MSI-X IRQ ──────────► eventfd increment ──► epoll_wait / io_uring
 *   (hardware interrupt)   (kernel → user)        (co_await inside IReactor)
 * ```
 *
 * ## VFIO IRQ setup flow
 * 1. Query the number of MSI-X vectors via `VFIO_DEVICE_GET_IRQ_INFO`.
 * 2. Create one eventfd per vector (`eventfd(0, EFD_NONBLOCK)`).
 * 3. Bind each eventfd to its MSI-X vector via `VFIO_DEVICE_SET_IRQS`.
 * 4. Register each eventfd with epoll/io_uring → wakeup on interrupt.
 *
 * ## Usage example
 * @code
 * auto dev = PCIeDevice::open("0000:03:00.0");
 * MSIXReactor msix(*dev, reactor);
 *
 * auto result = msix.setup(32); // activate 32 MSI-X vectors
 *
 * // Wait for an interrupt on vector 0
 * while (true) {
 *     co_await msix.wait(0); // suspend until interrupt fires
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

// ─── Constants ────────────────────────────────────────────────────────────────

/** @brief Maximum number of supported MSI-X vectors. */
inline constexpr size_t kMaxMSIXVectors = 2048;

// ─── IRQ statistics ───────────────────────────────────────────────────────────

/**
 * @brief Per-vector IRQ statistics.
 *
 * Cache-line aligned to prevent false sharing.
 */
struct alignas(64) VectorStats {
    std::atomic<uint64_t> irq_count{0};    ///< Number of interrupts received
    std::atomic<uint64_t> missed{0};        ///< Interrupts lost due to processing delay
    std::atomic<uint64_t> latency_ns{0};    ///< Last interrupt latency (ns)
};

// ─── MSIXReactor ─────────────────────────────────────────────────────────────

/**
 * @brief PCIe MSI-X interrupt ↔ IReactor asynchronous bridge.
 *
 * ## Design principles
 * - **Zero Thread**: Interrupt handlers run directly on the Reactor thread.
 * - **Per-vector eventfd**: Each vector has its own eventfd for IRQ affinity support.
 * - **Lock-free dispatch**: IRQ receipt → co_await resume operates without locks.
 */
class MSIXReactor {
public:
    /**
     * @brief Constructs an MSIXReactor.
     *
     * @param dev     VFIO PCIe device handle.
     * @param reactor Reactor to dispatch events to.
     */
    MSIXReactor(PCIeDevice& dev, Reactor& reactor) noexcept;
    ~MSIXReactor();

    MSIXReactor(const MSIXReactor&) = delete;
    MSIXReactor& operator=(const MSIXReactor&) = delete;

    /**
     * @brief Activates MSI-X vectors and binds eventfds.
     *
     * @param num_vectors Number of vectors to activate (1 ~ kMaxMSIXVectors).
     * @returns `Result<void>` on success.
     *
     * @note Must be called only once. Returns an error if called again.
     */
    [[nodiscard]] Result<void> setup(size_t num_vectors) noexcept;

    /**
     * @brief Suspends via co_await until an interrupt fires on the specified MSI-X vector.
     *
     * Monitors the eventfd using epoll/io_uring.
     * When an interrupt fires, reads and consumes the eventfd counter.
     *
     * @param vector_idx MSI-X vector index (0 ~ num_vectors-1).
     * @returns Consumed IRQ count (1 or more).
     */
    Task<uint64_t> wait(size_t vector_idx) noexcept;

    /**
     * @brief Consumes an interrupt in a non-blocking manner.
     *
     * @param vector_idx Vector index.
     * @returns IRQ count, or 0 if no IRQ is pending.
     */
    uint64_t try_consume(size_t vector_idx) noexcept;

    /**
     * @brief Masks the IRQ for a specific vector (hardware mask).
     *
     * Blocks interrupts at the hardware level via the MASK action of `VFIO_DEVICE_SET_IRQS`.
     *
     * @param vector_idx Vector index.
     */
    [[nodiscard]] Result<void> mask(size_t vector_idx) noexcept;

    /**
     * @brief Unmasks the IRQ for a specific vector.
     */
    [[nodiscard]] Result<void> unmask(size_t vector_idx) noexcept;

    /** @brief Number of active vectors. */
    [[nodiscard]] size_t num_vectors() const noexcept { return num_vectors_; }

    /** @brief Per-vector statistics reference. */
    [[nodiscard]] const VectorStats& stats(size_t vec) const noexcept {
        return stats_[vec < kMaxMSIXVectors ? vec : 0];
    }

    /** @brief Total number of IRQs received across all vectors. */
    [[nodiscard]] uint64_t total_irqs() const noexcept;

private:
    /**
     * @brief Registers an eventfd with the Reactor event loop.
     *
     * Uses `epoll_ctl(EPOLL_CTL_ADD, EPOLLIN | EPOLLET)` or
     * an `io_uring` POLL_ADD SQE.
     */
    Result<void> register_eventfd(size_t vec_idx, int efd) noexcept;

    /**
     * @brief Binds eventfds to MSI-X vectors via the VFIO SET_IRQS ioctl.
     */
    Result<void> bind_eventfds_to_device(size_t num_vectors) noexcept;

    PCIeDevice& dev_;
    Reactor&    reactor_;
    size_t      num_vectors_{0};

    // Per-vector eventfd array (dynamically allocated, up to kMaxMSIXVectors)
    int*        eventfds_{nullptr};   // [num_vectors_]

    // Per-vector statistics
    VectorStats stats_[kMaxMSIXVectors];

    std::atomic<bool> initialized_{false};
};

// ─── IRQ Affinity helpers ─────────────────────────────────────────────────────

/**
 * @brief Binds an MSI-X vector to the Reactor of a specific CPU core.
 *
 * Modifies `/proc/irq/<irq_num>/smp_affinity` so that the hardware interrupt
 * is handled only on the specified CPU(s).
 *
 * @param irq_num   Linux IRQ number (visible in `/proc/interrupts`).
 * @param cpu_mask  CPU bitmask (e.g. 0x1 = CPU 0, 0x3 = CPU 0 and 1).
 * @returns `Result<void>` on success.
 */
[[nodiscard]] Result<void> set_irq_affinity(int irq_num,
                                              uint64_t cpu_mask) noexcept;

/**
 * @brief Looks up the Linux IRQ number for a given MSI-X vector index.
 *
 * Scans `/sys/bus/pci/devices/<BDF>/msi_irqs/`.
 *
 * @param bdf       PCI BDF address.
 * @param vec_idx   MSI-X vector index.
 * @returns Linux IRQ number or an error.
 */
[[nodiscard]] Result<int> get_msix_irq_number(std::string_view bdf,
                                               size_t vec_idx) noexcept;

} // namespace qbuem::pcie

/** @} */
