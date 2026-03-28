#pragma once

/**
 * @file qbuem/io/iovec.hpp
 * @brief Stack-allocated scatter-gather I/O vector array.
 * @ingroup qbuem_io_buffers
 *
 * `IOVec<N>` is a container that holds up to N `iovec` entries on the stack.
 * Builds a vector array to pass to `writev(2)` / `readv(2)` without heap allocation.
 *
 * ### Design principles
 * - N is a compile-time constant — fixed-size array allocated on the stack
 * - zero-alloc: no heap allocation at all
 * - Supports both BufferView and raw pointers via `push()` overloads
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <span>
#include <sys/uio.h>

// Forward declaration — include scattered_span.hpp directly if you need the full type.
namespace qbuem { class scattered_span; }

namespace qbuem {

/**
 * @brief Stack-allocated scatter-gather iovec array.
 *
 * Holds up to N `iovec` entries.
 * Use the `as_span()` method to obtain a span suitable for passing directly
 * to `writev(2)` / `readv(2)` syscalls.
 *
 * ### Usage example
 * @code
 * IOVec<4> vec;
 * vec.push(header.data(), header.size());
 * vec.push(body_view);
 * ::writev(fd, vec.as_span().data(), static_cast<int>(vec.as_span().size()));
 * @endcode
 *
 * @tparam N Maximum number of iovec entries. Compile-time constant.
 */
template <size_t N>
struct IOVec {
  /** @brief Array of iovec entries. */
  std::array<iovec, N> vecs{};

  /** @brief Number of currently valid entries. */
  size_t count = 0;

  // ─── Append ──────────────────────────────────────────────────────────────

  /**
   * @brief Appends an iovec entry from a raw pointer and length.
   *
   * @param data Buffer pointer.
   * @param len  Buffer size in bytes.
   * @pre `count < N` — aborts via assert if exceeded.
   */
  void push(const void *data, size_t len) noexcept {
    assert(count < N && "IOVec capacity exceeded");
    vecs[count].iov_base = const_cast<void *>(data);
    vecs[count].iov_len  = len;
    ++count;
  }

  /**
   * @brief Appends an iovec entry from a `BufferView`.
   *
   * @param buf Read-only byte view.
   * @pre `count < N` — aborts via assert if exceeded.
   */
  void push(BufferView buf) noexcept {
    push(buf.data(), buf.size());
  }

  /**
   * @brief Appends an iovec entry from a `MutableBufferView`.
   *
   * @param buf Writable byte view.
   * @pre `count < N` — aborts via assert if exceeded.
   */
  void push(MutableBufferView buf) noexcept {
    push(buf.data(), buf.size());
  }

  // ─── Accessors ───────────────────────────────────────────────────────────

  /**
   * @brief Returns a `std::span` of the valid iovec entries.
   *
   * @returns Mutable span over `iovec[0..count)`.
   */
  [[nodiscard]] std::span<iovec> as_span() noexcept {
    return {vecs.data(), count};
  }

  /**
   * @brief Returns a const `std::span` of the valid iovec entries.
   *
   * @returns Const span over `iovec[0..count)`.
   */
  [[nodiscard]] std::span<const iovec> as_const_span() const noexcept {
    return {vecs.data(), count};
  }

  /**
   * @brief Computes the total byte count across all entries.
   *
   * @returns Sum of all `iov_len` values.
   */
  [[nodiscard]] size_t total_bytes() const noexcept {
    size_t total = 0;
    for (size_t i = 0; i < count; ++i)
      total += vecs[i].iov_len;
    return total;
  }

  /**
   * @brief Removes all entries and resets count to 0.
   */
  void clear() noexcept { count = 0; }

  /**
   * @brief Checks whether the array is empty.
   * @returns true if `count == 0`.
   */
  [[nodiscard]] bool empty() const noexcept { return count == 0; }

  /**
   * @brief Checks whether the array is full.
   * @returns true if `count == N`.
   */
  [[nodiscard]] bool full() const noexcept { return count == N; }

  /**
   * @brief Returns a `scattered_span` view over the current entries.
   *
   * The returned `scattered_span` is valid as long as this `IOVec` is in scope.
   * Include `<qbuem/io/scattered_span.hpp>` to use the full `scattered_span` API.
   *
   * @code
   * IOVec<2> vec;
   * vec.push(header.data(), header.size());
   * vec.push(body.data(),   body.size());
   * co_await stream.writev(vec.as_scattered());  // single writev syscall
   * @endcode
   */
  [[nodiscard]] scattered_span as_scattered() const noexcept;  // defined in scattered_span.hpp
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
