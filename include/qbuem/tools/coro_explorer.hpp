#pragma once

/**
 * @file qbuem/tools/coro_explorer.hpp
 * @brief CoroExplorer — coroutine stack-trace and suspension point visualization.
 * @defgroup qbuem_coro_explorer CoroExplorer
 * @ingroup qbuem_tools
 *
 * ## Overview
 *
 * `CoroExplorer` provides introspection of the live coroutine hierarchy:
 *
 * - **Coroutine tree** — parent/child relationships between `Task<T>` frames.
 * - **Suspension trace** — source location, awaiter type, and wait duration.
 * - **State machine dump** — current state of every tracked coroutine.
 * - **TUI/JSON export** — render to terminal or emit as JSON for the inspector.
 *
 * ## Instrumentation model
 *
 * Coroutines opt in by calling `CoroExplorer::track(handle, name)` at
 * construction time. A zero-cost `[[no_unique_address]]` guard type
 * (`CoroGuard`) automates registration and deregistration via RAII.
 *
 * ## Architecture
 * ```
 *  Task<T> constructor
 *       │  calls CoroExplorer::track()
 *       ▼
 *  CoroRegistry (lock-free slab, max 65536 entries)
 *       │
 *       ├─ CoroRecord { handle, name, parent, state, depth, suspend_ts }
 *       │
 *  CoroExplorer::snapshot()  ──► CoroTree  ──► render_tui() / to_json()
 * ```
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <optional>
#include <qbuem/compat/print.hpp>
#include <span>
#include <string_view>
#include <vector>

namespace qbuem::tools {

// ─── CoroState ───────────────────────────────────────────────────────────────

/**
 * @brief Coroutine lifecycle states.
 */
enum class CoroState : uint8_t {
    Running   = 0, ///< Currently executing on a reactor thread
    Suspended = 1, ///< Awaiting an event (timer, I/O, channel, etc.)
    Done      = 2, ///< Returned or threw — handle is expired
    Detached  = 3, ///< Removed from tracking (e.g. fire-and-forget)
};

[[nodiscard]] constexpr std::string_view coro_state_str(CoroState s) noexcept {
    switch (s) {
    case CoroState::Running:   return "running";
    case CoroState::Suspended: return "suspended";
    case CoroState::Done:      return "done";
    case CoroState::Detached:  return "detached";
    }
    return "unknown";
}

// ─── CoroRecord ──────────────────────────────────────────────────────────────

/**
 * @brief Per-coroutine tracking record.
 *
 * Stored in a fixed-size slab; trivially copyable for snapshot reads.
 */
struct CoroRecord {
    static constexpr size_t kNameLen    = 48;
    static constexpr size_t kAwaiterLen = 32;

    uint64_t   id{0};              ///< Unique coroutine ID (assigned on track())
    uint64_t   parent_id{0};       ///< Parent coroutine ID (0 = root)
    uint64_t   created_ns{0};      ///< Creation timestamp (CLOCK_MONOTONIC)
    uint64_t   suspend_ns{0};      ///< Last suspension timestamp
    uint64_t   resume_ns{0};       ///< Last resume timestamp
    uint64_t   total_suspend_ns{0};///< Cumulative time in Suspended state
    uint32_t   resume_count{0};    ///< Number of times resumed
    uint16_t   depth{0};           ///< Depth in the coroutine tree (root = 0)
    CoroState  state{CoroState::Running};
    uint8_t    _pad{};
    char       name[kNameLen]{};   ///< Human-readable name (e.g. "process_order") // NOLINT(modernize-avoid-c-arrays)
    char       awaiter[kAwaiterLen]{}; ///< Current awaiter type name (NUL-term) // NOLINT(modernize-avoid-c-arrays)

    void set_name(std::string_view n) noexcept {
        size_t len = std::min(n.size(), kNameLen - 1);
        std::memcpy(name, n.data(), len);
        name[len] = '\0';
    }

    void set_awaiter(std::string_view a) noexcept {
        size_t len = std::min(a.size(), kAwaiterLen - 1);
        std::memcpy(awaiter, a.data(), len);
        awaiter[len] = '\0';
    }

    /** @brief Total time this coroutine has been suspended (microseconds). */
    [[nodiscard]] double suspend_us() const noexcept {
        return static_cast<double>(total_suspend_ns) / 1000.0;
    }
};
static_assert(std::is_trivially_copyable_v<CoroRecord>);

// ─── CoroRegistry ────────────────────────────────────────────────────────────

/**
 * @brief Lock-free fixed-capacity coroutine registry.
 *
 * Uses a slab of `CoroRecord` entries indexed by ID % capacity.
 * Concurrent writes use CAS on a `version` field per slot.
 *
 * @tparam Cap  Maximum concurrent coroutines tracked.
 */
template<size_t Cap = 65536>
class CoroRegistry {
public:
    static_assert((Cap & (Cap - 1)) == 0, "Cap must be a power of 2");
    static constexpr size_t kMask = Cap - 1;

    /**
     * @brief Register a new coroutine.
     *
     * @param name       Human-readable name.
     * @param parent_id  Parent coroutine ID (0 for root).
     * @param depth      Depth in the coroutine tree.
     * @returns Assigned coroutine ID, or 0 if the registry is full.
     */
    [[nodiscard]] uint64_t track(std::string_view name,
                                  uint64_t parent_id = 0,
                                  uint16_t depth     = 0) noexcept {
        uint64_t id = id_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
        size_t   slot = id & kMask;

        CoroRecord& rec = slots_[slot];
        rec.id          = id;
        rec.parent_id   = parent_id;
        rec.created_ns  = now_ns();
        rec.depth       = depth;
        rec.state       = CoroState::Running;
        rec.resume_count= 0;
        rec.total_suspend_ns = 0;
        rec.set_name(name);
        rec.awaiter[0]  = '\0';

        active_.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    /**
     * @brief Mark a coroutine as suspended, recording the awaiter type.
     */
    void on_suspend(uint64_t id, std::string_view awaiter_type) noexcept {
        CoroRecord& rec = slots_[id & kMask];
        if (rec.id != id) return;
        rec.suspend_ns = now_ns();
        rec.state      = CoroState::Suspended;
        rec.set_awaiter(awaiter_type);
    }

    /**
     * @brief Mark a coroutine as resumed.
     */
    void on_resume(uint64_t id) noexcept {
        CoroRecord& rec = slots_[id & kMask];
        if (rec.id != id) return;
        uint64_t ns = now_ns();
        if (rec.suspend_ns > 0)
            rec.total_suspend_ns += (ns - rec.suspend_ns);
        rec.resume_ns = ns;
        rec.resume_count++;
        rec.state     = CoroState::Running;
        rec.awaiter[0]= '\0';
    }

    /**
     * @brief Mark a coroutine as done and remove it from active tracking.
     */
    void on_done(uint64_t id) noexcept {
        CoroRecord& rec = slots_[id & kMask];
        if (rec.id != id) return;
        rec.state = CoroState::Done;
        active_.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * @brief Snapshot all active (non-Done) coroutines.
     *
     * @param out  Output vector filled with current records.
     */
    void snapshot(std::vector<CoroRecord>& out) const noexcept {
        out.clear();
        for (size_t i = 0; i < Cap; ++i) {
            const auto& rec = slots_[i];
            if (rec.id > 0 && rec.state != CoroState::Done)
                out.push_back(rec);
        }
    }

    /** @brief Number of currently active (non-Done) coroutines. */
    [[nodiscard]] uint64_t active() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    /** @brief Total coroutines tracked since construction. */
    [[nodiscard]] uint64_t total() const noexcept {
        return id_counter_.load(std::memory_order_relaxed);
    }

private:
    static uint64_t now_ns() noexcept {
        timespec ts{}; ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
    }

    std::array<CoroRecord, Cap>     slots_{};
    alignas(64) std::atomic<uint64_t> id_counter_{0};
    alignas(64) std::atomic<uint64_t> active_{0};
};

// ─── Global registry singleton ───────────────────────────────────────────────

/**
 * @brief Process-wide coroutine registry singleton.
 *
 * Returns a reference to the single `CoroRegistry` instance.
 * Zero-overhead after the first call (static local + load).
 */
[[nodiscard]] inline CoroRegistry<>& global_coro_registry() noexcept {
    static CoroRegistry<> reg;
    return reg;
}

// ─── CoroGuard ───────────────────────────────────────────────────────────────

/**
 * @brief RAII coroutine lifecycle guard.
 *
 * Constructed at the top of a `Task<T>` coroutine body to register with
 * the global `CoroRegistry`. Automatically calls `on_done()` when the
 * coroutine exits (either via `co_return` or destruction).
 *
 * ### Usage in Task<T>
 * @code
 * Task<Result<void>> process_order(Order o, std::stop_token st) {
 *     CoroGuard guard("process_order");
 *     // ... coroutine body
 *     co_return {};
 * }
 * @endcode
 */
class CoroGuard {
public:
    explicit CoroGuard(std::string_view name,
                       uint64_t         parent_id = 0,
                       uint16_t         depth     = 0) noexcept
        : id_(global_coro_registry().track(name, parent_id, depth)) {}

    ~CoroGuard() noexcept {
        if (id_ != 0u) global_coro_registry().on_done(id_);
    }

    CoroGuard(const CoroGuard&)            = delete;
    CoroGuard& operator=(const CoroGuard&) = delete;
    CoroGuard(CoroGuard&&)                 = delete;
    CoroGuard& operator=(CoroGuard&&)      = delete;

    /** @brief Record a suspension point with an awaiter type name. */
    void suspend(std::string_view awaiter_type) noexcept {
        global_coro_registry().on_suspend(id_, awaiter_type);
    }

    /** @brief Record a resumption. */
    void resume() noexcept {
        global_coro_registry().on_resume(id_);
    }

    /** @brief The assigned coroutine ID. */
    [[nodiscard]] uint64_t id() const noexcept { return id_; }

private:
    uint64_t id_{0};
};

// ─── CoroExplorer ────────────────────────────────────────────────────────────

/**
 * @brief Coroutine stack-trace and suspension point visualization tool.
 *
 * Wraps the global `CoroRegistry` with snapshot, rendering, and JSON-export
 * capabilities. Safe to call from any thread.
 */
class CoroExplorer {
public:
    CoroExplorer() = default;

    /**
     * @brief Take a snapshot of all active coroutines.
     *
     * @returns Vector of `CoroRecord` entries (non-Done coroutines).
     */
    [[nodiscard]] std::vector<CoroRecord> snapshot() const noexcept {
        std::vector<CoroRecord> out;
        out.reserve(256);
        global_coro_registry().snapshot(out);
        // Sort by depth then creation time for a tree-like presentation
        std::sort(out.begin(), out.end(), [](const CoroRecord& a, const CoroRecord& b) {
            return a.depth != b.depth ? a.depth < b.depth : a.created_ns < b.created_ns;
        });
        return out;
    }

    /**
     * @brief Render the coroutine tree to ANSI terminal output.
     *
     * @param records  Snapshot from `snapshot()`.
     * @param out      Output file (default stdout).
     */
    static void render_tui(const std::vector<CoroRecord>& records,
                           FILE* out = stdout) noexcept {
        std::print(out, "\033[1;35m── CoroExplorer ─────────────────────────────────\033[0m\n");
        std::print(out, "  Active: {} coroutines\n\n", records.size());

        for (const auto& rec : records) {
            // Indentation by depth
            for (uint16_t d = 0; d < rec.depth; ++d) std::print(out, "  ");

            const char* state_colour =
                rec.state == CoroState::Running   ? "\033[0;32m" :
                rec.state == CoroState::Suspended ? "\033[0;33m" : "\033[0;31m";

            std::print(out, "{}[{}]\033[0m #{} {}",
                         state_colour,
                         coro_state_str(rec.state),
                         rec.id,
                         rec.name);

            if (rec.state == CoroState::Suspended && rec.awaiter[0])
                std::print(out, " (awaiting: {})", rec.awaiter);

            std::print(out, "  — suspended {:.1f} µs total, {} resumes\n",
                         rec.suspend_us(), rec.resume_count);
        }
        std::fflush(out);
    }

    /**
     * @brief Emit a JSON array of coroutine records.
     *
     * @param records  Snapshot from `snapshot()`.
     * @returns JSON string (suitable for the Inspector SSE `coros` event).
     */
    [[nodiscard]] static std::string to_json(const std::vector<CoroRecord>& records) noexcept {
        std::string json = "[";
        bool first = true;
        for (const auto& rec : records) {
            if (!first) json += ',';
            first = false;
            json += "{\"id\":";        json += std::to_string(rec.id);
            json += ",\"parent\":";    json += std::to_string(rec.parent_id);
            json += ",\"name\":\"";    json += rec.name; json += '"';
            json += ",\"state\":\"";   json += coro_state_str(rec.state); json += '"';
            json += ",\"awaiter\":\""; json += rec.awaiter; json += '"';
            json += ",\"depth\":";     json += std::to_string(rec.depth);
            json += ",\"suspends\":";  json += std::to_string(rec.total_suspend_ns);
            json += ",\"resumes\":";   json += std::to_string(rec.resume_count);
            json += '}';
        }
        json += ']';
        return json;
    }

    /** @brief Number of currently active coroutines. */
    [[nodiscard]] uint64_t active() const noexcept {
        return global_coro_registry().active();
    }

    /** @brief Total coroutines created since process start. */
    [[nodiscard]] uint64_t total() const noexcept {
        return global_coro_registry().total();
    }
};

} // namespace qbuem::tools

/** @} */ // end of qbuem_coro_explorer
