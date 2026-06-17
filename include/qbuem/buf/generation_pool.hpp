#pragma once

/**
 * @file qbuem/buf/generation_pool.hpp
 * @brief ABA-safe lock-free object pool with generation-indexed handles.
 * @defgroup qbuem_buf_genpool GenerationPool
 * @ingroup qbuem_buf
 *
 * ## Design
 * Solves the ABA problem by pairing each pool slot with a generation counter.
 * A `Handle` encodes both an index (lower 32 bits) and a generation (upper 32 bits)
 * into a single `uint64_t`. A resolved handle is only valid when the stored
 * generation exactly matches — stale handles resolve to `nullptr`.
 *
 * ## Generation Bit Convention
 * - Even generation  → slot is live (allocated).
 * - Odd generation   → slot is in transition (being freed).
 * The LSB is used as a "busy" flag so that `resolve()` can safely detect
 * mid-release slots without additional synchronisation.
 *
 * ## Capacity
 * Fixed at construction time. The free list is an intrusive LIFO stack
 * implemented with a single 64-bit atomic (index | generation packed word).
 *
 * @code
 * GenerationPool<MyEvent> pool(256);
 * auto [handle, ptr] = pool.acquire();   // returns Handle + T*
 * new (ptr) MyEvent{...};                // placement-construct
 * pool.release(handle);                  // returns slot to pool
 *
 * // Elsewhere:
 * MyEvent* p = pool.resolve(handle);     // nullptr if stale
 * @endcode
 */

#include <qbuem/common.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace qbuem {

/**
 * @brief 64-bit versioned handle into a GenerationPool.
 *
 * Encodes `index` (lower 32 bits) and `gen` (upper 32 bits).
 * A zero-valued Handle is invalid (null handle).
 */
class GenerationHandle {
public:
    static constexpr uint64_t kNull = UINT64_MAX;

    GenerationHandle() noexcept : raw_(kNull) {}
    GenerationHandle(uint32_t index, uint32_t gen) noexcept
        : raw_(static_cast<uint64_t>(gen) << 32 | index) {}

    [[nodiscard]] uint32_t index() const noexcept { return static_cast<uint32_t>(raw_); }
    [[nodiscard]] uint32_t gen()   const noexcept { return static_cast<uint32_t>(raw_ >> 32); }
    [[nodiscard]] bool     valid() const noexcept { return raw_ != kNull; }

    bool operator==(const GenerationHandle&) const noexcept = default;

private:
    uint64_t raw_;
};

/**
 * @brief Lock-free fixed-capacity object pool with ABA-safe handles.
 * @tparam T Object type stored in the pool. Must be default-constructible.
 */
template <typename T>
class GenerationPool {
    static_assert(std::is_default_constructible_v<T>,
                  "T must be default-constructible for pool pre-allocation");

    // Internally the free list stores: upper 32 = generation, lower 32 = index.
    // kNilIdx marks an empty free list.
    static constexpr uint32_t kNilIdx = UINT32_MAX;

    struct Slot {
        alignas(T) std::byte               storage[sizeof(T)];
        std::atomic<uint32_t>              generation{0}; // even=live, odd=transitioning
        uint32_t                           next_free{kNilIdx}; // next in free list (single-threaded init)
    };

    // Free-list head: upper 32 bits = aba tag, lower 32 bits = head index.
    static constexpr uint64_t make_head(uint32_t idx, uint32_t tag) noexcept {
        return static_cast<uint64_t>(tag) << 32 | idx;
    }
    static constexpr uint32_t head_idx(uint64_t h) noexcept { return static_cast<uint32_t>(h); }
    static constexpr uint32_t head_tag(uint64_t h) noexcept { return static_cast<uint32_t>(h >> 32); }

public:
    /** @brief Acquire result: a valid handle and a pointer to raw storage. */
    struct AcquireResult {
        GenerationHandle handle;
        T*               ptr;   ///< Points to uninitialized storage — use placement new.
    };

    /**
     * @brief Construct a pool of the given fixed capacity.
     * @param capacity Number of slots. Must be ≥ 1 and < UINT32_MAX.
     */
    explicit GenerationPool(size_t capacity)
        : capacity_(capacity)
        , slots_(std::make_unique<Slot[]>(capacity))
    {
        assert(capacity >= 1 && capacity < UINT32_MAX);
        // Build the free list in slot order (0 → 1 → 2 → … → capacity-1 → nil).
        for (size_t i = 0; i + 1 < capacity; ++i)
            slots_[i].next_free = static_cast<uint32_t>(i + 1);
        slots_[capacity - 1].next_free = kNilIdx;
        // Pristine slots start at an ODD generation (= free). Acquire bumps to the
        // next EVEN (= live), so `generation & 1 == 0` cleanly identifies live slots
        // — the invariant `for_each_live()` relies on (a pristine slot must not look
        // live). Handles remain opaque; ABA/resolve semantics are unchanged.
        for (size_t i = 0; i < capacity; ++i)
            slots_[i].generation.store(1u, std::memory_order_relaxed);
        head_.store(make_head(0, 0), std::memory_order_relaxed);
    }

    GenerationPool(const GenerationPool&)            = delete;
    GenerationPool& operator=(const GenerationPool&) = delete;

    /**
     * @brief Pop a slot from the free list and return its handle + storage pointer.
     * @returns `std::nullopt` when the pool is exhausted.
     */
    [[nodiscard]] std::optional<AcquireResult> acquire() noexcept {
        uint64_t old_head = head_.load(std::memory_order_acquire);
        while (true) {
            const uint32_t idx = head_idx(old_head);
            if (idx == kNilIdx) return std::nullopt; // exhausted

            Slot& s = slots_[idx];
            const uint32_t next = s.next_free;
            const uint32_t tag  = head_tag(old_head) + 1; // ABA bump

            if (head_.compare_exchange_weak(
                    old_head, make_head(next, tag),
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
            {
                // Mark slot as live: generation becomes even.
                uint32_t gen = s.generation.load(std::memory_order_relaxed);
                gen = (gen + 1) & ~uint32_t{1}; // next even
                s.generation.store(gen, std::memory_order_release);

                return AcquireResult{
                    GenerationHandle{idx, gen},
                    std::launder(reinterpret_cast<T*>(&s.storage))
                };
            }
        }
    }

    /**
     * @brief Return a slot to the pool.
     *
     * The handle's generation is validated before release.
     * Releasing a stale handle is a no-op (safe but non-fatal).
     *
     * @param handle Handle returned by `acquire()`.
     */
    void release(GenerationHandle handle) noexcept {
        if (!handle.valid()) return;
        const uint32_t idx = handle.index();
        assert(idx < capacity_);

        Slot& s = slots_[idx];
        // Validate generation — prevent double-free confusion.
        if (s.generation.load(std::memory_order_acquire) != handle.gen()) return;

        // Mark transitioning (odd).
        s.generation.fetch_add(1, std::memory_order_release);

        // Push back onto the free list.
        uint64_t old_head = head_.load(std::memory_order_relaxed);
        while (true) {
            s.next_free = head_idx(old_head);
            const uint32_t tag = head_tag(old_head) + 1;
            if (head_.compare_exchange_weak(
                    old_head, make_head(idx, tag),
                    std::memory_order_release,
                    std::memory_order_relaxed))
                break;
        }
    }

    /**
     * @brief Resolve a handle to a live object pointer.
     *
     * Returns `nullptr` if the handle is stale (generation mismatch) or null.
     *
     * @param handle Handle to resolve.
     * @returns Pointer to T, or nullptr.
     */
    [[nodiscard]] T* resolve(GenerationHandle handle) noexcept {
        if (!handle.valid()) return nullptr;
        const uint32_t idx = handle.index();
        if (idx >= capacity_) return nullptr;

        Slot& s = slots_[idx];
        const uint32_t cur_gen = s.generation.load(std::memory_order_acquire);
        // Even generation = live; must match the handle's generation exactly.
        if (cur_gen == handle.gen() && (cur_gen & 1u) == 0u)
            return std::launder(reinterpret_cast<T*>(&s.storage));
        return nullptr;
    }

    /**
     * @brief Acquire a slot and construct a `T` in place.
     *
     * Convenience over `acquire()` + placement-new: the slot storage is
     * constructed as `T(args...)`. Pair with `destroy()` (not bare `release()`)
     * so the destructor runs for non-trivially-destructible `T`.
     *
     * @returns `{handle, ptr}` to the constructed object, or `std::nullopt` when full.
     */
    template <class... Args>
    [[nodiscard]] std::optional<AcquireResult> emplace(Args&&... args) {
        auto acq = acquire();
        if (!acq) return std::nullopt;
        ::new (static_cast<void*>(acq->ptr)) T(std::forward<Args>(args)...);
        return acq;
    }

    /**
     * @brief Destroy the object in a slot and return the slot to the pool.
     *
     * The mirror of `emplace()`: runs `~T()` then `release(handle)`. A no-op for a
     * stale/null handle. Use this for non-trivially-destructible `T`.
     */
    void destroy(GenerationHandle handle) noexcept(std::is_nothrow_destructible_v<T>) {
        if (T* p = resolve(handle)) {
            p->~T();
            release(handle);
        }
    }

    /**
     * @brief Invoke `f(GenerationHandle, T&)` for every live slot.
     *
     * For single-threaded or quiescent use: it scans all slots and is NOT atomic
     * with respect to concurrent `acquire()`/`release()`. Iteration is in slot
     * order. Relies on the even-generation-⇒-live invariant established in the
     * constructor.
     */
    template <class F>
    void for_each_live(F&& f) {
        for (size_t i = 0; i < capacity_; ++i) {
            Slot& s = slots_[i];
            const uint32_t gen = s.generation.load(std::memory_order_acquire);
            if ((gen & 1u) == 0u)   // even ⇒ live
                f(GenerationHandle{static_cast<uint32_t>(i), gen},
                  *std::launder(reinterpret_cast<T*>(&s.storage)));
        }
    }

    /** @brief Pool capacity in slots. */
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

private:
    size_t                     capacity_;
    std::unique_ptr<Slot[]>    slots_;
    alignas(64) std::atomic<uint64_t> head_{0};
};

} // namespace qbuem
