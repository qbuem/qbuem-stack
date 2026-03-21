#pragma once

/**
 * @file qbuem/tools/buffer_heatmap.hpp
 * @brief BufferHeatmap — visual lifecycle tracking for zero-copy buffer segments.
 * @defgroup qbuem_buffer_heatmap BufferHeatmap
 * @ingroup qbuem_tools
 *
 * ## Overview
 *
 * `BufferHeatmap` tracks every buffer segment from the moment it is allocated
 * in an `Arena` or `FixedPoolResource` through pipeline stages to final
 * release, producing:
 *
 * - **Heatmap grid** — 2D ASCII/HTML view of time × buffer-slot utilization.
 * - **Lifetime histogram** — distribution of buffer hold durations.
 * - **Stage correlation** — which pipeline stage holds each buffer longest.
 * - **Stall detection** — buffers stuck in a stage above a configurable TTL.
 *
 * ## Design
 *
 * Pipeline stages instrument buffer hand-off via:
 * @code
 * auto ticket = heatmap.acquire(slot_idx, "parse");
 * // ... process buffer ...
 * ticket.release("validate");  // hand-off to next stage
 * @endcode
 *
 * `HeatmapTicket` is a lightweight RAII handle that records transition times
 * into a lock-free ring of `BufferEvent` records.
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace qbuem::tools {

// ─── BufferState ─────────────────────────────────────────────────────────────

/**
 * @brief Buffer segment lifecycle states.
 */
enum class BufferState : uint8_t {
    Free      = 0, ///< Available in the pool
    Allocated = 1, ///< Allocated but not yet in a stage
    InStage   = 2, ///< Currently being processed by a pipeline stage
    Stalled   = 3, ///< In a stage longer than stall_threshold_ns
    Released  = 4, ///< Released back to pool
};

// ─── BufferEvent ─────────────────────────────────────────────────────────────

/**
 * @brief A single buffer lifecycle event (transition record).
 *
 * Trivially copyable — stored in the event ring buffer.
 */
struct BufferEvent {
    static constexpr size_t kStageLen = 24;

    uint64_t    slot_idx{0};           ///< Buffer slot index in the pool
    uint64_t    timestamp_ns{0};       ///< Event time (CLOCK_MONOTONIC)
    uint64_t    duration_ns{0};        ///< Time since last event for this slot
    BufferState state{BufferState::Free};
    uint8_t     _pad[7]{}; // NOLINT(modernize-avoid-c-arrays)
    char        stage[kStageLen]{};    ///< Stage name at transition // NOLINT(modernize-avoid-c-arrays)

    void set_stage(std::string_view s) noexcept {
        size_t len = std::min(s.size(), kStageLen - 1);
        std::memcpy(stage, s.data(), len);
        stage[len] = '\0';
    }
};
static_assert(std::is_trivially_copyable_v<BufferEvent>);

// ─── SlotRecord ──────────────────────────────────────────────────────────────

/**
 * @brief Per-slot current state (one entry per buffer slot in the pool).
 */
struct SlotRecord {
    static constexpr size_t kStageLen = 24;

    uint64_t    slot_idx{0};
    uint64_t    alloc_ns{0};           ///< When this slot was last allocated
    uint64_t    stage_enter_ns{0};     ///< When the current stage acquired it
    uint64_t    total_hold_ns{0};      ///< Cumulative hold time across all stages
    BufferState state{BufferState::Free};
    uint8_t     _pad[7]{}; // NOLINT(modernize-avoid-c-arrays)
    char        stage[kStageLen]{}; // NOLINT(modernize-avoid-c-arrays)
    uint32_t    stage_count{0};        ///< Number of pipeline stages visited

    void set_stage(std::string_view s) noexcept {
        size_t len = std::min(s.size(), kStageLen - 1);
        std::memcpy(stage, s.data(), len);
        stage[len] = '\0';
    }
};

// ─── HeatmapTicket ───────────────────────────────────────────────────────────

/**
 * @brief RAII buffer acquisition ticket.
 *
 * Tracks a buffer through one pipeline stage. On destruction (or explicit
 * `release(next_stage)`), the handoff is recorded.
 *
 * heatmap_ is stored as void* to avoid a forward-declaration conflict with the
 * BufferHeatmap type alias. The inline destructor and release() cast it back to
 * BufferHeatmap* after the full type is available.
 */
class HeatmapTicket {
public:
    HeatmapTicket() = default;

    explicit HeatmapTicket(void*            heatmap,
                           uint64_t         slot_idx,
                           std::string_view stage) noexcept
        : heatmap_(heatmap), slot_idx_(slot_idx) {
        size_t len = std::min(stage.size(), kStageLen - 1);
        std::memcpy(stage_, stage.data(), len);
        stage_[len] = '\0';
    }

    ~HeatmapTicket() noexcept;

    HeatmapTicket(HeatmapTicket&& o) noexcept
        : heatmap_(o.heatmap_), slot_idx_(o.slot_idx_), released_(o.released_) {
        std::memcpy(stage_, o.stage_, kStageLen);
        o.heatmap_  = nullptr;
        o.released_ = true;
    }

    HeatmapTicket& operator=(HeatmapTicket&&) = delete;
    HeatmapTicket(const HeatmapTicket&)       = delete;
    HeatmapTicket& operator=(const HeatmapTicket&) = delete;

    /**
     * @brief Hand the buffer off to the next stage and mark this ticket released.
     *
     * @param next_stage  Name of the stage taking ownership.
     */
    void release(std::string_view next_stage = "") noexcept;

    [[nodiscard]] uint64_t slot_idx() const noexcept { return slot_idx_; }

private:
    static constexpr size_t kStageLen = 24;
    void*    heatmap_{nullptr};  ///< Actually BufferHeatmap* — cast after type alias is visible
    uint64_t slot_idx_{0};
    bool     released_{false};
    char     stage_[kStageLen]{}; // NOLINT(modernize-avoid-c-arrays)
};

// ─── BufferHeatmap ───────────────────────────────────────────────────────────

/**
 * @brief Zero-copy buffer segment lifecycle tracker.
 *
 * @tparam MaxSlots  Maximum buffer slots tracked (matches pool capacity).
 * @tparam RingCap   Event ring buffer capacity (power of 2).
 */
template<size_t MaxSlots = 4096, size_t RingCap = 65536>
class BufferHeatmapT {
public:
    static_assert((RingCap & (RingCap - 1)) == 0);

    static constexpr uint64_t kDefaultStallThresholdNs = 5'000'000ULL; ///< 5 ms stall detection

    explicit BufferHeatmapT(uint64_t stall_threshold_ns = kDefaultStallThresholdNs)
        : stall_threshold_ns_(stall_threshold_ns) {}

    // ── Slot lifecycle ────────────────────────────────────────────────────────

    /** @brief Record a slot allocation event. */
    void on_alloc(uint64_t slot_idx) noexcept {
        if (slot_idx >= MaxSlots) return;
        SlotRecord& rec = slots_[slot_idx];
        rec.slot_idx      = slot_idx;
        rec.alloc_ns      = now_ns();
        rec.stage_enter_ns= rec.alloc_ns;
        rec.state         = BufferState::Allocated;
        rec.stage_count   = 0;
        rec.stage[0]      = '\0';
        push_event(slot_idx, BufferState::Allocated, "alloc", 0);
        stats_.alloc_count.fetch_add(1, std::memory_order_relaxed);
    }

    /** @brief Record a stage-entry event. */
    void on_stage_enter(uint64_t slot_idx, std::string_view stage) noexcept {
        if (slot_idx >= MaxSlots) return;
        SlotRecord& rec = slots_[slot_idx];
        uint64_t    now = now_ns();
        rec.stage_enter_ns = now;
        rec.state          = BufferState::InStage;
        rec.stage_count++;
        rec.set_stage(stage);
        push_event(slot_idx, BufferState::InStage, stage, now - rec.alloc_ns);
    }

    /** @brief Record a stage-exit / handoff event. */
    void on_stage_exit(uint64_t slot_idx, std::string_view next_stage) noexcept {
        if (slot_idx >= MaxSlots) return;
        SlotRecord& rec = slots_[slot_idx];
        uint64_t    now = now_ns();
        rec.total_hold_ns += (now - rec.stage_enter_ns);
        if (next_stage.empty()) {
            rec.state = BufferState::Allocated;
        } else {
            on_stage_enter(slot_idx, next_stage);
            return;
        }
        push_event(slot_idx, BufferState::Allocated, "", now - rec.stage_enter_ns);
    }

    /** @brief Record a slot release (back to pool). */
    void on_release(uint64_t slot_idx) noexcept {
        if (slot_idx >= MaxSlots) return;
        SlotRecord& rec = slots_[slot_idx];
        uint64_t    now = now_ns();
        rec.total_hold_ns += (now - rec.stage_enter_ns);
        rec.state    = BufferState::Released;
        rec.stage[0] = '\0';
        push_event(slot_idx, BufferState::Released, "release",
                   now - rec.alloc_ns);
        stats_.free_count.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Queries ───────────────────────────────────────────────────────────────

    /**
     * @brief Detect stalled buffers (in a stage longer than stall_threshold_ns).
     *
     * @param out     Output vector of stalled slot records.
     * @param now_ns  Current monotonic time.
     */
    void find_stalls(std::vector<SlotRecord>& out, uint64_t now_ns_val) const noexcept {
        out.clear();
        for (size_t i = 0; i < MaxSlots; ++i) {
            const SlotRecord& rec = slots_[i];
            if (rec.state == BufferState::InStage &&
                rec.stage_enter_ns > 0 &&
                (now_ns_val - rec.stage_enter_ns) > stall_threshold_ns_) {
                out.push_back(rec);
                stats_.stall_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    /**
     * @brief Render an ASCII heatmap of slot states to `out`.
     *
     * Each character represents one slot:
     * - `.` = Free, `A` = Allocated, `S` = InStage, `!` = Stalled, `R` = Released
     */
    void render_ascii(FILE* out = stdout) const noexcept {
        std::fputs("\033[1;36m── Buffer Heatmap ───────────────────────────────\033[0m\n", out);
        const size_t cols = 64;
        for (size_t i = 0; i < MaxSlots; ++i) {
            char c = '.';
            switch (slots_[i].state) {
            case BufferState::Free:      c = '.'; break;
            case BufferState::Allocated: c = 'A'; break;
            case BufferState::InStage:   c = 'S'; break;
            case BufferState::Stalled:   c = '!'; break;
            case BufferState::Released:  c = 'R'; break;
            }
            std::fputc(c, out);
            if ((i + 1) % cols == 0) std::fputc('\n', out);
        }
        std::fputs("\n  . = Free  A = Allocated  S = InStage  ! = Stalled  R = Released\n", out);
        std::fprintf(out, "  Alloc: %llu  Free: %llu  Stalls: %llu\n",
                     static_cast<unsigned long long>(stats_.alloc_count.load()),
                     static_cast<unsigned long long>(stats_.free_count.load()),
                     static_cast<unsigned long long>(stats_.stall_count.load()));
        std::fflush(out);
    }

    // ── Stats ─────────────────────────────────────────────────────────────────

    struct Stats {
        mutable std::atomic<uint64_t> alloc_count{0};
        mutable std::atomic<uint64_t> free_count{0};
        mutable std::atomic<uint64_t> stall_count{0};
        std::atomic<uint64_t>         events_dropped{0};
    };

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    static uint64_t now_ns() noexcept {
        timespec ts{}; ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
    }

    void push_event(uint64_t slot_idx, BufferState state,
                    std::string_view stage, uint64_t dur_ns) noexcept {
        uint64_t t = ring_tail_.load(std::memory_order_relaxed);
        if (t - ring_head_.load(std::memory_order_acquire) >= RingCap) {
            stats_.events_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& ev     = events_[t & (RingCap - 1)];
        ev.slot_idx  = slot_idx;
        ev.timestamp_ns = now_ns();
        ev.duration_ns  = dur_ns;
        ev.state     = state;
        ev.set_stage(stage);
        ring_tail_.store(t + 1, std::memory_order_release);
    }

    std::array<SlotRecord, MaxSlots>  slots_{};
    std::array<BufferEvent, RingCap>  events_{};
    alignas(64) std::atomic<uint64_t> ring_tail_{0};
    alignas(64) std::atomic<uint64_t> ring_head_{0};
    uint64_t                          stall_threshold_ns_;
    Stats                             stats_;
};

/**
 * @brief Default `BufferHeatmap` (4096 slots, 65536-event ring).
 */
using BufferHeatmap = BufferHeatmapT<4096, 65536>;

// ─── HeatmapTicket destructor and release (inline, needs BufferHeatmap def) ──

inline HeatmapTicket::~HeatmapTicket() noexcept {
    if (!released_ && heatmap_ != nullptr)
        release();
}

inline void HeatmapTicket::release(std::string_view next_stage) noexcept {
    if (released_ || heatmap_ == nullptr) return;
    reinterpret_cast<BufferHeatmap*>(heatmap_)->on_stage_exit(slot_idx_, next_stage);
    released_ = true;
}

} // namespace qbuem::tools

/** @} */ // end of qbuem_buffer_heatmap
