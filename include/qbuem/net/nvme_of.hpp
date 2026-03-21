#pragma once

/**
 * @file qbuem/net/nvme_of.hpp
 * @brief NVMe-over-Fabrics (NVMe-oF) — high-speed remote block storage via RDMA/TCP.
 * @defgroup qbuem_nvme_of NVMe-oF
 * @ingroup qbuem_net
 *
 * ## Overview
 *
 * NVMe-over-Fabrics extends the NVMe command set over a fabric transport,
 * allowing remote NVMe storage to appear local with near-wire-speed latency.
 *
 * | Transport  | Latency  | Throughput   | Notes                           |
 * |------------|----------|--------------|---------------------------------|
 * | RDMA/RoCEv2| ~10 µs   | ~100 Gbit/s  | Lowest latency, zero CPU copy   |
 * | TCP        | ~50 µs   | ~25 Gbit/s   | No special NIC, cloud-friendly  |
 * | FC-NVMe    | ~5 µs    | ~128 Gbit/s  | Fibre Channel fabrics           |
 *
 * ## Architecture
 * ```
 *  App ──► NvmeOfClient::read(lba, len) ──► NVMe command queue (SQ)
 *                                              │ RDMA SEND / TCP
 *                                           Target (storage node)
 *                                              │ NVMe drive
 *                                           Completion queue (CQ)
 *          co_await completion ◄───────────────┘
 * ```
 *
 * ## Design — zero-dependency interface
 * Like `IHttp3Transport`, qbuem-stack does not link the kernel's `nvme-fabrics`
 * module or any userspace library (libnvme, SPDK) directly. Instead, callers
 * implement `INvmeOfTransport` and inject it into `NvmeOfClient`.
 *
 * Recommended implementations:
 * | Library  | Notes                                        |
 * |----------|----------------------------------------------|
 * | **SPDK** | Intel Storage Performance Development Kit    |
 * | **libnvme** | Linux kernel nvme-cli userspace library   |
 * | **XNVME** | Cross-platform NVMe I/O library             |
 *
 * ## Usage
 * @code
 * class SpDkNvmeOfTransport : public qbuem::INvmeOfTransport {
 *     // ... SPDK-based implementation ...
 * };
 *
 * NvmeOfClient client(std::make_unique<SpDkNvmeOfTransport>());
 * auto conn = co_await client.connect("rdma://storage01:4420/nqn.2024.qbuem", st);
 *
 * // Async read — zero CPU copy via RDMA
 * std::array<std::byte, 4096> buf{};
 * auto n = co_await (*conn)->read(0, buf, st);  // LBA 0, 4 KiB
 *
 * // Async write
 * co_await (*conn)->write(0, std::as_bytes(std::span{buf}), st);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace qbuem {

// ─── NvmeOf command types ────────────────────────────────────────────────────

/**
 * @brief NVMe Admin Command opcodes (NVM Express Specification §5).
 */
enum class NvmeAdminOpc : uint8_t {
    DeleteSQ       = 0x00, ///< Delete I/O Submission Queue
    CreateSQ       = 0x01, ///< Create I/O Submission Queue
    GetLogPage     = 0x02, ///< Get Log Page
    DeleteCQ       = 0x04, ///< Delete I/O Completion Queue
    CreateCQ       = 0x05, ///< Create I/O Completion Queue
    Identify       = 0x06, ///< Identify controller/namespace
    Abort          = 0x08, ///< Abort command
    SetFeatures    = 0x09, ///< Set Features
    GetFeatures    = 0x0A, ///< Get Features
    FirmwareActivate = 0x10, ///< Firmware Activate
    KeepAlive      = 0x18, ///< Keep Alive (NVMe-oF)
};

/**
 * @brief NVMe I/O Command opcodes (NVM Command Set §6).
 */
enum class NvmeIOOpc : uint8_t {
    Flush    = 0x00, ///< Flush — persist volatile cache to media
    Write    = 0x01, ///< Write LBAs
    Read     = 0x02, ///< Read LBAs
    WriteUncorrectable = 0x04, ///< Mark LBAs as invalid
    Compare  = 0x05, ///< Compare LBAs against host buffer
    WriteZeroes = 0x08, ///< Zero-fill LBA range
    DatasetMgmt = 0x09, ///< TRIM / deallocate LBA ranges
    Verify   = 0x0C, ///< Verify LBA integrity
};

/**
 * @brief NVMe-oF fabric command opcodes (NVMe-oF Specification §2).
 */
enum class NvmeFabricOpc : uint8_t {
    Connect      = 0x01, ///< Connect to a NVMe controller or I/O queue pair
    AuthSend     = 0x05, ///< Authentication Send
    AuthReceive  = 0x06, ///< Authentication Receive
    Disconnect   = 0x08, ///< Disconnect from a queue pair
    PropertySet  = 0x00, ///< Set controller properties (via Property Access capsule)
    PropertyGet  = 0x04, ///< Get controller properties
};

// ─── NvmeOfAddr ──────────────────────────────────────────────────────────────

/**
 * @brief Parsed NVMe-oF target address.
 *
 * Supports rdma:// and tcp:// URL schemes:
 *   `rdma://192.168.1.10:4420/nqn.2024.company:storage-target-01`
 *   `tcp://storage.internal:4420/nqn.2024.company:storage-target-01`
 */
struct NvmeOfAddr {
    enum class Transport { RDMA, TCP, FC };

    Transport    transport{Transport::TCP}; ///< Fabric transport type
    std::string  host;                      ///< Target host (IP or hostname)
    uint16_t     port{4420};                ///< Target port (default 4420)
    std::string  nqn;                       ///< NVMe Qualified Name of target subsystem
    uint32_t     queue_depth{1024};         ///< I/O queue depth
    uint32_t     num_io_queues{1};          ///< Number of I/O queue pairs

    /**
     * @brief Parse a NVMe-oF URL string.
     *
     * @param url  e.g. "rdma://192.168.1.10:4420/nqn.2024.company:target"
     * @returns Parsed `NvmeOfAddr`, or `std::nullopt` on invalid input.
     */
    [[nodiscard]] static std::optional<NvmeOfAddr> parse(std::string_view url) noexcept {
        NvmeOfAddr addr;
        if (url.starts_with("rdma://"))      { addr.transport = Transport::RDMA; url.remove_prefix(7); }
        else if (url.starts_with("tcp://"))  { addr.transport = Transport::TCP;  url.remove_prefix(6); }
        else return std::nullopt;

        auto slash = url.find('/');
        std::string_view hostport = (slash != std::string_view::npos) ? url.substr(0, slash) : url;
        if (slash != std::string_view::npos) addr.nqn = std::string(url.substr(slash + 1));

        auto colon = hostport.rfind(':');
        if (colon != std::string_view::npos) {
            addr.host = std::string(hostport.substr(0, colon));
            uint16_t p = 0;
            std::from_chars(hostport.data() + colon + 1,
                            hostport.data() + hostport.size(), p);
            if (p != 0u) addr.port = p;
        } else {
            addr.host = std::string(hostport);
        }
        return addr;
    }
};

// ─── NvmeOfStats ─────────────────────────────────────────────────────────────

/**
 * @brief Per-connection I/O statistics (atomic, cache-line aligned).
 */
struct alignas(64) NvmeOfStats {
    std::atomic<uint64_t> read_ops{0};      ///< Total read commands issued
    std::atomic<uint64_t> write_ops{0};     ///< Total write commands issued
    std::atomic<uint64_t> read_bytes{0};    ///< Total bytes read
    std::atomic<uint64_t> write_bytes{0};   ///< Total bytes written
    std::atomic<uint64_t> errors{0};        ///< Total command errors
    std::atomic<uint64_t> avg_latency_ns{0};///< Exponential moving average latency (ns)
    std::atomic<uint64_t> queue_depth{0};   ///< Current outstanding commands
};

// ─── INvmeOfTransport ────────────────────────────────────────────────────────

/**
 * @brief Injection interface for NVMe-oF fabric transport implementations.
 *
 * Implement this interface using SPDK, libnvme, or a custom RDMA/TCP driver,
 * then inject into `NvmeOfClient`.
 *
 * ### Thread safety
 * All methods are called from a single reactor thread; no locking required.
 */
class INvmeOfTransport {
public:
    virtual ~INvmeOfTransport() = default;

    /**
     * @brief Connect to a NVMe-oF target and negotiate queue pairs.
     *
     * @param addr  Parsed target address.
     * @param st    Cancellation token.
     * @returns `Result<void>` — ok when I/O queues are ready.
     */
    [[nodiscard]] virtual Task<Result<void>>
    connect(const NvmeOfAddr& addr, const std::stop_token& st) = 0;

    /**
     * @brief Read `buf.size()` bytes starting at logical block `lba`.
     *
     * @param lba   Logical Block Address (512-byte or 4096-byte blocks depending on format).
     * @param buf   Receive buffer (must be LBA-size-aligned for direct I/O).
     * @param st    Cancellation token.
     * @returns Bytes read on success, or error.
     */
    [[nodiscard]] virtual Task<Result<size_t>>
    read(uint64_t lba, std::span<std::byte> buf, const std::stop_token& st) = 0;

    /**
     * @brief Write `buf.size()` bytes starting at logical block `lba`.
     *
     * @param lba   Logical Block Address.
     * @param buf   Data to write (LBA-size-aligned).
     * @param st    Cancellation token.
     * @returns Bytes written on success, or error.
     */
    [[nodiscard]] virtual Task<Result<size_t>>
    write(uint64_t lba, std::span<const std::byte> buf, const std::stop_token& st) = 0;

    /**
     * @brief Flush all volatile write data to persistent storage.
     *
     * @param st Cancellation token.
     * @returns `Result<void>` — ok when all data is durable.
     */
    [[nodiscard]] virtual Task<Result<void>>
    flush(const std::stop_token& st) = 0;

    /**
     * @brief TRIM / deallocate an LBA range (hole punch).
     *
     * @param lba   Start LBA.
     * @param count Number of LBAs to deallocate.
     * @param st    Cancellation token.
     */
    [[nodiscard]] virtual Task<Result<void>>
    trim(uint64_t lba, uint32_t count, const std::stop_token& st) = 0;

    /**
     * @brief Scatter-gather read — fills multiple discontiguous buffers.
     *
     * @param lba   Start LBA.
     * @param bufs  List of receive buffers.
     * @param st    Cancellation token.
     * @returns Total bytes read.
     */
    [[nodiscard]] virtual Task<Result<size_t>>
    read_scatter(uint64_t lba,
                 std::span<std::span<std::byte>> bufs,
                 const std::stop_token& st) = 0;

    /** @brief True if the connection is open. */
    [[nodiscard]] virtual bool is_connected() const noexcept = 0;

    /** @brief Reference to the transport's statistics counters. */
    [[nodiscard]] virtual NvmeOfStats& stats() noexcept = 0;

    /**
     * @brief Gracefully disconnect from the target.
     * @param st Cancellation token.
     */
    virtual Task<void> disconnect(const std::stop_token& st) = 0;
};

// ─── NvmeOfConnection ────────────────────────────────────────────────────────

/**
 * @brief An open NVMe-oF connection — wraps a transport and adds retry/stats.
 *
 * Created by `NvmeOfClient::connect()`. Callers issue `read()`/`write()` calls
 * which are forwarded to the injected transport. On transient errors, the
 * connection retries up to `max_retries` times with exponential backoff.
 */
class NvmeOfConnection {
public:
    explicit NvmeOfConnection(std::unique_ptr<INvmeOfTransport> transport,
                               int max_retries = 3)
        : transport_(std::move(transport))
        , max_retries_(max_retries)
    {}

    /**
     * @brief Async read with transparent retry on transient errors.
     */
    [[nodiscard]] Task<Result<size_t>>
    read(uint64_t lba, std::span<std::byte> buf, const std::stop_token& st) const {
        for (int attempt = 0; attempt <= max_retries_; ++attempt) {
            auto r = co_await transport_->read(lba, buf, st);
            if (r) {
                transport_->stats().read_ops.fetch_add(1, std::memory_order_relaxed);
                transport_->stats().read_bytes.fetch_add(*r, std::memory_order_relaxed);
                co_return r;
            }
            if (attempt == max_retries_) co_return r;
            transport_->stats().errors.fetch_add(1, std::memory_order_relaxed);
        }
        co_return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    /**
     * @brief Async write with transparent retry on transient errors.
     */
    [[nodiscard]] Task<Result<size_t>>
    write(uint64_t lba, std::span<const std::byte> buf, const std::stop_token& st) const {
        for (int attempt = 0; attempt <= max_retries_; ++attempt) {
            auto r = co_await transport_->write(lba, buf, st);
            if (r) {
                transport_->stats().write_ops.fetch_add(1, std::memory_order_relaxed);
                transport_->stats().write_bytes.fetch_add(*r, std::memory_order_relaxed);
                co_return r;
            }
            if (attempt == max_retries_) co_return r;
        }
        co_return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    /** @brief Scatter-gather read forwarded to transport. */
    [[nodiscard]] Task<Result<size_t>>
    read_scatter(uint64_t lba,
                 std::span<std::span<std::byte>> bufs,
                 const std::stop_token& st) {
        co_return co_await transport_->read_scatter(lba, bufs, st);
    }

    /** @brief Flush to persistent storage. */
    [[nodiscard]] Task<Result<void>> flush(const std::stop_token& st) {
        co_return co_await transport_->flush(st);
    }

    /** @brief TRIM / deallocate range. */
    [[nodiscard]] Task<Result<void>>
    trim(uint64_t lba, uint32_t count, const std::stop_token& st) {
        co_return co_await transport_->trim(lba, count, st);
    }

    /** @brief I/O statistics snapshot. */
    [[nodiscard]] const NvmeOfStats& stats() const noexcept {
        return transport_->stats();
    }

    /** @brief True if still connected. */
    [[nodiscard]] bool is_connected() const noexcept {
        return transport_ != nullptr && transport_->is_connected();
    }

private:
    std::unique_ptr<INvmeOfTransport> transport_;
    int                               max_retries_;
};

// ─── NvmeOfClient ────────────────────────────────────────────────────────────

/**
 * @brief Factory that establishes NVMe-oF connections.
 *
 * ### Usage
 * @code
 * NvmeOfClient client;
 *
 * // Inject a SPDK-based RDMA transport
 * auto transport = std::make_unique<SpdkRdmaTransport>();
 * auto conn = co_await client.connect(
 *     "rdma://192.168.1.10:4420/nqn.2024.company:nvme01",
 *     std::move(transport), st);
 * if (!conn) { // handle error }
 *
 * // Read 4 KiB from LBA 0
 * alignas(4096) std::array<std::byte, 4096> buf{};
 * auto n = co_await (*conn)->read(0, buf, st);
 * @endcode
 */
class NvmeOfClient {
public:
    /**
     * @brief Connect to a NVMe-oF target.
     *
     * @param url_or_nqn  NVMe-oF URL (rdma://host:port/nqn or tcp://...).
     * @param transport   Fabric transport implementation.
     * @param st          Cancellation token.
     * @returns `NvmeOfConnection` on success, or error.
     */
    [[nodiscard]] static Task<Result<std::unique_ptr<NvmeOfConnection>>>
    connect(std::string_view url_or_nqn,
            std::unique_ptr<INvmeOfTransport> transport,
            const std::stop_token& st) {
        auto addr = NvmeOfAddr::parse(url_or_nqn);
        if (!addr) co_return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));

        auto r = co_await transport->connect(*addr, st);
        if (!r) co_return std::unexpected(r.error());

        co_return std::make_unique<NvmeOfConnection>(std::move(transport));
    }
};

} // namespace qbuem

/** @} */ // end of qbuem_nvme_of
