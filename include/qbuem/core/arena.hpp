#pragma once

/**
 * @file qbuem/core/arena.hpp
 * @brief High-performance memory management: Arena allocator and FixedPoolResource.
 * @ingroup qbuem_memory
 *
 * This header provides two core memory management strategies for qbuem-stack:
 *
 * 1. **Arena**: A bump-pointer allocator for variable-size objects.
 *    - Allocation: O(1) (pointer increment only)
 *    - Deallocation: O(1) (bulk release via reset(); no individual frees)
 *    - Use case: Batch management of short-lived objects with a clear lifetime,
 *      such as HTTP request processing.
 *
 * 2. **FixedPoolResource**: A free-list pool allocator for fixed-size objects.
 *    - Allocation and deallocation: O(1) (linked-list head pointer manipulation only)
 *    - Use case: Repeated allocation/deallocation of same-size objects
 *      (e.g., Connection, coroutine frames).
 *
 * ### Thread Safety
 * Both classes are **not thread-safe**. This is intentional by design:
 * in a Shared-Nothing architecture each thread/core owns an independent allocator.
 * External synchronization must be provided if sharing across threads is required.
 */

/**
 * @defgroup qbuem_memory Memory Management
 * @brief Arena and fixed-size pool allocators.
 *
 * Using these allocators instead of traditional `new`/`delete`:
 * - Avoids lock contention from malloc.
 * - Significantly reduces memory fragmentation.
 * - Enables O(1) bulk deallocation of objects with the same lifetime.
 * @{
 */

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace qbuem {

/**
 * @brief Bump-pointer Arena allocator for variable-size objects.
 *
 * Internally manages a list of fixed-size blocks. Each block is filled
 * sequentially using bump-pointer allocation. When the current block is
 * exhausted a new block is dynamically allocated.
 *
 * ### O(1) Allocation Strategy
 * Only pointer arithmetic is performed during allocation:
 * 1. Compute alignment padding (bitwise operations)
 * 2. Advance `current_ptr_`
 * 3. If the block boundary is exceeded, allocate a new block and retry
 *
 * ### Lifetime Management
 * Destructors of objects in the Arena are not called automatically.
 * Either call destructors explicitly, or restrict use to types that do not
 * require destruction.
 *
 * ### Usage Example
 * @code
 * qbuem::Arena arena(64 * 1024); // 64 KiB initial block
 *
 * // Allocate from the Arena
 * auto *buf = static_cast<uint8_t *>(arena.allocate(1024));
 *
 * // Bulk release after HTTP request processing
 * arena.reset(); // resets the pointer without returning memory to the OS
 * @endcode
 *
 * @note Arena supports move but not copy.
 *       Create an independent Arena per thread.
 * @warning Individual object deallocation (`free`) is not supported.
 *          Either `reset()` the entire Arena or destroy the Arena object itself.
 */
class Arena {
public:
  /**
   * @brief Construct an Arena with the specified initial block size.
   *
   * The first block is allocated immediately. Additional blocks are added
   * automatically when the current block runs out of space.
   *
   * @param initial_size Size of the first block (bytes). Default: 64 KiB.
   */
  explicit Arena(size_t initial_size = 64 * 1024)
      : current_block_size_(initial_size) {
    allocate_block(current_block_size_);
  }

  /** @brief Release all owned blocks. */
  ~Arena() = default;

  /** @brief Copy construction is disabled — Arena has exclusive ownership. */
  Arena(const Arena &) = delete;
  /** @brief Copy assignment is disabled — Arena has exclusive ownership. */
  Arena &operator=(const Arena &) = delete;
  /** @brief Move construction is allowed. The source Arena becomes empty. */
  Arena(Arena &&) = default;
  /** @brief Move assignment is allowed. The source Arena becomes empty. */
  Arena &operator=(Arena &&) = default;

  /**
   * @brief Allocate memory from the Arena (O(1) amortized).
   *
   * If the current block has space, the pointer is advanced and memory is
   * returned immediately. Otherwise a new block is allocated first.
   *
   * Alignment is satisfied automatically according to the `alignment` parameter.
   *
   * @param size      Requested allocation size (bytes).
   * @param alignment Requested alignment. Default: platform maximum alignment.
   * @returns Pointer to the allocated memory. Never returns nullptr.
   * @throws std::bad_alloc if system memory is exhausted (new block allocation fails).
   *
   * @note The lifetime of the returned pointer is tied to the Arena.
   *       The pointer is invalidated when the Arena is `reset()` or destroyed.
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
   * @brief Reset the Arena making all memory available for reuse (O(1)).
   *
   * Does not return memory to the OS. Only the internal bump-pointer is
   * reset to the start of the first block. All previously allocated pointers
   * are invalidated.
   *
   * This is efficient for repetitive short-lived allocation patterns such as
   * HTTP request/response cycles, recycling memory without OS deallocation cost.
   *
   * @warning All pointers previously allocated from this Arena become unusable
   *          after reset(). Beware of dangling pointer access.
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
  /**
   * @brief Allocate a new memory block and set it as the current block.
   * @param size Size of the new block (bytes).
   */
  void allocate_block(size_t size) {
    auto block = std::make_unique<uint8_t[]>(size);
    uint8_t *ptr = block.get();
    blocks_.push_back(std::move(block));
    block_ends_.push_back(ptr + size);

    current_block_index_ = blocks_.size() - 1;
    current_ptr_ = ptr;
    current_block_end_ = block_ends_.back();
  }

  /**
   * @brief Set the block at the given index as the current block.
   * @param index Index of the block to activate (relative to blocks_ vector).
   */
  void setup_block(size_t index) {
    current_block_index_ = index;
    current_ptr_ = blocks_[index].get();
    current_block_end_ = block_ends_[index];
  }

  /** @brief Initial block size and threshold for new block allocation (bytes). */
  size_t current_block_size_;

  /** @brief List of owned memory blocks, managed by unique_ptr for lifetime control. */
  std::vector<std::unique_ptr<uint8_t[]>> blocks_;

  /** @brief End pointer for each block. Corresponds 1:1 with `blocks_`. */
  std::vector<uint8_t *> block_ends_;

  /** @brief Index of the currently active block (relative to blocks_). */
  size_t current_block_index_ = 0;

  /**
   * @brief Bump-pointer within the current block.
   *
   * Points to the start position of the next allocation.
   * Advances forward with each allocation.
   */
  uint8_t *current_ptr_ = nullptr;

  /** @brief End pointer of the current block. Allocation cannot exceed this. */
  uint8_t *current_block_end_ = nullptr;
};

// ─── FixedPoolResource ────────────────────────────────────────────────────────

/**
 * @brief O(1) fixed-size pool allocator for same-size objects.
 *
 * Implemented by embedding the free-list pointer inside each slot.
 * The first bytes of each free slot store a pointer to the next free slot,
 * with no separate metadata storage.
 *
 * ### How It Works
 * On initialization:
 * ```
 * [slot0] -> [slot1] -> [slot2] -> ... -> [slotN-1] -> nullptr
 * free_list_ = slot0
 * ```
 * allocate(): pops the head from `free_list_` (O(1))
 * deallocate(): prepends the returned slot to the `free_list_` head (O(1))
 *
 * ### Usage Example
 * @code
 * // Create a pool with 256 slots of sizeof(MyCtx) each
 * qbuem::FixedPoolResource<sizeof(MyCtx), alignof(MyCtx)> pool(256);
 *
 * void *slot = pool.allocate();   // O(1); returns nullptr if exhausted
 * if (slot) {
 *     auto *ctx = new (slot) MyCtx(...); // placement new
 *     // ... use ...
 *     ctx->~MyCtx();              // explicit destructor call
 *     pool.deallocate(slot);      // O(1)
 * }
 * @endcode
 *
 * @tparam ObjectSize  Size of each slot in bytes.
 *                     Rounded up internally to satisfy Alignment.
 * @tparam Alignment   Memory alignment for each slot. Default: platform maximum.
 *
 * @note `ObjectSize` must be at least `sizeof(void*)` because the free-list
 *       pointer is stored inside each slot.
 * @note Not thread-safe. Use an independent pool per thread or provide
 *       external synchronization.
 * @warning Passing a pointer to `deallocate()` that was not obtained from this
 *          pool results in undefined behavior.
 */
template <size_t ObjectSize, size_t Alignment = alignof(std::max_align_t)>
class FixedPoolResource {
  /**
   * @brief Actual slot size rounded up to Alignment.
   *
   * Computed via bit masking: `(ObjectSize + Alignment - 1) & ~(Alignment - 1)`
   */
  static constexpr size_t kSlotSize =
      (ObjectSize + Alignment - 1) & ~(Alignment - 1);

  static_assert(kSlotSize >= sizeof(void *),
                "ObjectSize must be >= sizeof(void*) to embed the free-list ptr");

  /**
   * @brief Custom deleter for memory allocated with aligned new[].
   *
   * Memory allocated with `std::align_val_t` must be released the same way
   * to avoid undefined behavior.
   */
  struct AlignedDeleter {
    void operator()(uint8_t *p) const noexcept {
      ::operator delete[](p, std::align_val_t{Alignment});
    }
  };

public:
  /**
   * @brief Construct the pool with the given capacity and initialize the free-list.
   *
   * Allocates `capacity * kSlotSize` bytes in one shot on construction and
   * links the slots into a free-list.
   *
   * @param capacity Maximum number of slots. `allocate()` returns nullptr when exceeded.
   */
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

  /** @brief Release all owned memory. Destructors of already-allocated slots are not called. */
  ~FixedPoolResource() = default;

  /** @brief Copy construction is disabled — pool has exclusive ownership. */
  FixedPoolResource(const FixedPoolResource &) = delete;
  /** @brief Copy assignment is disabled — pool has exclusive ownership. */
  FixedPoolResource &operator=(const FixedPoolResource &) = delete;
  /** @brief Move construction is allowed. The source pool becomes empty. */
  FixedPoolResource(FixedPoolResource &&) = default;
  /** @brief Move assignment is allowed. The source pool becomes empty. */
  FixedPoolResource &operator=(FixedPoolResource &&) = default;

  /**
   * @brief Allocate one slot from the pool (O(1)).
   *
   * Pops the head slot from the free-list and returns it.
   * The returned slot is uninitialized; initialize it with placement new or equivalent.
   *
   * @returns Pointer to the allocated slot. Returns nullptr if the pool is exhausted.
   * @note `[[nodiscard]]` — ignoring the return value triggers a compile-time warning.
   */
  [[nodiscard]] void *allocate() noexcept {
    if (!free_list_) [[unlikely]]
      return nullptr;
    void *slot = free_list_;
    free_list_ = *reinterpret_cast<void **>(free_list_);
    ++used_;
    return slot;
  }

  /**
   * @brief Return a slot to the pool (O(1)).
   *
   * Prepends the returned slot to the free-list head.
   * The object destructor must be called explicitly before this function.
   *
   * @param ptr Slot pointer obtained from `allocate()`.
   * @warning Passing a pointer not obtained from this pool results in undefined behavior.
   * @warning Passing nullptr results in undefined behavior.
   */
  void deallocate(void *ptr) noexcept {
    *reinterpret_cast<void **>(ptr) = free_list_;
    free_list_ = ptr;
    --used_;
  }

  /**
   * @brief Return the total number of slots in the pool.
   * @returns The capacity value specified at construction.
   */
  size_t capacity()  const noexcept { return capacity_; }

  /**
   * @brief Return the number of currently allocated slots.
   * @returns Number of slots currently in use.
   */
  size_t used()      const noexcept { return used_; }

  /**
   * @brief Return the number of currently available slots.
   * @returns `capacity() - used()`.
   */
  size_t available() const noexcept { return capacity_ - used_; }

private:
  /** @brief Total number of slots in the pool. */
  size_t   capacity_;

  /** @brief Number of slots currently allocated. */
  size_t   used_ = 0;

  /**
   * @brief Head pointer of the free-list.
   *
   * The first bytes of each free slot contain a pointer to the next free slot.
   * The last free slot stores nullptr.
   */
  void    *free_list_ = nullptr;

  /** @brief Aligned contiguous memory block. Owns `capacity * kSlotSize` bytes. */
  std::unique_ptr<uint8_t[], AlignedDeleter> storage_;
};

} // namespace qbuem

/** @} */ // end of qbuem_memory
