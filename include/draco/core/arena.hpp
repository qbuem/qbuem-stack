#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace draco {

/**
 * @brief High-performance Arena allocator for per-request memory management.
 *
 * Provides O(1) allocation and O(1) bulk deallocation (reset).
 * This is NOT thread-safe by design; each thread/core should have its own
 * Arena.
 */
class Arena {
public:
  explicit Arena(size_t initial_size = 64 * 1024)
      : current_block_size_(initial_size) {
    allocate_block(current_block_size_);
  }

  ~Arena() = default;

  // Arena is move-only
  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;
  Arena(Arena &&) = default;
  Arena &operator=(Arena &&) = default;

  /**
   * @brief Allocate memory from the arena.
   */
  void *allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
    size_t padding =
        (alignment - (reinterpret_cast<uintptr_t>(current_ptr_) % alignment)) %
        alignment;
    size_t total_size = size + padding;

    if (current_ptr_ + total_size > current_block_end_) {
      // Need a new block
      allocate_block(std::max(size * 2, current_block_size_));
      padding = (alignment -
                 (reinterpret_cast<uintptr_t>(current_ptr_) % alignment)) %
                alignment;
      total_size = size + padding;
    }

    void *result = current_ptr_ + padding;
    current_ptr_ += total_size;
    return result;
  }

  /**
   * @brief Reset the arena, making all previously allocated memory available.
   *
   * Does NOT actually free the memory to the OS; just resets the pointers.
   */
  void reset() {
    if (blocks_.empty())
      return;

    // Keep only the largest block to avoid fragmentation over time,
    // or just reset all to the start of the first block.
    // For simplicity, we'll reset to the start of the first block and keep
    // others for reuse.
    current_block_index_ = 0;
    setup_block(0);
  }

private:
  void allocate_block(size_t size) {
    auto block = std::make_unique<uint8_t[]>(size);
    uint8_t *ptr = block.get();
    blocks_.push_back(std::move(block));
    block_ends_.push_back(ptr + size);

    current_block_index_ = blocks_.size() - 1;
    current_ptr_ = ptr;
    current_block_end_ = block_ends_.back();
  }

  void setup_block(size_t index) {
    current_block_index_ = index;
    current_ptr_ = blocks_[index].get();
    current_block_end_ = block_ends_[index];
  }

  size_t current_block_size_;
  std::vector<std::unique_ptr<uint8_t[]>> blocks_;
  std::vector<uint8_t *> block_ends_;

  size_t current_block_index_ = 0;
  uint8_t *current_ptr_ = nullptr;
  uint8_t *current_block_end_ = nullptr;
};

} // namespace draco
