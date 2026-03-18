#pragma once

/**
 * @file qbuem/ebpf/ebpf_tracer.hpp
 * @brief eBPF CO-RE based qbuem-stack observability — BPF probe points and map read interface.
 * @defgroup qbuem_ebpf EBPFTracer
 * @ingroup qbuem_observability
 *
 * ## Overview
 * Uses eBPF CO-RE (Compile Once - Run Everywhere) to measure qbuem-stack
 * runtime events in kernel-space and deliver them to user-space.
 *
 * ## Architecture
 * ```
 * qbuem-stack (user-space)     kernel eBPF programs    user-space consumer
 *  TCP accept ──────────────► kprobe/tracepoint ────► BPF ringbuf ──► EBPFTracer::poll()
 *  HTTP parse  ─────────────► uprobe               ──► BPF hashmap ──► EBPFTracer::read_map()
 *  io_uring SQE ───────────► tracepoint/fentry    ──► perf event  ──► EBPFTracer::subscribe()
 * ```
 *
 * ## CO-RE compatibility
 * BTF (BPF Type Format) is used to relocate kernel struct offsets at runtime.
 * A BPF object compiled once runs on kernel 5.4+ without recompilation.
 *
 * ## Probe point catalog
 * | Probe point                       | Type       | Event                          |
 * |-----------------------------------|------------|--------------------------------|
 * | `tcp_accept`                      | kprobe     | New TCP connection accepted     |
 * | `http_parse_latency`              | uprobe     | HTTP parsing latency           |
 * | `pipeline_action_enter/exit`      | uprobe     | Pipeline action execution time |
 * | `io_uring_submit`                 | tracepoint | io_uring SQE submission        |
 * | `shm_channel_send`                | uprobe     | SHM channel message send       |
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace qbuem::ebpf {

// ─── Event types ──────────────────────────────────────────────────────────────

/** @brief Kinds of events collected from eBPF probe points. */
enum class EventType : uint16_t {
    TcpAccept          = 1,
    TcpClose           = 2,
    HttpParseBegin     = 3,
    HttpParseEnd       = 4,
    PipelineActionEnter= 5,
    PipelineActionExit = 6,
    IoUringSqeSubmit   = 7,
    IoUringCqeReceive  = 8,
    ShmChannelSend     = 9,
    ShmChannelRecv     = 10,
    RdmaWrite          = 11,
    JwtVerify          = 12,
    Custom             = 255,
};

// ─── Event record ─────────────────────────────────────────────────────────────

/**
 * @brief Event record delivered from the BPF ringbuf to user-space.
 *
 * Size: 64 bytes (single cache line).
 */
struct alignas(64) TraceEvent {
    uint64_t  timestamp_ns{0};  ///< ktime_get_ns() timestamp
    uint64_t  duration_ns{0};   ///< Elapsed time (for enter/exit pairs)
    uint64_t  tid{0};           ///< Thread ID (BPF bpf_get_current_pid_tgid())
    uint32_t  cpu{0};           ///< CPU number the event ran on
    EventType type{EventType::Custom};
    uint16_t  flags{0};         ///< Per-event flags
    uint8_t   label[24]{};      ///< Event label (null-terminated, max 23 chars)
    uint64_t  val{0};           ///< Per-event extra data (e.g. fd, bytes)

    /** @brief Safely sets the event label. */
    void set_label(std::string_view s) noexcept {
        size_t n = s.size() < 23 ? s.size() : 23;
        __builtin_memcpy(label, s.data(), n);
        label[n] = '\0';
    }
    [[nodiscard]] std::string_view get_label() const noexcept {
        return {reinterpret_cast<const char*>(label)};
    }
};
static_assert(sizeof(TraceEvent) == 64, "TraceEvent must be exactly 64 bytes (one cache line)");

// ─── BPF map statistics ───────────────────────────────────────────────────────

/**
 * @brief Global statistics aggregated by qbuem BPF programs.
 *
 * Stored in a BPF hashmap/percpu_array and queried via `EBPFTracer::read_stats()`.
 */
struct BPFStats {
    uint64_t tcp_accepts{0};         ///< Total TCP accept count
    uint64_t http_requests{0};       ///< Total HTTP request count
    uint64_t avg_http_parse_ns{0};   ///< Average HTTP parse latency (ns)
    uint64_t io_uring_submits{0};    ///< io_uring SQE submission count
    uint64_t pipeline_actions{0};    ///< Pipeline action execution count
    uint64_t shm_sends{0};          ///< SHM channel send count
    uint64_t jwt_verifications{0};   ///< JWT verification count
    uint64_t rdma_writes{0};        ///< RDMA Write count
};

// ─── Event callback ───────────────────────────────────────────────────────────

/** @brief BPF ringbuf event receive callback. */
using EventCallback = std::function<void(const TraceEvent&)>;

// ─── EBPFTracer ──────────────────────────────────────────────────────────────

/**
 * @brief qbuem-stack eBPF observability tracer.
 *
 * ## Lifecycle
 * 1. `EBPFTracer::create()` — Load BPF object, initialize maps and programs.
 * 2. `enable()` — Attach probe points (uprobe/kprobe/tracepoint).
 * 3. `poll()` — Receive events from the BPF ringbuf.
 * 4. `disable()` — Detach probe points.
 *
 * ## Permissions
 * Requires `CAP_BPF` (Linux 5.8+) or `CAP_SYS_ADMIN`.
 */
class EBPFTracer {
public:
    /**
     * @brief Initializes the EBPFTracer.
     *
     * @param bpf_obj_path Path to the BPF object file (`.bpf.o`).
     *                     If `""`, the built-in skeleton BPF is used.
     * @returns Tracer or error.
     */
    static Result<std::unique_ptr<EBPFTracer>> create(
        std::string_view bpf_obj_path = "") noexcept;

    EBPFTracer() = default;
    EBPFTracer(const EBPFTracer&) = delete;
    EBPFTracer& operator=(const EBPFTracer&) = delete;
    virtual ~EBPFTracer() = default;

    // ── Probe point management ─────────────────────────────────────────────

    /**
     * @brief Activates a specific probe point.
     *
     * @param event_type Event type to activate.
     * @returns `Result<void>` on success.
     */
    virtual Result<void> enable(EventType event_type) noexcept = 0;

    /**
     * @brief Activates all probe points.
     */
    virtual Result<void> enable_all() noexcept = 0;

    /**
     * @brief Deactivates a specific probe point.
     */
    virtual Result<void> disable(EventType event_type) noexcept = 0;

    /**
     * @brief Deactivates all probe points.
     */
    virtual void disable_all() noexcept = 0;

    // ── Event reception ────────────────────────────────────────────────────

    /**
     * @brief Polls the BPF ringbuf and receives events.
     *
     * @param out        Array to store received events.
     * @param timeout_ms Polling timeout (ms). 0 = non-blocking.
     * @returns Number of events received.
     */
    virtual size_t poll(std::span<TraceEvent> out,
                         int timeout_ms = 0) noexcept = 0;

    /**
     * @brief Registers an event callback.
     *
     * Use this when a callback-based approach is preferred over `poll()`.
     *
     * @param cb Callback invoked when an event is received.
     */
    virtual void subscribe(EventCallback cb) noexcept = 0;

    // ── Statistics ─────────────────────────────────────────────────────────

    /**
     * @brief Reads aggregated statistics from the BPF map.
     *
     * @returns Current BPFStats snapshot.
     */
    [[nodiscard]] virtual BPFStats read_stats() const noexcept = 0;

    /**
     * @brief Resets BPF statistics counters.
     */
    virtual void reset_stats() noexcept = 0;

    // ── Direct BPF map access ──────────────────────────────────────────────

    /**
     * @brief Looks up a value by key in a BPF map.
     *
     * @tparam K Key type.
     * @tparam V Value type.
     * @param map_name BPF map name.
     * @param key      Lookup key.
     * @returns Value or nullopt (key not found).
     */
    template <typename K, typename V>
    [[nodiscard]] std::optional<V> lookup_map(std::string_view map_name,
                                               const K& key) const noexcept;

    // ── Diagnostics ────────────────────────────────────────────────────────

    /** @brief Number of loaded BPF programs. */
    [[nodiscard]] virtual size_t program_count() const noexcept = 0;

    /** @brief Number of loaded BPF maps. */
    [[nodiscard]] virtual size_t map_count() const noexcept = 0;

    /** @brief BPF ringbuf drop count (when buffer is saturated). */
    [[nodiscard]] virtual uint64_t ringbuf_drops() const noexcept = 0;
};

// ─── Lightweight probe macro (user-space uprobe probe points) ─────────────────

/**
 * @brief Inline no-op function that defines a uprobe probe point.
 *
 * BPF programs attach a uprobe to this function.
 * Declared with `[[gnu::noinline]]` to prevent the symbol from being removed
 * even in release builds.
 *
 * @code
 * // Used internally by qbuem:
 * QBUEM_TRACE_POINT("pipeline.http.parse.begin", fd, 0);
 * // BPF program: SEC("uprobe/qbuem_trace_point")
 * @endcode
 */
[[gnu::noinline]] inline void qbuem_trace_point(
    const char* label, uint64_t val0, uint64_t val1) noexcept {
    // Probe point no-op — a BPF uprobe is attached here.
    // volatile prevents the compiler from optimizing away the arguments.
    volatile const char* l = label;
    volatile uint64_t    v0 = val0;
    volatile uint64_t    v1 = val1;
    (void)l; (void)v0; (void)v1;
}

/**
 * @brief Convenience macro — invokes a probe point.
 *
 * Probe points are retained even in release builds (for BPF attachment).
 * Overhead is a single no-op function call (~1 ns).
 */
#define QBUEM_TRACE(label, val0, val1) \
    ::qbuem::ebpf::qbuem_trace_point((label), (val0), (val1))

} // namespace qbuem::ebpf

/** @} */
