#pragma once

/**
 * @file qbuem/buf/lock_free_hash_map.hpp
 * @brief MPMC lock-free hash map using open addressing and atomic CAS.
 * @defgroup qbuem_buf_lfhm LockFreeHashMap
 * @ingroup qbuem_buf
 *
 * ## Design
 * - Open addressing with linear probing for cache-line-friendly access.
 * - Each slot transitions through: Empty -> Busy -> Committed -> Tombstone.
 * - State transitions are managed via `std::atomic<uint8_t>` CAS.
 * - Capacity must be a power of two (enforced at construction).
 *
 * ## Constraints
 * - Key type K must be an unsigned integer (or castable to uint64_t).
 * - Value type V must fit inside a `std::atomic<V>` (trivially copyable, ≤8B).
 * - Key value `0` is reserved as the "empty sentinel".
 *
 * ## Thread Safety
 * - `put()` and `get()` are fully wait-free on a non-full map.
 * - `remove()` uses a tombstone to avoid breaking probe chains.
 * - No ABA hazard for pointer-stable V (use GenerationPool<T> for pointers).
 *
 * @code
 * LockFreeHashMap<uint64_t, uint32_t> map(1024);
 * map.put(42, 100);
 * auto v = map.get(42); // Result<uint32_t>
 * map.remove(42);
 * @endcode
 */

#include <qbuem/common.hpp>

#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <memory>

namespace qbuem {

/**
 * @brief MPMC lock-free open-addressed hash map.
 * @tparam K Key type — must be convertible to/from uint64_t; 0 is reserved.
 * @tparam V Value type — must be trivially copyable and ≤8 bytes.
 */
template <typename K, typename V>
class LockFreeHashMap {
    static_assert(std::is_trivially_copyable_v<K>, "K must be trivially copyable");
    static_assert(std::is_trivially_copyable_v<V>, "V must be trivially copyable");
    static_assert(sizeof(V) <= 8, "V must fit in 8 bytes for atomic store");

    static constexpr uint8_t kEmpty     = 0;
    static constexpr uint8_t kBusy      = 1;
    static constexpr uint8_t kCommitted = 2;
    static constexpr uint8_t kTombstone = 3;

    static constexpr K kEmptyKey = K{0}; // sentinel; callers must not use key 0

    struct alignas(64) Slot {
        std::atomic<uint8_t> state{kEmpty};
        std::atomic<K>       key{kEmptyKey};
        std::atomic<V>       value{};
    };

public:
    /**
     * @brief Construct with the given capacity (rounded up to next power of two).
     * @param capacity Minimum number of slots. Must be ≥ 2.
     */
    explicit LockFreeHashMap(size_t capacity)
        : capacity_(std::bit_ceil(capacity))
        , mask_(capacity_ - 1)
        , slots_(std::make_unique<Slot[]>(capacity_))
    {
        assert(capacity >= 2);
    }

    LockFreeHashMap(const LockFreeHashMap&)            = delete;
    LockFreeHashMap& operator=(const LockFreeHashMap&) = delete;

    /**
     * @brief Insert or update a key-value pair.
     * @param key   Must not be 0.
     * @param value Value to store.
     * @returns true on success, false if the map is full.
     */
    [[nodiscard]] bool put(K key, V value) noexcept {
        assert(key != kEmptyKey && "key 0 is reserved");
        const size_t start = hash(key);
        for (size_t i = 0; i < capacity_; ++i) {
            Slot& s = slots_[(start + i) & mask_];

            uint8_t expected_empty = kEmpty;
            uint8_t expected_tomb  = kTombstone;

            // Try to claim an empty or tombstone slot.
            if (s.state.compare_exchange_strong(
                    expected_empty, kBusy,
                    std::memory_order_acquire, std::memory_order_relaxed)
             || s.state.compare_exchange_strong(
                    expected_tomb, kBusy,
                    std::memory_order_acquire, std::memory_order_relaxed))
            {
                s.key.store(key, std::memory_order_relaxed);
                s.value.store(value, std::memory_order_relaxed);
                s.state.store(kCommitted, std::memory_order_release);
                return true;
            }

            // If the slot already holds our key, update in place.
            if (s.state.load(std::memory_order_acquire) == kCommitted
             && s.key.load(std::memory_order_relaxed) == key)
            {
                s.value.store(value, std::memory_order_release);
                return true;
            }
        }
        return false; // map full
    }

    /**
     * @brief Look up a key.
     * @param key Key to search for.
     * @returns Result containing the value, or errc::no_such_file_or_directory.
     */
    [[nodiscard]] Result<V> get(K key) const noexcept {
        assert(key != kEmptyKey && "key 0 is reserved");
        const size_t start = hash(key);
        for (size_t i = 0; i < capacity_; ++i) {
            const Slot& s = slots_[(start + i) & mask_];
            const uint8_t state = s.state.load(std::memory_order_acquire);

            if (state == kEmpty) break; // probe chain ended
            if (state == kTombstone) continue;
            if (state == kCommitted
             && s.key.load(std::memory_order_relaxed) == key)
            {
                return s.value.load(std::memory_order_relaxed);
            }
        }
        return unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }

    /**
     * @brief Remove a key (marks slot as tombstone).
     * @param key Key to remove.
     * @returns true if the key was found and removed, false if not found.
     */
    [[nodiscard]] bool remove(K key) noexcept {
        assert(key != kEmptyKey && "key 0 is reserved");
        const size_t start = hash(key);
        for (size_t i = 0; i < capacity_; ++i) {
            Slot& s = slots_[(start + i) & mask_];
            const uint8_t state = s.state.load(std::memory_order_acquire);

            if (state == kEmpty) return false;
            if (state == kCommitted
             && s.key.load(std::memory_order_relaxed) == key)
            {
                s.state.store(kTombstone, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    /** @brief Number of allocated slots (power of two). */
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

private:
    [[nodiscard]] size_t hash(K key) const noexcept {
        // FNV-1a-inspired integer mix
        uint64_t h = static_cast<uint64_t>(key);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return static_cast<size_t>(h);
    }

    size_t                    capacity_;
    size_t                    mask_;
    std::unique_ptr<Slot[]>   slots_;
};

} // namespace qbuem
