#ifndef QBUEM_BUF_KQUEUE_BUFFER_POOL_HPP
#define QBUEM_BUF_KQUEUE_BUFFER_POOL_HPP

#include <vector>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <cstdlib>
#include <stdexcept>

namespace qbuem {

/**
 * @brief User-space Buffer Ring for macOS kqueue.
 * Mimics io_uring buffer ring functionality in user-space for zero-allocation I/O.
 * 
 * Each buffer is cache-line aligned (64 bytes) to prevent false sharing and
 * optimize memory controller throughput.
 */
class KqueueBufferPool {
public:
  struct Buffer {
    void* addr;    ///< Cache-aligned buffer address
    uint32_t len;  ///< Buffer length (requested + alignment padding)
    uint16_t bid;  ///< Buffer ID for release()
  };

  /**
   * @brief Create a pool of aligned buffers.
   * @param buffer_size Size of each buffer. Will be rounded up to 64 bytes.
   * @param count Number of buffers in the pool.
   */
  KqueueBufferPool(size_t buffer_size, size_t count)
      : buffer_size_((buffer_size + 63) & ~63), count_(count) {
    if (posix_memalign(&storage_, 64, buffer_size_ * count) != 0) {
        throw std::bad_alloc();
    }
    
    free_indices_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      free_indices_.push_back(static_cast<uint16_t>(i));
    }
    available_count_.store(count, std::memory_order_relaxed);
  }

  ~KqueueBufferPool() {
      if (storage_) free(storage_);
  }

  // Non-copyable
  KqueueBufferPool(const KqueueBufferPool&) = delete;
  KqueueBufferPool& operator=(const KqueueBufferPool&) = delete;

  /**
   * @brief Acquire a buffer from the pool (O(1)).
   * @return Valid Buffer or Buffer with addr=nullptr if pool is empty.
   */
  Buffer acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_indices_.empty()) [[unlikely]] {
      return {nullptr, 0, 0};
    }
    uint16_t bid = free_indices_.back();
    free_indices_.pop_back();
    available_count_.store(free_indices_.size(), std::memory_order_relaxed);
    
    return {
        static_cast<uint8_t*>(storage_) + (bid * buffer_size_),
        static_cast<uint32_t>(buffer_size_),
        bid
    };
  }

  /**
   * @brief Release a buffer back to the pool (O(1)).
   * @param bid The buffer ID from Acquire().
   */
  void release(uint16_t bid) {
    std::lock_guard<std::mutex> lock(mutex_);
    free_indices_.push_back(bid);
    available_count_.store(free_indices_.size(), std::memory_order_relaxed);
  }

  /** @returns Current number of available buffers. */
  size_t available() const noexcept {
    return available_count_.load(std::memory_order_relaxed);
  }

private:
  const size_t buffer_size_;
  const size_t count_;
  void* storage_ = nullptr;
  std::vector<uint16_t> free_indices_;
  std::atomic<size_t> available_count_;
  std::mutex mutex_;
};

} // namespace qbuem

#endif // QBUEM_BUF_KQUEUE_BUFFER_POOL_HPP
