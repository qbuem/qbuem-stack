#pragma once

/**
 * @file qbuem/spdk/nvme_io.hpp
 * @brief User-space NVMe direct I/O via io_uring passthrough.
 * @defgroup qbuem_spdk NVMeIO
 * @ingroup qbuem_storage
 *
 * ## Overview
 * Issues NVMe commands directly from user-space using Linux
 * `io_uring IORING_OP_URING_CMD` (io_uring passthrough) and the NVMe
 * character device (`/dev/ngXnY`).
 *
 * ## Technical background
 * ### io_uring NVMe Passthrough (Linux 6.0+)
 * ```
 * IORING_OP_URING_CMD
 *   └─ NVMe admin/IO command
 *        └─ submitted directly to NVMe controller queue
 *             └─ completion event → CQE → co_await resume
 * ```
 *
 * ### Comparison with SPDK (Storage Performance Development Kit)
 * | Approach              | Latency | Dependencies | Notes                          |
 * |-----------------------|---------|--------------|--------------------------------|
 * | io_uring passthrough  | ~4µs    | None         | Uses kernel NVMe driver        |
 * | SPDK UIO              | ~2µs    | SPDK lib     | DPDK-style full user-space     |
 * | Block layer (AIO)     | ~20µs   | None         | Kernel scheduler involvement   |
 *
 * This header defaults to io_uring passthrough and provides an interface
 * that allows the SPDK UIO backend to be swapped in as a plugin.
 *
 * ## DMA buffers
 * NVMe I/O requires physically contiguous DMA buffers.
 * Use `NVMeIOContext::alloc_dma()` to allocate properly aligned buffers.
 *
 * @code
 * auto ctx = NVMeIOContext::open("/dev/ng0n1");
 * auto dma  = ctx->alloc_dma(4096); // 4 KB DMA buffer
 *
 * // Read sector 0 (LBA 0, 1 sector = 512 B or 4096 B)
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

// ─── NVMe command result ──────────────────────────────────────────────────────

/**
 * @brief NVMe command completion status.
 *
 * Based on NVMe specification section 4.6 (Completion Queue Entry).
 */
struct NVMeResult {
    uint32_t result{0};    ///< Command-specific result DW0
    uint16_t sq_id{0};     ///< Submission Queue ID
    uint16_t cmd_id{0};    ///< Command ID
    uint16_t status{0};    ///< Status Field (bits 14:1)

    /** @brief Returns true if the command succeeded (Status Code == 0). */
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

// ─── DMA buffer ───────────────────────────────────────────────────────────────

/**
 * @brief DMA-aligned buffer for NVMe I/O (RAII).
 *
 * Allocated via `mmap(MAP_HUGETLB)` or `posix_memalign(4096)`.
 * NVMe controllers generally require 4 KB-aligned buffers.
 */
class DMABuffer {
public:
    virtual ~DMABuffer() = default;

    /** @brief User-space address of the buffer. */
    [[nodiscard]] virtual void* data() noexcept = 0;
    [[nodiscard]] virtual const void* data() const noexcept = 0;

    /** @brief Buffer size (bytes). */
    [[nodiscard]] virtual size_t size() const noexcept = 0;

    /** @brief Zero-initializes the buffer. */
    virtual void zero() noexcept = 0;

    /** @brief Byte span view. */
    [[nodiscard]] MutableBufferView span() noexcept {
        return {static_cast<uint8_t*>(data()), size()};
    }
    [[nodiscard]] BufferView span() const noexcept {
        return {static_cast<const uint8_t*>(data()), size()};
    }
};

// ─── Device information ───────────────────────────────────────────────────────

/**
 * @brief NVMe namespace / controller information.
 */
struct NVMeDeviceInfo {
    uint64_t  ns_size{0};       ///< Namespace size (number of LBAs)
    uint32_t  lba_size{512};    ///< LBA size (bytes, typically 512 or 4096)
    uint32_t  optimal_io_size{0}; ///< Optimal I/O size (bytes, 0 = no limit)
    uint32_t  max_sectors{0};   ///< Maximum sectors per single command
    uint16_t  ctrl_id{0};       ///< Controller ID
    uint8_t   ns_id{1};         ///< Namespace ID
    bool      volatile_write_cache{false}; ///< Whether write-back cache is present

    /** @brief Total capacity (bytes). */
    [[nodiscard]] uint64_t total_bytes() const noexcept {
        return ns_size * lba_size;
    }
};

// ─── I/O statistics ───────────────────────────────────────────────────────────

/**
 * @brief NVMe I/O performance statistics (per-context).
 */
struct NVMeStats {
    std::atomic<uint64_t> read_ops{0};      ///< Completed read operations
    std::atomic<uint64_t> write_ops{0};     ///< Completed write operations
    std::atomic<uint64_t> read_bytes{0};    ///< Total bytes read
    std::atomic<uint64_t> write_bytes{0};   ///< Total bytes written
    std::atomic<uint64_t> errors{0};        ///< Error count
    std::atomic<uint64_t> avg_latency_ns{0};///< Average latency (ns, exponential moving average)
};

// ─── NVMeIOContext ────────────────────────────────────────────────────────────

/**
 * @brief NVMe direct I/O context.
 *
 * Uses io_uring passthrough (`IORING_OP_URING_CMD`) to bypass the kernel
 * block layer and submit commands directly to the NVMe queue.
 *
 * ## Prerequisites
 * - Linux 6.0+ (`CONFIG_IO_URING_URING_CMD` enabled)
 * - Read/write access to `/dev/ngXnY` (`CAP_SYS_RAWIO` or ACL)
 * - NVMe driver loaded (`modprobe nvme`)
 *
 * ## Concurrency model
 * Multiple coroutines may share the same `NVMeIOContext`.
 * Concurrent submissions are serialized internally via io_uring SQ/CQ locking.
 * For best performance, use one independent `NVMeIOContext` per core.
 */
class NVMeIOContext {
public:
    /**
     * @brief Opens an NVMe character device.
     *
     * @param dev_path   Device path (e.g. "/dev/ng0n1").
     * @param queue_depth io_uring queue depth (default: 256).
     * @returns Context or error.
     */
    static Result<std::unique_ptr<NVMeIOContext>> open(
        std::string_view dev_path, uint32_t queue_depth = 256) noexcept;

    NVMeIOContext() = default;
    NVMeIOContext(const NVMeIOContext&) = delete;
    NVMeIOContext& operator=(const NVMeIOContext&) = delete;
    virtual ~NVMeIOContext() = default;

    // ── DMA buffer allocation ──────────────────────────────────────────────

    /**
     * @brief Allocates a DMA-aligned buffer for NVMe I/O.
     *
     * @param size  Size in bytes (rounded up to 4 KB alignment).
     * @returns DMABuffer or error.
     */
    [[nodiscard]] virtual Result<std::unique_ptr<DMABuffer>>
    alloc_dma(size_t size) noexcept = 0;

    // ── NVMe I/O ───────────────────────────────────────────────────────────

    /**
     * @brief NVMe read (io_uring passthrough).
     *
     * @param buf    DMA buffer (allocated via alloc_dma()).
     * @param lba    Starting LBA.
     * @param nsects Number of sectors to read.
     * @returns NVMe completion result.
     */
    virtual Task<Result<NVMeResult>> read(DMABuffer& buf,
                                           uint64_t   lba,
                                           uint32_t   nsects) noexcept = 0;

    /**
     * @brief NVMe write (io_uring passthrough).
     *
     * @param buf    DMA buffer.
     * @param lba    Starting LBA.
     * @param nsects Number of sectors to write.
     * @returns NVMe completion result.
     */
    virtual Task<Result<NVMeResult>> write(const DMABuffer& buf,
                                            uint64_t         lba,
                                            uint32_t         nsects) noexcept = 0;

    /**
     * @brief NVMe Flush (Write Cache Sync).
     *
     * Synchronizes data to non-volatile storage via `FUA (Force Unit Access)`
     * or a Flush command.
     */
    virtual Task<Result<NVMeResult>> flush() noexcept = 0;

    /**
     * @brief NVMe Dataset Management (TRIM / DEALLOCATE).
     *
     * @param lba    Starting LBA.
     * @param nsects Number of sectors.
     */
    virtual Task<Result<NVMeResult>> trim(uint64_t lba,
                                           uint32_t nsects) noexcept = 0;

    // ── Vector I/O ─────────────────────────────────────────────────────────

    /**
     * @brief Reads multiple LBA ranges in a single operation (scatter-read).
     *
     * Internally issued as an io_uring linked SQE chain.
     *
     * @param ranges Array of {buf, lba, nsects} ranges.
     * @returns Completion result for all ranges.
     */
    struct IORange { DMABuffer* buf; uint64_t lba; uint32_t nsects; };
    virtual Task<Result<size_t>> read_scatter(std::span<IORange> ranges) noexcept = 0;

    // ── Device information ─────────────────────────────────────────────────

    /** @brief Device information (LBA size, capacity, etc.). */
    [[nodiscard]] virtual const NVMeDeviceInfo& device_info() const noexcept = 0;

    /** @brief I/O statistics reference. */
    [[nodiscard]] virtual const NVMeStats& stats() const noexcept = 0;

    /** @brief Device path. */
    [[nodiscard]] virtual std::string_view device_path() const noexcept = 0;

    // ── Admin commands ─────────────────────────────────────────────────────

    /**
     * @brief NVMe Identify Controller query.
     *
     * @param out 4096-byte DMA buffer to store the result.
     * @returns NVMe completion result.
     */
    virtual Task<Result<NVMeResult>> identify_controller(DMABuffer& out) noexcept = 0;

    /**
     * @brief NVMe Get Log Page (SMART/Health Information).
     *
     * @param out  Result buffer.
     * @param lid  Log Page ID (e.g. 0x02 = SMART).
     * @returns NVMe completion result.
     */
    virtual Task<Result<NVMeResult>> get_log_page(DMABuffer& out,
                                                   uint8_t lid) noexcept = 0;
};

} // namespace qbuem::spdk

/** @} */
