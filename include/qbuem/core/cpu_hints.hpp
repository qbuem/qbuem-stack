#pragma once

/**
 * @file qbuem/core/cpu_hints.hpp
 * @brief CPU performance hints — Prefetch, Cache-line, connection struct preload
 * @defgroup qbuem_cpu_hints CPU Hints
 * @ingroup qbuem_core
 *
 * Informs the compiler and CPU about memory access patterns to reduce cache misses.
 *
 * ## Usage Examples
 * ```cpp
 * // Preload the next connection struct into cache
 * Connection* next = ring.peek_next();
 * qbuem::prefetch_read(next);
 *
 * // Write-purpose prefetch (preload write buffer)
 * qbuem::prefetch_write(output_buf);
 *
 * // Cache-line aligned struct
 * struct alignas(qbuem::kCacheLineSize) ReactorState {
 *     std::atomic<int> state;
 * };
 * ```
 *
 * @{
 */

#include <cstddef>
#include <cstdint>

namespace qbuem {

// ---------------------------------------------------------------------------
// Cache line size
// ---------------------------------------------------------------------------

/**
 * @brief Typical x86-64 / ARM64 cache line size (64 bytes).
 *
 * Uses `std::hardware_destructive_interference_size` if available,
 * otherwise defaults to 64.
 */
#ifdef __cpp_lib_hardware_interference_size
#  include <new>
inline constexpr size_t kCacheLineSize =
    std::hardware_destructive_interference_size;
#else
inline constexpr size_t kCacheLineSize = 64;
#endif

// ---------------------------------------------------------------------------
// Prefetch hints
// ---------------------------------------------------------------------------

/**
 * @brief Read-purpose prefetch — preloads data into cache.
 *
 * Uses `__builtin_prefetch(ptr, 0, locality)`.
 *
 * @tparam Locality Cache locality hint:
 *   - 0: no temporal locality (kept in cache only briefly)
 *   - 1: low temporal locality
 *   - 2: moderate temporal locality
 *   - 3: high temporal locality (default — kept in L1)
 * @param ptr  Memory address to prefetch.
 */
template <int Locality = 3>
inline void prefetch_read(const void* ptr) noexcept {
  __builtin_prefetch(ptr, 0, Locality);
}

/**
 * @brief Write-purpose prefetch — preloads write buffer into cache.
 *
 * Uses `__builtin_prefetch(ptr, 1, locality)`.
 *
 * @tparam Locality Cache locality hint (same as prefetch_read).
 * @param ptr  Memory address to prefetch.
 */
template <int Locality = 3>
inline void prefetch_write(void* ptr) noexcept {
  __builtin_prefetch(ptr, 1, Locality);
}

/**
 * @brief Prefetches the next N items ahead in a connection struct array.
 *
 * Used in accept loops and similar patterns to preload upcoming connection
 * structs into cache before they are needed.
 *
 * ```cpp
 * for (int i = 0; i < n; ++i) {
 *     prefetch_ahead(connections, i, n);
 *     process(connections[i]);
 * }
 * ```
 *
 * @tparam T     Connection struct type.
 * @tparam Ahead Number of elements ahead to prefetch (default 4).
 * @param  arr   Array pointer.
 * @param  cur   Current index.
 * @param  count Total length of the array.
 */
template <typename T, int Ahead = 4>
inline void prefetch_ahead(const T* arr, size_t cur, size_t count) noexcept {
  size_t next = cur + static_cast<size_t>(Ahead);
  if (next < count) {
    prefetch_read<1>(&arr[next]);
  }
}

// ---------------------------------------------------------------------------
// Compiler hint macros
// ---------------------------------------------------------------------------

/**
 * @brief Informs the compiler that the condition is likely true.
 */
#define QBUEM_LIKELY(x)   __builtin_expect(!!(x), 1)

/**
 * @brief Informs the compiler that the condition is likely false.
 */
#define QBUEM_UNLIKELY(x) __builtin_expect(!!(x), 0)

/**
 * @brief Informs the compiler that this function is rarely called (cold path).
 */
#define QBUEM_COLD __attribute__((cold))

/**
 * @brief Informs the compiler that this function is frequently called (hot path).
 */
#define QBUEM_HOT  __attribute__((hot))

// ---------------------------------------------------------------------------
// Cache-line alignment helper
// ---------------------------------------------------------------------------

/**
 * @brief Places two atomic variables on separate cache lines to prevent false sharing.
 *
 * ```cpp
 * struct Counter {
 *     alignas(kCacheLineSize) std::atomic<uint64_t> producer_count{0};
 *     alignas(kCacheLineSize) std::atomic<uint64_t> consumer_count{0};
 * };
 * ```
 */
struct CacheLinePad {
  char pad[kCacheLineSize];
};

// ---------------------------------------------------------------------------
// Memory barriers
// ---------------------------------------------------------------------------

/**
 * @brief Compiler memory barrier — prevents compiler instruction reordering only.
 *
 * Does not prevent CPU-level reordering. For CPU barriers use
 * `std::atomic_thread_fence(std::memory_order_seq_cst)`.
 */
inline void compiler_barrier() noexcept {
  asm volatile("" ::: "memory");
}

/**
 * @brief CPU pause instruction — used in spin-wait loops.
 *
 * x86: `PAUSE`, ARM: `YIELD`, other: no-op.
 * Reduces spinlock contention in hyperthreading environments.
 */
inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#else
  compiler_barrier();
#endif
}

} // namespace qbuem

/** @} */
