#pragma once

/**
 * @file qbuem/ebpf/memleak_bridge.hpp
 * @brief eBPF memory-leak and allocation profiler bridge.
 * @defgroup qbuem_memleak_bridge MemleakBridge
 * @ingroup qbuem_ebpf
 *
 * ## Overview
 *
 * `MemleakBridge` attaches eBPF uprobes to `malloc`/`free`/`mmap` and custom
 * qbuem Arena/FixedPoolResource entry points. It surfaces outstanding allocation
 * call-stacks via a shared perf ring-buffer so a userspace consumer can produce:
 *
 * - **Leak report** — allocations without a matching free after `ttl_ns`.
 * - **Flame graph** — hierarchical allocation call-stack aggregation.
 * - **Hotspot list** — call-sites ranked by byte-footprint.
 *
 * ## Architecture
 * ```
 *  Process                       Kernel eBPF               User-space consumer
 *  malloc(N) ─────────────────► uprobe: record (addr,sz,  ──► MemleakBridge::poll()
 *  free(p)   ─────────────────►         stack, ts)        ──► outstanding_allocations()
 *  Arena::allocate() ──────────► uprobe (opt)             ──► generate_report()
 *                                         │
 *                                 BPF_MAP_TYPE_HASH (addr → AllocInfo)
 *                                 BPF_MAP_TYPE_RINGBUF (event stream)
 * ```
 *
 * ## Injection model
 *
 * `MemleakBridge` depends on `IEbpfRuntime` for loading/attaching BPF programs.
 * This keeps the header zero-dependency on libbpf/bpftool.
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace qbuem::ebpf {

// ─── AllocRecord ─────────────────────────────────────────────────────────────

/**
 * @brief A single allocation event captured from eBPF.
 *
 * Trivially copyable — passed through the BPF ring buffer.
 * Stack frames are stored as raw instruction pointer addresses.
 */
struct AllocRecord {
    static constexpr size_t kMaxFrames = 32;

    uint64_t address{0};          ///< Allocated address returned by malloc/mmap
    uint64_t size_bytes{0};       ///< Requested allocation size
    uint64_t timestamp_ns{0};     ///< Allocation time (CLOCK_MONOTONIC ns)
    uint64_t tid{0};              ///< Thread ID of the allocating thread
    uint8_t  freed{0};            ///< 1 if a matching free() was observed
    uint8_t  source{0};           ///< 0=malloc, 1=mmap, 2=arena, 3=pool
    std::array<uint8_t, 6>          _pad{};
    std::array<uint64_t, kMaxFrames> stack{}; ///< Instruction pointer call-stack (0-terminated)
    uint32_t depth{0};            ///< Number of valid frames in `stack`
};
static_assert(std::is_trivially_copyable_v<AllocRecord>);

// ─── MemleakReport ───────────────────────────────────────────────────────────

/**
 * @brief Summary report of outstanding allocations.
 */
struct MemleakReport {
    uint64_t              total_outstanding_bytes{0};  ///< Sum of all outstanding allocation sizes
    uint64_t              total_outstanding_count{0};  ///< Number of outstanding allocations
    uint64_t              total_freed_bytes{0};        ///< Total bytes freed since start
    uint64_t              report_ts_ns{0};             ///< When the report was generated
    std::vector<AllocRecord> leaks;                    ///< Records with freed==0 older than TTL
};

// ─── IEbpfRuntime ────────────────────────────────────────────────────────────

/**
 * @brief Minimal eBPF runtime interface.
 *
 * Implement this to bridge to libbpf, libbcc, or a test stub.
 * The `MemleakBridge` depends only on this interface — no libbpf symbols
 * appear in the public header.
 */
class IEbpfRuntime {
public:
    virtual ~IEbpfRuntime() = default;

    /**
     * @brief Attach a uprobe to the given symbol in the target binary.
     *
     * @param binary_path  Path to the ELF binary to attach to.
     * @param symbol       Symbol name (e.g. "malloc", "free").
     * @param is_ret       true for uretprobe (fires on function return).
     * @returns 0 on success, negative errno on failure.
     */
    virtual int attach_uprobe(std::string_view binary_path,
                              std::string_view symbol,
                              bool             is_ret) noexcept = 0;

    /**
     * @brief Read pending events from the BPF ring buffer.
     *
     * @param out     Output buffer for decoded `AllocRecord` entries.
     * @param max_n   Maximum records to read in this call.
     * @returns Number of records written into `out`.
     */
    virtual size_t poll_events(std::span<AllocRecord> out,
                               size_t                 max_n) noexcept = 0;

    /**
     * @brief Query the outstanding allocation map (addr → AllocRecord).
     *
     * @param out  Output buffer to fill with currently tracked allocations.
     * @returns Number of entries written.
     */
    virtual size_t read_alloc_map(std::span<AllocRecord> out) noexcept = 0;

    /** @brief True if the BPF programs are loaded and attached. */
    [[nodiscard]] virtual bool is_attached() const noexcept = 0;

    /** @brief Detach all programs and close BPF fds. */
    virtual void detach() noexcept = 0;
};

// ─── MemleakStats ─────────────────────────────────────────────────────────────

/**
 * @brief Live allocation statistics updated by `MemleakBridge::poll()`.
 */
struct alignas(64) MemleakStats {
    std::atomic<uint64_t> alloc_events{0};  ///< malloc/mmap events observed
    std::atomic<uint64_t> free_events{0};   ///< free/munmap events observed
    std::atomic<uint64_t> outstanding{0};   ///< Current outstanding allocation count
    std::atomic<uint64_t> outstanding_bytes{0}; ///< Outstanding bytes
    std::atomic<uint64_t> poll_cycles{0};   ///< Total poll() calls
};

// ─── MemleakBridge ───────────────────────────────────────────────────────────

/**
 * @brief eBPF memory leak and allocation profiler bridge.
 *
 * Wraps `IEbpfRuntime` to provide:
 * - Continuous event polling loop
 * - Outstanding-allocation tracking
 * - Leak report generation
 *
 * ### Thread safety
 * `poll()` must be called from a single consumer thread.
 * `stats()` and `generate_report()` may be called from any thread.
 *
 * ### Leak threshold
 * An allocation is considered a "leak candidate" if it remains outstanding
 * for longer than `leak_ttl_ns` nanoseconds.
 */
class MemleakBridge {
public:
    static constexpr uint64_t kDefaultLeakTtlNs = 30'000'000'000ULL; ///< 30 s
    static constexpr size_t   kPollBatch         = 256;

    /**
     * @brief Construct with a runtime and optional leak TTL.
     *
     * @param runtime     Injected eBPF runtime (may be nullptr for dry-run mode).
     * @param leak_ttl_ns Allocations older than this are reported as leaks.
     */
    explicit MemleakBridge(IEbpfRuntime* runtime,
                           uint64_t      leak_ttl_ns = kDefaultLeakTtlNs)
        : runtime_(runtime), leak_ttl_ns_(leak_ttl_ns) {}

    /**
     * @brief Attach uprobes to `malloc`, `free`, `mmap`, `munmap`.
     *
     * @param binary_path  Path to the target binary (usually `/proc/self/exe`).
     * @returns true if all probes attached successfully.
     */
    bool attach(std::string_view binary_path = "/proc/self/exe") noexcept {
        if (runtime_ == nullptr) return false;
        bool ok = true;
        ok &= (runtime_->attach_uprobe(binary_path, "malloc",  false) == 0);
        ok &= (runtime_->attach_uprobe(binary_path, "malloc",  true)  == 0); // uretprobe
        ok &= (runtime_->attach_uprobe(binary_path, "free",    false) == 0);
        ok &= (runtime_->attach_uprobe(binary_path, "mmap",    false) == 0);
        ok &= (runtime_->attach_uprobe(binary_path, "munmap",  false) == 0);
        attached_ = ok;
        return ok;
    }

    /**
     * @brief Poll for new allocation events (call from a background loop).
     *
     * Drains up to `kPollBatch` events from the BPF ring buffer, updates
     * in-memory tracking tables and stats.
     *
     * @returns Number of events processed.
     */
    size_t poll() noexcept {
        if (runtime_ == nullptr || !attached_) return 0;
        stats_.poll_cycles.fetch_add(1, std::memory_order_relaxed);

        std::array<AllocRecord, kPollBatch> batch{};
        size_t n = runtime_->poll_events(batch, kPollBatch);

        for (size_t i = 0; i < n; ++i) {
            const auto& rec = batch[i];
            if (rec.freed != 0u) {
                stats_.free_events.fetch_add(1, std::memory_order_relaxed);
                stats_.outstanding.fetch_sub(1, std::memory_order_relaxed);
                stats_.outstanding_bytes.fetch_sub(rec.size_bytes, std::memory_order_relaxed);
            } else {
                stats_.alloc_events.fetch_add(1, std::memory_order_relaxed);
                stats_.outstanding.fetch_add(1, std::memory_order_relaxed);
                stats_.outstanding_bytes.fetch_add(rec.size_bytes, std::memory_order_relaxed);
            }
        }
        return n;
    }

    /**
     * @brief Generate a leak report from the outstanding allocation map.
     *
     * @param now_ns  Current monotonic time in nanoseconds.
     * @returns Populated `MemleakReport`.
     */
    [[nodiscard]] MemleakReport generate_report(uint64_t now_ns) const noexcept {
        MemleakReport report;
        report.report_ts_ns = now_ns;
        report.total_outstanding_count = stats_.outstanding.load(std::memory_order_acquire);
        report.total_outstanding_bytes = stats_.outstanding_bytes.load(std::memory_order_acquire);

        if (runtime_ == nullptr || !attached_) return report;

        // Read all outstanding allocations from the BPF map
        std::array<AllocRecord, 4096> buf{};
        size_t n = runtime_->read_alloc_map(buf);
        for (size_t i = 0; i < n; ++i) {
            const auto& rec = buf[i];
            if (rec.freed == 0u && (now_ns - rec.timestamp_ns) > leak_ttl_ns_) {
                report.leaks.push_back(rec);
                report.total_freed_bytes += 0; // freed bytes tracked separately
            }
        }
        return report;
    }

    /** @brief Detach all eBPF probes. */
    void detach() noexcept {
        if (runtime_ != nullptr) runtime_->detach();
        attached_ = false;
    }

    /** @brief Live allocation statistics. */
    [[nodiscard]] const MemleakStats& stats() const noexcept { return stats_; }

    /** @brief True if probes are attached. */
    [[nodiscard]] bool is_attached() const noexcept { return attached_; }

private:
    IEbpfRuntime*  runtime_{nullptr};
    uint64_t       leak_ttl_ns_;
    bool           attached_{false};
    MemleakStats   stats_;
};

} // namespace qbuem::ebpf

/** @} */ // end of qbuem_memleak_bridge
