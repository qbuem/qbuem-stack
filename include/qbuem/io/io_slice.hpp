#pragma once

/**
 * @file qbuem/io/io_slice.hpp
 * @brief Zero-heap-allocation I/O buffer slice primitive types.
 * @defgroup qbuem_io_buffers IO Buffer Primitives
 * @ingroup qbuem_io
 *
 * `IOSlice` and `MutableIOSlice` are fat pointers referencing a contiguous
 * byte region. They live on the stack and incur no heap allocation.
 *
 * - `IOSlice`       : Read-only byte slice (supports conversion to BufferView / iovec)
 * - `MutableIOSlice`: Writable byte slice (supports conversion to MutableBufferView / iovec)
 *
 * ### Design Principles
 * - zero-alloc: no heap allocation, stack value type
 * - Bidirectional conversion to BufferView / iovec for seamless integration with existing APIs
 * @{
 */

#include <qbuem/common.hpp>

#include <cstddef>
#include <sys/uio.h>

namespace qbuem {

// ─── IOSlice ──────────────────────────────────────────────────────────────────

/**
 * @brief Fat pointer referencing a read-only contiguous byte region (zero-alloc).
 *
 * Does not own the data. The lifetime of the buffer pointed to by this slice
 * must outlive the slice itself.
 *
 * ### Usage Example
 * @code
 * std::array<std::byte, 64> data{};
 * IOSlice slice{data.data(), data.size()};
 * auto bv = slice.to_buffer_view();   // span<const uint8_t>
 * auto iov = slice.to_iovec();        // struct iovec
 * @endcode
 */
struct IOSlice {
  /** @brief Pointer to the first byte of the buffer. */
  const std::byte *data;

  /** @brief Number of bytes in the buffer. */
  size_t size;

  /**
   * @brief Converts to a read-only `BufferView`.
   *
   * @returns The same byte region represented as `std::span<const uint8_t>`.
   * @note Only the pointer type is reinterpreted — no copy occurs.
   */
  [[nodiscard]] BufferView to_buffer_view() const noexcept {
    return {reinterpret_cast<const uint8_t *>(data), size};
  }

  /**
   * @brief Converts to a POSIX `iovec` struct.
   *
   * Suitable for direct use with scatter-gather syscalls such as
   * `readv(2)` / `writev(2)`.
   * `iov_base` is cast to a non-const pointer via `const_cast` (required by POSIX).
   *
   * @returns An `iovec` pointing to the same byte region.
   */
  [[nodiscard]] iovec to_iovec() const noexcept {
    return {const_cast<std::byte *>(data), size};
  }
};

// ─── MutableIOSlice ───────────────────────────────────────────────────────────

/**
 * @brief Fat pointer referencing a writable contiguous byte region (zero-alloc).
 *
 * Does not own the data. The lifetime of the buffer pointed to by this slice
 * must outlive the slice itself.
 *
 * ### Usage Example
 * @code
 * alignas(64) std::byte recv_buf[4096];
 * MutableIOSlice slice{recv_buf, sizeof(recv_buf)};
 * auto bv = slice.to_buffer_view();   // span<uint8_t>
 * auto iov = slice.to_iovec();        // struct iovec
 * @endcode
 */
struct MutableIOSlice {
  /** @brief Writable pointer to the first byte of the buffer. */
  std::byte *data;

  /** @brief Number of bytes in the buffer. */
  size_t size;

  /**
   * @brief Converts to a writable `MutableBufferView`.
   *
   * @returns The same byte region represented as `std::span<uint8_t>`.
   * @note Only the pointer type is reinterpreted — no copy occurs.
   */
  [[nodiscard]] MutableBufferView to_buffer_view() const noexcept {
    return {reinterpret_cast<uint8_t *>(data), size};
  }

  /**
   * @brief Converts to a POSIX `iovec` struct.
   *
   * @returns An `iovec` pointing to the same byte region.
   */
  [[nodiscard]] iovec to_iovec() const noexcept {
    return {data, size};
  }

  /**
   * @brief Converts to a read-only `IOSlice`.
   *
   * @returns A read-only slice pointing to the same byte region.
   */
  [[nodiscard]] IOSlice as_const() const noexcept {
    return {data, size};
  }
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
