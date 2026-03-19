#pragma once

/**
 * @file qbuem/tools/chaos_hardware.hpp
 * @brief ChaosHardware — user-space fault injection for PCIe/NVMe/RDMA paths.
 * @defgroup qbuem_chaos_hardware ChaosHardware
 * @ingroup qbuem_tools
 *
 * ## Overview
 *
 * `ChaosHardware` injects controlled faults into hardware I/O paths to test
 * the resilience of the qbuem-stack under real-world failure scenarios:
 *
 * | Fault class          | Description                                              |
 * |----------------------|----------------------------------------------------------|
 * | `ErrorInjection`     | Return EIO/ETIMEDOUT/ENODEV on NVMe/RDMA calls          |
 * | `LatencySpike`       | Delay completions by N microseconds (probabilistic)     |
 * | `BitFlip`            | Corrupt N random bits in I/O buffer data                |
 * | `PartialWrite`       | Truncate an NVMe/RDMA write to K bytes out of N         |
 * | `Reorder`            | Deliver completions out-of-order                        |
 * | `DropCompletion`     | Silently discard a completion (forces timeout/retry)    |
 *
 * ## Design
 *
 * `ChaosHardware` wraps `IChaosTarget` — an abstraction over any injected
 * hardware path. The target is called before / after every I/O and may
 * modify or suppress the operation. Production code compiles to a no-op
 * when `QBUEM_CHAOS_ENABLED` is not defined.
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

namespace qbuem::tools {

// ─── FaultClass ──────────────────────────────────────────────────────────────

/**
 * @brief Hardware fault categories.
 */
enum class FaultClass : uint8_t {
    None            = 0,
    ErrorInjection  = 1, ///< Synthetic error code returned
    LatencySpike    = 2, ///< Delayed completion
    BitFlip         = 3, ///< Data corruption (bit flip in buffer)
    PartialWrite    = 4, ///< Truncated write
    Reorder         = 5, ///< Out-of-order completion delivery
    DropCompletion  = 6, ///< Suppressed completion (timeout forced)
};

[[nodiscard]] constexpr std::string_view fault_class_str(FaultClass fc) noexcept {
    switch (fc) {
    case FaultClass::None:           return "none";
    case FaultClass::ErrorInjection: return "error_injection";
    case FaultClass::LatencySpike:   return "latency_spike";
    case FaultClass::BitFlip:        return "bit_flip";
    case FaultClass::PartialWrite:   return "partial_write";
    case FaultClass::Reorder:        return "reorder";
    case FaultClass::DropCompletion: return "drop_completion";
    }
    return "unknown";
}

// ─── FaultSpec ───────────────────────────────────────────────────────────────

/**
 * @brief Configuration for a single fault injection rule.
 */
struct FaultSpec {
    FaultClass fault{FaultClass::None};

    /// Probability [0.0, 1.0] that the fault fires on each eligible operation.
    double probability{0.01};

    /// For LatencySpike: additional delay in nanoseconds.
    uint64_t latency_ns{100'000ULL}; // 100 µs default

    /// For BitFlip: number of bits to flip per operation.
    uint32_t bit_flip_count{1};

    /// For PartialWrite: bytes to write (< requested size).
    uint32_t partial_bytes{0};

    /// For ErrorInjection: errno value to inject (e.g. EIO = 5).
    int32_t error_code{5};

    /// Operation type filter: 0 = all, 1 = reads, 2 = writes, 3 = admin.
    uint8_t op_filter{0};

    /// True if this rule is currently active.
    bool enabled{true};
};

// ─── FaultEvent ──────────────────────────────────────────────────────────────

/**
 * @brief A recorded fault injection event.
 */
struct FaultEvent {
    static constexpr size_t kTargetLen = 32;

    uint64_t    timestamp_ns{0};
    uint64_t    io_size{0};         ///< Requested I/O size
    FaultClass  fault{FaultClass::None};
    uint8_t     _pad[7]{};
    char        target[kTargetLen]{}; ///< Target identifier (e.g. "nvme0n1", "mlx5_0")
    int32_t     injected_error{0};  ///< Actual error code injected (0 = no error)
    uint64_t    latency_ns{0};      ///< Injected latency

    void set_target(std::string_view t) noexcept {
        size_t len = std::min(t.size(), kTargetLen - 1);
        std::memcpy(target, t.data(), len);
        target[len] = '\0';
    }
};
static_assert(std::is_trivially_copyable_v<FaultEvent>);

// ─── IChaosTarget ────────────────────────────────────────────────────────────

/**
 * @brief Interface for hardware I/O paths that can receive fault injection.
 *
 * Implement on top of `INvmeOfTransport`, `IRdmaChannel`, etc.
 */
class IChaosTarget {
public:
    virtual ~IChaosTarget() = default;

    /** @brief Short identifier for this target (e.g. "nvme0n1"). */
    [[nodiscard]] virtual std::string_view target_id() const noexcept = 0;

    /**
     * @brief Called before an I/O operation is issued.
     *
     * @param op_type  1=read, 2=write, 3=admin.
     * @param buf      Buffer for the operation (may be modified for BitFlip).
     * @param size     Requested transfer size.
     * @returns -errno to abort the operation with an injected error, or 0 to proceed.
     */
    virtual int pre_io(uint8_t op_type,
                       std::span<std::byte> buf,
                       uint64_t             size) noexcept = 0;

    /**
     * @brief Called after an I/O operation completes.
     *
     * @param op_type  1=read, 2=write, 3=admin.
     * @param result   Actual I/O result (bytes transferred or -errno).
     * @returns Modified result (e.g. DropCompletion returns INT_MIN to suppress).
     */
    virtual int post_io(uint8_t op_type, int result) noexcept = 0;
};

// ─── ChaosStats ──────────────────────────────────────────────────────────────

/**
 * @brief Fault injection statistics.
 */
struct alignas(64) ChaosStats {
    std::atomic<uint64_t> total_ops{0};         ///< Total I/O operations observed
    std::atomic<uint64_t> faults_injected{0};   ///< Total faults actually fired
    std::atomic<uint64_t> errors_injected{0};   ///< ErrorInjection count
    std::atomic<uint64_t> latency_spikes{0};    ///< LatencySpike count
    std::atomic<uint64_t> bit_flips{0};         ///< BitFlip count
    std::atomic<uint64_t> partial_writes{0};    ///< PartialWrite count
    std::atomic<uint64_t> dropped_completions{0}; ///< DropCompletion count
};

// ─── ChaosHardware ───────────────────────────────────────────────────────────

/**
 * @brief User-space hardware fault injector.
 *
 * Routes I/O operations through registered `IChaosTarget`s, applying
 * `FaultSpec` rules based on probabilistic sampling.
 *
 * ### Thread safety
 * `inject_pre()` / `inject_post()` are lock-free and safe to call from
 * reactor threads. `add_rule()` / `remove_rule()` must be called on
 * the configuration thread (cold path).
 *
 * ### Compile-out
 * When `QBUEM_CHAOS_ENABLED` is not defined, `inject_pre()` and
 * `inject_post()` compile to empty inline no-ops with zero overhead.
 */
class ChaosHardware {
public:
    static constexpr size_t kMaxRules   = 16;
    static constexpr size_t kEventRing  = 1024;

    ChaosHardware() = default;

    /**
     * @brief Add or update a fault injection rule.
     *
     * @param spec  Rule specification.
     * @returns True if added, false if the rule table is full.
     */
    bool add_rule(const FaultSpec& spec) noexcept {
        for (auto& r : rules_) {
            if (!r.enabled) { r = spec; rule_count_.fetch_add(1, std::memory_order_relaxed); return true; }
        }
        return false;
    }

    /** @brief Clear all fault injection rules. */
    void clear_rules() noexcept {
        for (auto& r : rules_) r.enabled = false;
        rule_count_.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Enable or disable all fault injection globally.
     */
    void set_enabled(bool enabled) noexcept {
        enabled_.store(enabled, std::memory_order_relaxed);
    }

    /**
     * @brief Called before an I/O operation (hot path).
     *
     * @param target  Target identifier.
     * @param op_type 1=read, 2=write, 3=admin.
     * @param buf     I/O buffer (may be modified for BitFlip).
     * @param size    Requested size.
     * @returns -errno if operation should be aborted, 0 to proceed.
     */
#ifdef QBUEM_CHAOS_ENABLED
    [[nodiscard]] int inject_pre(std::string_view     target,
                                  uint8_t              op_type,
                                  std::span<std::byte> buf,
                                  uint64_t             size) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) return 0;
        stats_.total_ops.fetch_add(1, std::memory_order_relaxed);

        for (const auto& rule : rules_) {
            if (!rule.enabled) continue;
            if (rule.op_filter && rule.op_filter != op_type) continue;
            if (!should_fire(rule.probability)) continue;

            stats_.faults_injected.fetch_add(1, std::memory_order_relaxed);
            record_event(target, rule.fault, size, rule.error_code, rule.latency_ns);

            switch (rule.fault) {
            case FaultClass::ErrorInjection:
                stats_.errors_injected.fetch_add(1, std::memory_order_relaxed);
                return -rule.error_code;
            case FaultClass::BitFlip:
                apply_bit_flip(buf, rule.bit_flip_count);
                stats_.bit_flips.fetch_add(1, std::memory_order_relaxed);
                break;
            case FaultClass::LatencySpike:
                apply_latency(rule.latency_ns);
                stats_.latency_spikes.fetch_add(1, std::memory_order_relaxed);
                break;
            case FaultClass::PartialWrite:
                stats_.partial_writes.fetch_add(1, std::memory_order_relaxed);
                return static_cast<int>(rule.partial_bytes);  // signal partial
            default:
                break;
            }
        }
        return 0;
    }

    [[nodiscard]] int inject_post(std::string_view target,
                                   uint8_t          op_type,
                                   int              result) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) return result;
        for (const auto& rule : rules_) {
            if (!rule.enabled) continue;
            if (rule.fault != FaultClass::DropCompletion) continue;
            if (rule.op_filter && rule.op_filter != op_type) continue;
            if (!should_fire(rule.probability)) continue;
            stats_.dropped_completions.fetch_add(1, std::memory_order_relaxed);
            record_event(target, FaultClass::DropCompletion, 0, 0, 0);
            return INT32_MIN; // sentinel: "completion dropped"
        }
        return result;
    }
#else
    [[nodiscard]] int inject_pre(std::string_view, uint8_t,
                                  std::span<std::byte>, uint64_t) noexcept { return 0; }
    [[nodiscard]] int inject_post(std::string_view, uint8_t, int result) noexcept { return result; }
#endif

    /** @brief Fault injection statistics. */
    [[nodiscard]] const ChaosStats& stats() const noexcept { return stats_; }

    /** @brief Read recent fault events from the ring (up to `max_n`). */
    size_t drain_events(std::span<FaultEvent> out) noexcept {
        size_t n = 0;
        uint64_t h = event_head_.load(std::memory_order_acquire);
        uint64_t t = event_tail_.load(std::memory_order_acquire);
        while (h < t && n < out.size()) {
            out[n++] = event_ring_[h & (kEventRing - 1)];
            event_head_.store(++h, std::memory_order_release);
        }
        return n;
    }

private:
    static bool should_fire(double probability) noexcept {
        // LCG pseudo-random, lock-free
        static std::atomic<uint64_t> seed{12345678901234567ULL};
        uint64_t s = seed.fetch_add(6364136223846793005ULL, std::memory_order_relaxed);
        double r = static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53);
        return r < probability;
    }

    static void apply_bit_flip(std::span<std::byte> buf, uint32_t count) noexcept {
        if (buf.empty()) return;
        static std::atomic<uint64_t> rng{9876543210987654321ULL};
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t r = rng.fetch_add(1234567891234567ULL, std::memory_order_relaxed);
            size_t byte_idx = r % buf.size();
            uint8_t bit     = static_cast<uint8_t>(1u << (r & 7u));
            buf[byte_idx] = static_cast<std::byte>(static_cast<uint8_t>(buf[byte_idx]) ^ bit);
        }
    }

    static void apply_latency(uint64_t ns) noexcept {
        timespec ts{};
        ts.tv_sec  = static_cast<time_t>(ns / 1'000'000'000ULL);
        ts.tv_nsec = static_cast<long>(ns % 1'000'000'000ULL);
        ::nanosleep(&ts, nullptr);
    }

    void record_event(std::string_view target, FaultClass fault,
                      uint64_t size, int32_t err, uint64_t lat_ns) noexcept {
        uint64_t t = event_tail_.load(std::memory_order_relaxed);
        auto& ev = event_ring_[t & (kEventRing - 1)];
        ev.timestamp_ns   = now_ns();
        ev.io_size        = size;
        ev.fault          = fault;
        ev.injected_error = err;
        ev.latency_ns     = lat_ns;
        ev.set_target(target);
        event_tail_.store(t + 1, std::memory_order_release);
        // Drop oldest if full
        if (t - event_head_.load(std::memory_order_acquire) >= kEventRing)
            event_head_.fetch_add(1, std::memory_order_relaxed);
    }

    static uint64_t now_ns() noexcept {
        timespec ts{}; ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
    }

    std::array<FaultSpec, kMaxRules>    rules_{};
    std::array<FaultEvent, kEventRing>  event_ring_{};
    alignas(64) std::atomic<uint64_t>   event_tail_{0};
    alignas(64) std::atomic<uint64_t>   event_head_{0};
    alignas(64) std::atomic<bool>       enabled_{false};
    std::atomic<uint32_t>               rule_count_{0};
    ChaosStats                          stats_;
};

} // namespace qbuem::tools

/** @} */ // end of qbuem_chaos_hardware
