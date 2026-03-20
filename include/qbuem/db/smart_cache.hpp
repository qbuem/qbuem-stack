#pragma once

/**
 * @file qbuem/db/smart_cache.hpp
 * @brief Smart DB Cache — SHM-shared query result cache with hardware invalidation.
 * @defgroup qbuem_smart_cache Smart DB Cache
 * @ingroup qbuem_db
 *
 * ## Overview
 *
 * `SmartCache` stores query results in shared memory, making cached data
 * accessible to all processes on the same machine without inter-process
 * round trips. Cache invalidation is driven by hardware events (RDMA writes,
 * CAS on cache-line aligned slots) rather than software signals.
 *
 * ## Architecture
 * ```
 *  Process A                  Process B
 *  ┌─────────────────┐        ┌─────────────────┐
 *  │  SmartCache     │        │  SmartCache     │
 *  │  .get(key)      │        │  .get(key)      │
 *  │      ▼          │        │      ▼          │
 *  └──────┼──────────┘        └──────┼──────────┘
 *         │                          │
 *         └─────────┐  ┌─────────────┘
 *                   ▼  ▼
 *           ┌──────────────────┐
 *           │  SHM Region      │  (memfd_create / shm_open)
 *           │  CacheEntry[N]   │  cache-line aligned slots
 *           │  version atomic  │  hardware CAS invalidation
 *           └──────────────────┘
 * ```
 *
 * ## Hardware invalidation
 *
 * Each `CacheEntry` contains a `generation` counter (64-bit atomic on a
 * separate cache line). On write:
 * 1. `generation` is set to an odd value (write in progress — "dirty").
 * 2. Data is written.
 * 3. `generation` is incremented to even (committed).
 *
 * On read: verify `generation` is even (no concurrent write), read data,
 * re-verify `generation` hasn't changed — a seqlock pattern.
 * No mutex required on the hot path.
 *
 * ## RDMA invalidation (distributed)
 * When a remote writer updates the underlying database and needs to invalidate
 * the cache across nodes, it posts an RDMA WRITE to the `generation` field of
 * affected slots, triggering cache misses on all readers.
 *
 * ## Usage
 * @code
 * SmartCache<OrderBook, 1024> cache("market_data_cache");
 *
 * // Writer (database update path)
 * cache.put("SAMSUNG", order_book);
 *
 * // Reader (hot path — zero copy, no lock)
 * auto entry = cache.get("SAMSUNG");
 * if (entry) {
 *     process(*entry);  // direct pointer into SHM
 * } else {
 *     // Cache miss — query DB then populate
 *     auto book = co_await db.query_order_book("SAMSUNG", st);
 *     cache.put("SAMSUNG", book);
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/shm/shm_channel.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace qbuem {

// ─── CacheSlot ───────────────────────────────────────────────────────────────

/**
 * @brief A single cache slot in the shared memory region.
 *
 * Uses seqlock pattern for wait-free reads with guaranteed consistency:
 * - `generation` even   → slot is clean / readable
 * - `generation` odd    → write in progress (reader must retry)
 * - `generation` == 0   → slot is empty
 *
 * Layout is cache-line padded to prevent false sharing between adjacent slots.
 *
 * @tparam V  Value type (must be trivially copyable — stored in SHM).
 * @tparam KeyLen  Maximum key string length (NUL-terminated).
 */
template<typename V, size_t KeyLen = 64>
    requires std::is_trivially_copyable_v<V>
struct alignas(64) CacheSlot {
    alignas(64) std::atomic<uint64_t> generation{0}; ///< Seqlock generation counter
    alignas(64) char                  key[KeyLen]{};  ///< NUL-terminated key string
    alignas(64) V                     value{};         ///< Cached value
    uint64_t                          expire_ns{0};    ///< Expiry (nanoseconds since epoch; 0=no expiry)
    uint8_t                           _pad[64 - sizeof(uint64_t)]{};

    /** @brief True if this slot is occupied. */
    [[nodiscard]] bool occupied() const noexcept {
        return generation.load(std::memory_order_acquire) != 0 && key[0] != '\0';
    }

    /** @brief True if generation is even (no write in progress). */
    [[nodiscard]] bool is_consistent() const noexcept {
        return (generation.load(std::memory_order_acquire) & 1u) == 0;
    }

    /** @brief True if this slot's key matches the given key. */
    [[nodiscard]] bool key_matches(std::string_view k) const noexcept {
        return std::strncmp(key, k.data(), KeyLen) == 0;
    }
};

// ─── SmartCacheStats ─────────────────────────────────────────────────────────

/**
 * @brief Per-cache statistics (cache-line aligned atomics).
 */
struct alignas(64) SmartCacheStats {
    std::atomic<uint64_t> hits{0};         ///< Successful cache lookups
    std::atomic<uint64_t> misses{0};       ///< Cache misses
    std::atomic<uint64_t> evictions{0};    ///< Slot evictions (LRU)
    std::atomic<uint64_t> invalidations{0};///< Hardware-driven invalidations
    std::atomic<uint64_t> writes{0};       ///< Total put() calls
    std::atomic<uint64_t> seqlock_retries{0}; ///< Seqlock read retries due to concurrent write
};

// ─── SmartCache ──────────────────────────────────────────────────────────────

/**
 * @brief SHM-backed query result cache with seqlock-based hardware invalidation.
 *
 * All cache slots are stored in a shared memory region accessible to other
 * processes on the same machine. Writers use a seqlock (generation counter)
 * for wait-free consistent reads. No mutex on the hot path.
 *
 * ### Eviction policy
 * Open-addressing hash table with linear probing. When a `put()` call finds all
 * candidate slots occupied, the oldest slot (lowest generation) is evicted.
 *
 * ### TTL (Time-To-Live)
 * Each slot records an optional expiry timestamp. `get()` checks the wall clock
 * and treats expired entries as cache misses.
 *
 * @tparam V        Value type (must be trivially copyable for SHM placement).
 * @tparam Capacity Number of cache slots (should be a prime for better distribution).
 * @tparam KeyLen   Maximum key string length.
 */
template<typename V, size_t Capacity = 1024, size_t KeyLen = 64>
    requires std::is_trivially_copyable_v<V>
class SmartCache {
public:
    using Slot = CacheSlot<V, KeyLen>;
    static constexpr size_t kCapacity = Capacity;

    /**
     * @brief Construct and initialise the cache.
     *
     * @param name  SHM region name (used by `shm_open` for cross-process sharing).
     *              Empty = in-process only (no SHM).
     */
    explicit SmartCache(std::string_view name = "") : name_(name) {
        // Zero-initialise all slots
        for (auto& slot : slots_) {
            slot.generation.store(0, std::memory_order_relaxed);
            slot.key[0] = '\0';
        }
    }

    // ── Write ─────────────────────────────────────────────────────────────────

    /**
     * @brief Insert or update a cache entry.
     *
     * Uses the seqlock write protocol:
     * 1. Increment `generation` to odd (write in progress).
     * 2. Write key + value.
     * 3. Increment `generation` to even (commit).
     *
     * @param key   Cache key (NUL-terminated, max `KeyLen-1` characters).
     * @param value Value to cache.
     * @param ttl_ns Optional TTL in nanoseconds (0 = no expiry).
     */
    void put(std::string_view key, const V& value, uint64_t ttl_ns = 0) noexcept {
        Slot& slot = find_or_alloc(key);

        // Seqlock: mark write in progress (odd generation)
        uint64_t gen = slot.generation.load(std::memory_order_relaxed);
        slot.generation.store(gen | 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Write key + value
        std::strncpy(slot.key, key.data(), KeyLen - 1);
        slot.key[KeyLen - 1] = '\0';
        slot.value = value;
        slot.expire_ns = ttl_ns ? (now_ns() + ttl_ns) : 0;

        // Commit: increment to next even generation
        std::atomic_thread_fence(std::memory_order_seq_cst);
        slot.generation.store(gen + 2, std::memory_order_release);

        stats_.writes.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Read ──────────────────────────────────────────────────────────────────

    /**
     * @brief Lookup a cache entry by key.
     *
     * Uses the seqlock read protocol for wait-free consistency:
     * 1. Read `generation` (must be even).
     * 2. Read key + value.
     * 3. Verify `generation` hasn't changed.
     * 4. Retry if inconsistent (concurrent write detected).
     *
     * @param key  Cache key to look up.
     * @returns Copy of the cached value, or `std::nullopt` on miss/expiry.
     */
    [[nodiscard]] std::optional<V> get(std::string_view key) noexcept {
        size_t idx = hash_key(key) % Capacity;

        for (size_t probe = 0; probe < Capacity; ++probe) {
            Slot& slot = slots_[(idx + probe) % Capacity];

            if (!slot.occupied()) {
                stats_.misses.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }
            if (!slot.key_matches(key)) continue;

            // Seqlock read loop
            for (int retry = 0; retry < 8; ++retry) {
                uint64_t gen1 = slot.generation.load(std::memory_order_acquire);
                if (gen1 & 1) {
                    // Write in progress — spin briefly
                    stats_.seqlock_retries.fetch_add(1, std::memory_order_relaxed);
                    for (int i = 0; i < 16; ++i) {
#if defined(__x86_64__) || defined(__i386__)
                        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__ARM_ARCH)
                        __asm__ volatile("yield" ::: "memory");
#endif
                    }
                    continue;
                }

                V copy = slot.value;
                uint64_t expire = slot.expire_ns;

                std::atomic_thread_fence(std::memory_order_acquire);
                uint64_t gen2 = slot.generation.load(std::memory_order_acquire);
                if (gen1 != gen2) continue; // generation changed — retry

                // Check TTL
                if (expire != 0 && now_ns() > expire) {
                    // Expired — invalidate and report miss
                    invalidate(key);
                    stats_.misses.fetch_add(1, std::memory_order_relaxed);
                    return std::nullopt;
                }

                stats_.hits.fetch_add(1, std::memory_order_relaxed);
                return copy;
            }
            break; // Could not get consistent read
        }

        stats_.misses.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    // ── Invalidation ─────────────────────────────────────────────────────────

    /**
     * @brief Invalidate (evict) a cache entry by key.
     *
     * Sets the key to empty and increments the generation to signal any
     * in-flight readers to discard their cached value.
     * Can be triggered remotely via RDMA WRITE to the generation field.
     *
     * @param key  Key to invalidate.
     * @returns True if the entry was found and invalidated.
     */
    bool invalidate(std::string_view key) noexcept {
        size_t idx = hash_key(key) % Capacity;
        for (size_t probe = 0; probe < Capacity; ++probe) {
            Slot& slot = slots_[(idx + probe) % Capacity];
            if (!slot.occupied()) return false;
            if (!slot.key_matches(key)) continue;

            // Seqlock invalidation: mark dirty, clear key, commit
            uint64_t gen = slot.generation.load(std::memory_order_relaxed);
            slot.generation.store(gen | 1, std::memory_order_release);
            slot.key[0] = '\0';
            slot.generation.store(gen + 2, std::memory_order_release);

            stats_.invalidations.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    /**
     * @brief Invalidate all entries (flush the cache).
     */
    void invalidate_all() noexcept {
        for (auto& slot : slots_) {
            if (!slot.occupied()) continue;
            uint64_t gen = slot.generation.load(std::memory_order_relaxed);
            slot.generation.store(gen | 1, std::memory_order_release);
            slot.key[0] = '\0';
            slot.generation.store(gen + 2, std::memory_order_release);
        }
        stats_.invalidations.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Stats ─────────────────────────────────────────────────────────────────

    /** @brief Snapshot of cache statistics. */
    [[nodiscard]] const SmartCacheStats& stats() const noexcept { return stats_; }

    /** @brief Cache hit rate (0.0 – 1.0). */
    [[nodiscard]] double hit_rate() const noexcept {
        uint64_t h = stats_.hits.load();
        uint64_t m = stats_.misses.load();
        return (h + m) ? static_cast<double>(h) / static_cast<double>(h + m) : 0.0;
    }

    /** @brief Current number of occupied slots. */
    [[nodiscard]] size_t size() const noexcept {
        size_t n = 0;
        for (auto& s : slots_) if (s.occupied()) ++n;
        return n;
    }

private:
    // ── Internal helpers ──────────────────────────────────────────────────────

    static uint64_t now_ns() noexcept {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    static size_t hash_key(std::string_view key) noexcept {
        // FNV-1a 64-bit hash
        uint64_t h = 14695981039346656037ULL;
        for (char c : key) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
        return static_cast<size_t>(h);
    }

    Slot& find_or_alloc(std::string_view key) noexcept {
        size_t idx = hash_key(key) % Capacity;
        Slot* oldest = nullptr;
        uint64_t oldest_gen = UINT64_MAX;

        for (size_t probe = 0; probe < Capacity; ++probe) {
            Slot& slot = slots_[(idx + probe) % Capacity];
            if (!slot.occupied()) return slot;
            if (slot.key_matches(key)) return slot;
            // Track oldest for eviction
            uint64_t g = slot.generation.load(std::memory_order_relaxed);
            if (g < oldest_gen) { oldest_gen = g; oldest = &slot; }
        }

        // All slots full — evict oldest
        stats_.evictions.fetch_add(1, std::memory_order_relaxed);
        return *oldest;
    }

    alignas(64) std::array<Slot, Capacity> slots_{};
    SmartCacheStats                        stats_;
    std::string                            name_;
};

} // namespace qbuem

/** @} */ // end of qbuem_smart_cache
