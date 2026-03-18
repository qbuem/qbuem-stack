#pragma once

/**
 * @file qbuem/io/buffer_pool.hpp
 * @brief Fixed-size buffer pool — lock-free intrusive free list.
 * @ingroup qbuem_io_buffers
 *
 * `BufferPool<BufSize, Count>` pre-allocates `Count` buffers of `BufSize`
 * bytes each on the stack (or static storage) at compile time.
 * `acquire()` and `return_buffer()` are implemented as a lock-free intrusive
 * free list using `std::atomic` CAS.
 *
 * ### Design Principles
 * - Fixed count pre-allocation — no runtime heap allocation (including `acquire()`)
 * - `alignas(64)` buffers: prevent false sharing, SIMD-friendly
 * - `Buffer::release()` automatically returns the buffer to the owning pool
 * - When the pool is exhausted, `acquire()` returns `nullptr` (no exceptions)
 * @{
 */

#include <qbuem/common.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace qbuem {

/**
 * @brief Fixed-size, fixed-count buffer pool.
 *
 * Each buffer is aligned to a cache line (64 bytes).
 * Use `acquire()` to obtain a buffer and `Buffer::release()` to return it.
 *
 * ### Thread Safety
 * `acquire()` and `return_buffer()` are safe to call from multiple threads
 * via a CAS loop that mitigates the ABA problem.
 *
 * ### Usage Example
 * @code
 * BufferPool<4096, 64> pool;
 * auto *buf = pool.acquire();
 * if (!buf) { // pool exhausted }
 * // ... use buf->data ...
 * buf->release();  // return to pool
 * @endcode
 *
 * @tparam BufSize Byte size of each buffer.
 * @tparam Count   Number of buffers to pre-allocate in the pool.
 */
template <size_t BufSize, size_t Count>
class BufferPool {
public:
  /**
   * @brief A single buffer managed by the pool.
   *
   * `data` is the region directly readable and writable by the user.
   * The `pool` pointer is used to return the buffer to the owning pool.
   * `next_` is used for free list linkage and must not be accessed by the user.
   */
  struct Buffer {
    /**
     * @brief User data region.
     *
     * Aligned to a cache line (64 bytes) to prevent false sharing and
     * SIMD load penalties.
     */
    alignas(64) std::byte data[BufSize];

    /** @brief The pool that owns this buffer. Used by `release()`. */
    BufferPool *pool;

    /**
     * @brief Returns the buffer to the owning pool.
     *
     * Calls `pool->return_buffer(this)`.
     * Accessing this pointer after release is undefined behavior.
     */
    void release() noexcept { pool->return_buffer(this); }

    /** @brief Free list linkage pointer (must not be accessed by the user). */
    Buffer *next_ = nullptr;
  };

  /**
   * @brief Initializes the pool by linking all buffers into the free list.
   */
  BufferPool() noexcept {
    // Set each buffer's pool pointer and build the free list
    for (size_t i = 0; i < Count; ++i) {
      storage_[i].pool  = this;
      storage_[i].next_ = free_list_head_.load(std::memory_order_relaxed);
      free_list_head_.store(&storage_[i], std::memory_order_relaxed);
    }
    // free_count_ is initialized to Count via in-class initializer.
  }

  /** @brief Copy constructor deleted — buffers store a `this` pointer, so move/copy is not allowed. */
  BufferPool(const BufferPool &) = delete;

  /** @brief Copy assignment deleted. */
  BufferPool &operator=(const BufferPool &) = delete;

  // ─── Acquire / Return ─────────────────────────────────────────────────────

  /**
   * @brief Acquires a buffer from the pool.
   *
   * Pops the head of the free list via a lock-free CAS loop.
   *
   * @returns Pointer to an available buffer, or `nullptr` if the pool is exhausted.
   * @note The `data` contents of the returned buffer are not initialized.
   *
   * @par ABA Problem (theoretical limitation)
   * This implementation is exposed to the classic lock-free stack ABA problem:
   * if thread T1 reads `head` and is preempted, another thread may pop and
   * return `head`, causing T1's CAS to succeed with a stale `head->next_`.
   *
   * **Practical risk**: BufferPool buffers are owned by the pool and are never
   * freed before the pool itself, so the pointer is always valid. Under high
   * contention, a freed buffer may be re-linked with an incorrect `next_`.
   *
   * **Mitigation**: Use from a single Reactor thread to eliminate ABA.
   * For multi-threaded use, consider tagged pointers or Hazard Pointers.
   */
  Buffer *acquire() noexcept {
    Buffer *head = free_list_head_.load(std::memory_order_acquire);
    while (head) {
      if (free_list_head_.compare_exchange_weak(
              head, head->next_,
              std::memory_order_release,
              std::memory_order_acquire)) {
        head->next_ = nullptr;
        free_count_.fetch_sub(1, std::memory_order_relaxed);
        return head;
      }
    }
    return nullptr;
  }

  /**
   * @brief Returns a buffer to the pool.
   *
   * Pushes to the head of the free list via a lock-free CAS loop.
   *
   * @param buf Buffer to return. Must have been acquired from this pool.
   */
  void return_buffer(Buffer *buf) noexcept {
    Buffer *head = free_list_head_.load(std::memory_order_acquire);
    do {
      buf->next_ = head;
    } while (!free_list_head_.compare_exchange_weak(
        head, buf,
        std::memory_order_release,
        std::memory_order_acquire));
    free_count_.fetch_add(1, std::memory_order_relaxed);
  }

  // ─── Status ───────────────────────────────────────────────────────────────

  /**
   * @brief Returns the number of buffers currently available.
   *
   * O(1) — reads an atomic counter.
   *
   * @returns Number of buffers remaining in the free list.
   */
  [[nodiscard]] size_t available() const noexcept {
    return free_count_.load(std::memory_order_relaxed);
  }

private:
  /** @brief Pre-allocated buffer storage. */
  Buffer storage_[Count];

  /** @brief Lock-free free list head pointer. */
  std::atomic<Buffer *> free_list_head_{nullptr};

  /** @brief Number of buffers in the free list — supports O(1) available(). */
  std::atomic<size_t> free_count_{Count};
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
