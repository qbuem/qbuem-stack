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

// ─── FixedPoolResource ────────────────────────────────────────────────────────
/**
 * @brief O(1) fixed-size object pool for same-sized allocations.
 *
 * Uses a singly-linked free list embedded within pool slots.
 * All allocations and deallocations are O(1). Not thread-safe by design;
 * use one pool per thread or guard externally.
 *
 * Example:
 *   FixedPoolResource<sizeof(MyCtx), alignof(MyCtx)> pool(256);
 *   void *slot = pool.allocate();   // O(1), returns nullptr if exhausted
 *   pool.deallocate(slot);          // O(1)
 */
template <size_t ObjectSize, size_t Alignment = alignof(std::max_align_t)>
class FixedPoolResource {
  static constexpr size_t kSlotSize =
      (ObjectSize + Alignment - 1) & ~(Alignment - 1);

  static_assert(kSlotSize >= sizeof(void *),
                "ObjectSize must be >= sizeof(void*) to embed the free-list ptr");

  struct AlignedDeleter {
    void operator()(uint8_t *p) const noexcept {
      ::operator delete[](p, std::align_val_t{Alignment});
    }
  };

public:
  explicit FixedPoolResource(size_t capacity)
      : capacity_(capacity),
        storage_(static_cast<uint8_t *>(
            ::operator new[](kSlotSize * capacity, std::align_val_t{Alignment}))) {
    // Build free list: each slot stores a pointer to the next slot.
    for (size_t i = 0; i + 1 < capacity_; ++i) {
      auto *slot = reinterpret_cast<void **>(storage_.get() + i * kSlotSize);
      *slot = storage_.get() + (i + 1) * kSlotSize;
    }
    auto *last = reinterpret_cast<void **>(
        storage_.get() + (capacity_ - 1) * kSlotSize);
    *last = nullptr;
    free_list_ = storage_.get();
  }

  ~FixedPoolResource() = default;

  FixedPoolResource(const FixedPoolResource &) = delete;
  FixedPoolResource &operator=(const FixedPoolResource &) = delete;
  FixedPoolResource(FixedPoolResource &&) = default;
  FixedPoolResource &operator=(FixedPoolResource &&) = default;

  /** @brief Allocate one slot — O(1). Returns nullptr when pool is exhausted. */
  [[nodiscard]] void *allocate() noexcept {
    if (!free_list_) [[unlikely]]
      return nullptr;
    void *slot = free_list_;
    free_list_ = *reinterpret_cast<void **>(free_list_);
    ++used_;
    return slot;
  }

  /** @brief Return a slot to the pool — O(1). */
  void deallocate(void *ptr) noexcept {
    *reinterpret_cast<void **>(ptr) = free_list_;
    free_list_ = ptr;
    --used_;
  }

  size_t capacity()  const noexcept { return capacity_; }
  size_t used()      const noexcept { return used_; }
  size_t available() const noexcept { return capacity_ - used_; }

private:
  size_t   capacity_;
  size_t   used_ = 0;
  void    *free_list_ = nullptr;
  std::unique_ptr<uint8_t[], AlignedDeleter> storage_;
};

} // namespace draco
