#pragma once

/**
 * @file qbuem/core/mmap_arena.hpp
 * @brief mmap-based Arena allocator — supports madvise(MADV_DONTNEED/MADV_FREE).
 * @ingroup qbuem_memory
 *
 * This header provides a bump-pointer Arena allocator that operates over a
 * fixed-capacity memory region allocated with mmap.
 *
 * ## Differences from Arena (arena.hpp)
 * | Feature               | Arena            | MmapArena                     |
 * |----------------------|-----------------|-------------------------------|
 * | Memory source        | `new[]`          | `mmap(MAP_ANONYMOUS)`         |
 * | Capacity             | Dynamic growth   | Fixed at construction         |
 * | reset() behavior     | Pointer reset only | Pointer reset + `MADV_DONTNEED` |
 * | Post-connection-close | None            | `MADV_FREE` (lazy OS reclaim) |
 *
 * ## Design Principles
 * - **O(1) allocation**: Only performs bump-pointer increment.
 * - **Zero-overhead reset()**: Resets pointer to the start and returns pages
 *   to the OS immediately via `MADV_DONTNEED`. The kernel provides zero-filled
 *   pages on next access.
 * - **Lazy OS reclaim**: `release()` calls `MADV_FREE` to hint the kernel to
 *   reclaim physical pages at its convenience.
 *
 * ## Platform Support
 * - Linux: `mmap(MAP_ANONYMOUS)`, `madvise(MADV_DONTNEED)`, `madvise(MADV_FREE)`.
 * - Non-Linux: Falls back to `new std::byte[]` / `delete[]`; madvise calls are no-ops.
 *
 * ## Usage Example
 * @code
 * // Create a 4 MiB Arena
 * qbuem::MmapArena arena(4 * 1024 * 1024);
 *
 * // Aligned allocation (bump-pointer)
 * void *hdr = arena.allocate(sizeof(MyHeader), alignof(MyHeader));
 * void *buf = arena.allocate(1024);
 *
 * // After completing request processing: reset pointer + MADV_DONTNEED
 * arena.reset();
 *
 * // On connection close: lazy physical memory reclaim via MADV_FREE
 * arena.release();
 * @endcode
 *
 * @{
 */

#include <cstddef>
#include <memory>
#include <new>

#if defined(__linux__)
#  include <sys/mman.h>
#  include <numaif.h>
#  if !defined(MPOL_BIND)
// Define directly if numaif.h is unavailable (graceful fallback)
#    define MPOL_BIND 2
extern "C" long mbind(void*, unsigned long, int, const unsigned long*,
                      unsigned long, unsigned int);
#  endif
#endif

namespace qbuem {

/**
 * @brief mmap-based bump-pointer Arena allocator.
 *
 * Reserves an mmap region of `capacity_bytes` at construction.
 * Subsequent `allocate()` calls only perform a bump-pointer advance
 * including alignment padding.
 *
 * ### Alignment Handling
 * `allocate(bytes, align)` rounds up the current pointer to a multiple of
 * `align`, then advances by `bytes`:
 * ```
 * padding = (align - (current % align)) % align
 * result  = base + offset + padding
 * offset += padding + bytes
 * ```
 *
 * ### reset() vs release()
 * - `reset()`: Called between request/response cycles. Resets the bump-pointer
 *   to 0 and reclaims physical pages immediately via `MADV_DONTNEED`.
 * - `release()`: Called on connection close. Hints the kernel to lazily reclaim
 *   physical pages via `MADV_FREE`. Does not reset the pointer.
 *
 * @note Not copyable; movable.
 * @note This class is not thread-safe.
 *       Design each thread/coroutine to own an independent `MmapArena`.
 * @warning The lifetime of pointers returned by `allocate()` depends on the
 *          `MmapArena` object's lifetime and calls to `reset()`.
 */
class MmapArena {
public:
  /**
   * @brief Construct a MmapArena with the specified capacity.
   *
   * Reserves `capacity_bytes` of virtual address space via
   * `mmap(MAP_PRIVATE|MAP_ANONYMOUS)`. Physical pages are committed on first access.
   *
   * Falls back to `new std::byte[capacity_bytes]` on non-Linux platforms.
   *
   * @param capacity_bytes Maximum capacity of the Arena (bytes). Must be > 0.
   * @throws std::bad_alloc if mmap or new allocation fails.
   */
  explicit MmapArena(std::size_t capacity_bytes)
      : base_(map_memory(capacity_bytes)), capacity_(capacity_bytes) {}

  /**
   * @brief Destructor — releases the mmap region.
   *
   * Linux: releases the entire `capacity_` bytes via `munmap`.
   * Non-Linux: releases via `delete[]`.
   */
  ~MmapArena() noexcept {
    if (base_ == nullptr) return;
#if defined(__linux__)
    ::munmap(base_, capacity_);
#else
    delete[] base_;
#endif
  }

  /** @brief Copy construction disabled — Arena has exclusive ownership. */
  MmapArena(const MmapArena &) = delete;
  /** @brief Copy assignment disabled — Arena has exclusive ownership. */
  MmapArena &operator=(const MmapArena &) = delete;

  /**
   * @brief Move constructor. The source Arena becomes empty (base_ == nullptr).
   * @param other The source Arena to move from.
   */
  MmapArena(MmapArena &&other) noexcept
      : base_(other.base_),
        capacity_(other.capacity_),
        offset_(other.offset_) {
    other.base_     = nullptr;
    other.capacity_ = 0;
    other.offset_   = 0;
  }

  /**
   * @brief Move assignment. Releases existing resources and takes ownership from source.
   * @param other The source Arena to move from.
   * @returns `*this`.
   */
  MmapArena &operator=(MmapArena &&other) noexcept {
    if (this == &other) return *this;
    // Release existing resources
    if (base_ != nullptr) {
#if defined(__linux__)
      ::munmap(base_, capacity_);
#else
      delete[] base_;
#endif
    }
    base_     = other.base_;
    capacity_ = other.capacity_;
    offset_   = other.offset_;
    other.base_     = nullptr;
    other.capacity_ = 0;
    other.offset_   = 0;
    return *this;
  }

  /**
   * @brief Allocate aligned memory from the Arena (O(1) bump-pointer).
   *
   * Rounds up the current bump-pointer to the `align` boundary, then
   * advances by `bytes`. Returns `nullptr` if capacity is exceeded.
   *
   * @param bytes Requested memory size (bytes). Returns nullptr if 0.
   * @param align Memory alignment (must be a power of two).
   *              Default is `alignof(std::max_align_t)`.
   * @returns Pointer to allocated memory. `nullptr` on capacity overflow.
   *
   * @note The lifetime of the returned pointer depends on the `MmapArena`
   *       object and calls to `reset()`.
   */
  [[nodiscard]] void *allocate(
      std::size_t bytes,
      std::size_t align = alignof(std::max_align_t)) noexcept {
    if (bytes == 0 || base_ == nullptr) return nullptr;

    // Round up current pointer to a multiple of align.
    auto current = reinterpret_cast<std::uintptr_t>(base_) + offset_;
    std::size_t padding = (align - (current % align)) % align;

    // Check for capacity overflow
    if (offset_ + padding + bytes > capacity_) return nullptr;

    void *result = reinterpret_cast<void *>(current + padding);
    offset_ += padding + bytes;
    return result;
  }

  /**
   * @brief Reset the bump-pointer to the start and return pages to the OS.
   *
   * Resets offset to 0, invalidating all previously allocated pointers.
   *
   * On Linux, calls `madvise(MADV_DONTNEED)` on the entire mmap region to
   * return physical pages to the OS immediately. The kernel provides
   * zero-filled new pages on next access.
   *
   * On non-Linux platforms, only the pointer is reset.
   *
   * @note Calling between HTTP request/response cycles improves memory reuse efficiency.
   * @warning All pointers allocated before reset() become unusable afterward.
   */
  void reset() noexcept {
    offset_ = 0;
#if defined(__linux__)
    if (base_ != nullptr) {
      ::madvise(base_, capacity_, MADV_DONTNEED);
    }
#endif
  }

  /**
   * @brief Hints the kernel to lazily reclaim physical pages via MADV_FREE.
   *
   * Hints the kernel to reclaim physical pages at its convenience.
   * The virtual address space is retained; the kernel provides new pages on next access.
   * The bump-pointer (`offset_`) is not reset.
   *
   * No-op on non-Linux platforms.
   *
   * @note Call on connection close to reduce physical memory pressure.
   * @note `MADV_FREE` requires Linux 4.5 or later.
   *       On older kernels, EINVAL may be returned; this function ignores errors.
   */
  void release() noexcept {
#if defined(__linux__)
    if (base_ != nullptr) {
      ::madvise(base_, capacity_, MADV_FREE);
    }
#endif
  }

  /**
   * @brief Returns the number of bytes allocated so far.
   *
   * Includes alignment padding bytes.
   *
   * @returns Bytes in use (0 ~ capacity()).
   */
  [[nodiscard]] std::size_t used() const noexcept {
    return offset_;
  }

  /**
   * @brief Returns the total capacity of the Arena (bytes).
   *
   * Equal to the `capacity_bytes` passed to the constructor.
   *
   * @returns Total bytes allocated via mmap.
   */
  [[nodiscard]] std::size_t capacity() const noexcept {
    return capacity_;
  }

  /**
   * @brief Binds the mmap region to a specific NUMA node's memory.
   *
   * Uses `mbind(2)` (Linux) to set a policy (MPOL_BIND) so physical pages of
   * this Arena are allocated from the specified NUMA node.
   *
   * Allocating a reactor-local Arena from the same NUMA node's memory avoids
   * cross-NUMA memory accesses and reduces latency.
   *
   * @param numa_node  NUMA node number to bind to (0-based).
   * @returns true on success. false on non-Linux or when libnuma is unavailable.
   */
  bool bind_to_numa_node(int numa_node) noexcept {
#if defined(__linux__)
    if (base_ == nullptr || numa_node < 0) return false;

    // nodemask: 64-bit array with the bit for the specified node set
    unsigned long nodemask = 1UL << static_cast<unsigned>(numa_node);
    unsigned long maxnode  = static_cast<unsigned>(numa_node) + 1;

    int ret = static_cast<int>(::mbind(
        base_, capacity_,
        MPOL_BIND,
        &nodemask, maxnode + 1,
        0  // flags: 0 = applies to future allocations only
    ));
    return (ret == 0);
#else
    (void)numa_node;
    return false;
#endif
  }

private:
  /**
   * @brief Allocates `size` bytes of memory appropriate for the platform.
   *
   * Linux: `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)`.
   * Non-Linux: `new std::byte[size]`.
   *
   * @param size Number of bytes to allocate.
   * @returns Pointer to the allocated memory (cast by the caller as needed).
   * @throws std::bad_alloc on allocation failure.
   */
  static void *map_memory(std::size_t size) {
#if defined(__linux__)
    void *ptr = ::mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);

    if (ptr == MAP_FAILED) {
      throw std::bad_alloc{};
    }
    return ptr;
#else
    return new std::byte[size];
#endif
  }

  /** @brief Start pointer of memory allocated via mmap (or new[]). */
  void *base_ = nullptr;

  /** @brief Total byte count of the mmap region. */
  std::size_t capacity_ = 0;

  /**
   * @brief Byte offset of the current bump-pointer from `base_`.
   *
   * Increases by alignment padding plus requested size on each `allocate()` call.
   * Reset to 0 by `reset()`.
   */
  std::size_t offset_ = 0;
};

} // namespace qbuem

/** @} */
