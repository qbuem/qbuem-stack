#pragma once

/**
 * @file qbuem/core/huge_pages.hpp
 * @brief mmap(MAP_HUGETLB)-based huge page buffer pool.
 * @ingroup qbuem_memory
 *
 * This header provides a high-performance buffer pool that uses huge pages
 * to minimize TLB misses.
 *
 * ## Design Principles
 * - Attempts to map huge pages (2 MiB or 1 GiB) via `MAP_HUGETLB`.
 * - Falls back to ordinary `mmap(MAP_ANONYMOUS)` on `ENOMEM` or `EPERM`.
 * - Manages buffers with a free-list for O(1) acquire/release.
 * - Thread safety is provided via `std::mutex`.
 *
 * ## Platform Support
 * - Linux: `mmap(MAP_HUGETLB)` or `mmap(MAP_ANONYMOUS)` fallback.
 * - Non-Linux: `new std::byte[N * Count]` fallback.
 *
 * ## Usage Example
 * @code
 * // Create a pool of 8 buffers each 2 MiB in size
 * qbuem::HugeBufferPool<2 * 1024 * 1024, 8> pool;
 *
 * auto buf = pool.acquire(); // returns std::span<std::byte>
 * if (!buf.empty()) {
 *     // use buffer
 *     pool.release(buf);     // return to pool
 * }
 * @endcode
 *
 * @{
 */

#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

#if defined(__linux__)
#  include <cerrno>
#  include <sys/mman.h>
#endif

namespace qbuem {

/**
 * @brief mmap(MAP_HUGETLB)-based huge page buffer pool.
 *
 * Allocates `Count` fixed-size buffers in a single huge-page mmap region and
 * manages them with a free-list. Buffers are borrowed with `acquire()` and
 * returned with `release()`.
 *
 * ### Memory Layout
 * The entire `N * Count` bytes are allocated as one contiguous mmap region.
 * Each buffer slot is aligned on `N`-byte boundaries:
 * ```
 * [buffer 0 | buffer 1 | ... | buffer Count-1]
 * <- N bytes ->← N bytes ->
 * ```
 *
 * ### Huge Page Fallback Strategy
 * 1. Attempt `mmap(MAP_HUGETLB)`.
 * 2. If `errno == ENOMEM` or `errno == EPERM`, retry with `mmap(MAP_ANONYMOUS)`.
 * 3. On non-Linux platforms, allocate with `new std::byte[]`.
 *
 * ### Thread Safety
 * `acquire()` and `release()` are protected by an internal `std::mutex`.
 *
 * @tparam N     Size of one buffer (bytes). Must be > 0.
 * @tparam Count Total number of buffers. Must be > 0.
 *
 * @note Not copyable or movable. The pool has a fixed lifetime.
 * @warning Passing a span to `release()` that was not obtained from this pool
 *          results in undefined behavior.
 */
template <std::size_t N, std::size_t Count>
class HugeBufferPool {
  static_assert(N > 0,     "Buffer size N must be greater than 0.");
  static_assert(Count > 0, "Buffer count Count must be greater than 0.");

public:
  /**
   * @brief Construct the pool and allocate memory via mmap.
   *
   * On Linux, first attempts `MAP_HUGETLB` for huge page allocation;
   * falls back to ordinary anonymous mmap on failure.
   * On non-Linux platforms, uses `new std::byte[]`.
   *
   * After initialization, all `Count` buffers are registered in the free-list.
   *
   * @throws std::bad_alloc if mmap or new allocation fails completely.
   */
  HugeBufferPool() {
    base_ = map_memory();
    // Add all buffer slots to the free-list.
    free_list_.reserve(Count);
    for (std::size_t i = 0; i < Count; ++i) {
      free_list_.push_back(base_ + i * N);
    }
  }

  /**
   * @brief Destroy the pool and release the mmap region.
   *
   * On Linux, releases the entire `N * Count` bytes via `munmap`.
   * On non-Linux platforms, releases via `delete[]`.
   *
   * @warning All memory is released even if buffers have not been returned.
   *          It is recommended to `release()` all buffers before destroying the pool.
   */
  ~HugeBufferPool() noexcept {
    if (base_ == nullptr) return;
#if defined(__linux__)
    ::munmap(base_, N * Count);
#else
    delete[] base_;
#endif
  }

  /** @brief Copy construction disabled — pool has exclusive ownership. */
  HugeBufferPool(const HugeBufferPool &) = delete;
  /** @brief Copy assignment disabled — pool has exclusive ownership. */
  HugeBufferPool &operator=(const HugeBufferPool &) = delete;
  /** @brief Move construction disabled — mutex and pointer state are complex. */
  HugeBufferPool(HugeBufferPool &&) = delete;
  /** @brief Move assignment disabled — mutex and pointer state are complex. */
  HugeBufferPool &operator=(HugeBufferPool &&) = delete;

  /**
   * @brief Acquire one buffer from the pool (O(1), thread-safe).
   *
   * Pops a buffer slot from the free-list and returns it as `std::span<std::byte>`.
   * Returns an empty span (`empty() == true`) if the pool is exhausted.
   *
   * @returns `std::span<std::byte>` of size `N`.
   *          Empty span if the pool is exhausted.
   *
   * @note The lifetime of the returned span depends on the pool object's lifetime.
   *       The span is invalidated when the pool is destroyed.
   * @note `[[nodiscard]]` — ignoring the return value causes a compile-time warning.
   */
  [[nodiscard]] std::span<std::byte> acquire() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_list_.empty()) {
      return {}; // return empty span — pool exhausted
    }
    std::byte *ptr = free_list_.back();
    free_list_.pop_back();
    return {ptr, N};
  }

  /**
   * @brief Return an acquired buffer to the pool (O(1), thread-safe).
   *
   * Adds the span obtained from `acquire()` back to the free-list.
   * Passing an empty span is a no-op.
   *
   * @param buf `std::span<std::byte>` obtained from `acquire()`.
   *             Passing a span whose size is not `N` results in undefined behavior.
   *
   * @warning Passing a span not obtained from this pool results in undefined behavior.
   */
  void release(std::span<std::byte> buf) noexcept {
    if (buf.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    free_list_.push_back(buf.data());
  }

  /**
   * @brief Return the number of buffers currently available in the free-list.
   *
   * The returned value is a snapshot at the time of the call and may be
   * immediately stale in a multi-threaded environment.
   *
   * @returns Number of buffers remaining in the free-list (0 ~ Count).
   */
  [[nodiscard]] std::size_t available() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_list_.size();
  }

  /**
   * @brief Return the total number of buffers in the pool.
   *
   * Equal to the template parameter `Count` specified at construction.
   *
   * @returns `Count`.
   */
  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Count;
  }

private:
  /**
   * @brief Allocate `N * Count` bytes of memory appropriate for the platform.
   *
   * Linux: attempts `mmap(MAP_HUGETLB)` first, then falls back to `mmap(MAP_ANONYMOUS)`.
   * Non-Linux: `new std::byte[N * Count]`.
   *
   * @returns Pointer to the start of the allocated memory (`std::byte*`).
   * @throws std::bad_alloc if all allocation attempts fail.
   */
  static std::byte *map_memory() {
    static constexpr std::size_t kTotalBytes = N * Count;

#if defined(__linux__)
    // First attempt: MAP_HUGETLB (huge pages)
    void *ptr = ::mmap(
        nullptr,
        kTotalBytes,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
        -1,
        0);

    if (ptr != MAP_FAILED) {
      return static_cast<std::byte *>(ptr);
    }

    // Fall back to ordinary anonymous mmap on ENOMEM or EPERM
    if (errno == ENOMEM || errno == EPERM) {
      ptr = ::mmap(
          nullptr,
          kTotalBytes,
          PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS,
          -1,
          0);

      if (ptr != MAP_FAILED) {
        return static_cast<std::byte *>(ptr);
      }
    }

    throw std::bad_alloc{};
#else
    // Non-Linux: new[] fallback
    return new std::byte[kTotalBytes];
#endif
  }

  /** @brief Start pointer of the memory allocated via mmap (or new[]). */
  std::byte *base_ = nullptr;

  /**
   * @brief Free-list holding pointers to available buffer slots.
   *
   * Each element points to `base_ + i * N`.
   * `acquire()` pops the last element; `release()` appends to the end.
   */
  std::vector<std::byte *> free_list_;

  /** @brief Mutex protecting `acquire()` and `release()` calls. */
  mutable std::mutex mutex_;
};

} // namespace qbuem

/** @} */
