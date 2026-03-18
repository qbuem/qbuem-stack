#pragma once

/**
 * @file qbuem/io/write_buf.hpp
 * @brief Write cork buffer — accumulates multiple chunks and flushes with a single writev().
 * @ingroup qbuem_io_buffers
 *
 * `WriteBuf` gathers data across multiple `append()` calls and passes it
 * to a single `writev(2)` syscall via `as_iovec()`.
 * This minimises the number of syscalls and TCP packet fragmentation.
 *
 * ### Design principles
 * - Internal buffer is `std::vector<std::byte>` (planned to be replaced with Arena in the future)
 * - `as_iovec()` returns `IOVec<16>` — maximum 16-segment writev
 * - Buffer capacity is preserved after `clear()` (no reallocation)
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/io/iovec.hpp>

#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace qbuem {

/**
 * @brief Write cork buffer — accumulates chunks then flushes with a single writev().
 *
 * Use this to transmit data composed of multiple pieces — such as header + body
 * in HTTP responses or protocol frames — in a single system call.
 *
 * ### Usage example
 * @code
 * WriteBuf wbuf;
 * wbuf.append(header_view);
 * wbuf.append("\r\n");
 * wbuf.append(body_view);
 *
 * auto vec = wbuf.as_iovec();
 * ::writev(fd, vec.as_span().data(), static_cast<int>(vec.as_span().size()));
 * wbuf.clear();
 * @endcode
 */
class WriteBuf {
public:
  /**
   * @brief Constructs a WriteBuf with the specified initial capacity.
   *
   * @param initial_cap Initial internal buffer capacity in bytes. Default: 4096.
   */
  explicit WriteBuf(size_t initial_cap = 4096) {
    buf_.reserve(initial_cap);
  }

  // ─── Append ───────────────────────────────────────────────────────────────

  /**
   * @brief Copies data from a BufferView into the buffer.
   *
   * @param data Read-only byte view to copy.
   */
  void append(BufferView data) {
    const auto *src = reinterpret_cast<const std::byte *>(data.data());
    buf_.insert(buf_.end(), src, src + data.size());
  }

  /**
   * @brief Copies the contents of a string_view into the buffer.
   *
   * @param sv String view to copy.
   */
  void append(std::string_view sv) {
    const auto *src = reinterpret_cast<const std::byte *>(sv.data());
    buf_.insert(buf_.end(), src, src + sv.size());
  }

  /**
   * @brief Copies data from a raw pointer and length into the buffer.
   *
   * @param data Source pointer.
   * @param len  Number of bytes to copy.
   */
  void append(const void *data, size_t len) {
    const auto *src = static_cast<const std::byte *>(data);
    buf_.insert(buf_.end(), src, src + len);
  }

  // ─── writev conversion ────────────────────────────────────────────────────

  /**
   * @brief Converts the entire buffer into a single iovec.
   *
   * Because `WriteBuf` is a single contiguous buffer, the returned `IOVec<16>`
   * always contains exactly one entry. An empty IOVec is returned for an empty buffer.
   *
   * @returns `IOVec<16>` — a single iovec entry pointing at the entire buffer.
   * @note The returned IOVec depends on the lifetime of `buf_`.
   *       Calling `append()` or `clear()` after `as_iovec()` invalidates the result.
   */
  [[nodiscard]] IOVec<16> as_iovec() const noexcept {
    IOVec<16> vec;
    if (!buf_.empty())
      vec.push(buf_.data(), buf_.size());
    return vec;
  }

  // ─── State ────────────────────────────────────────────────────────────────

  /**
   * @brief Returns the number of bytes currently accumulated in the buffer.
   *
   * @returns Byte count of the internal buffer.
   */
  [[nodiscard]] size_t size() const noexcept { return buf_.size(); }

  /**
   * @brief Checks whether the buffer is empty.
   *
   * @returns true if `size() == 0`.
   */
  [[nodiscard]] bool empty() const noexcept { return buf_.empty(); }

  /**
   * @brief Clears the buffer contents.
   *
   * The internal capacity is preserved, so no reallocation occurs.
   */
  void clear() noexcept { buf_.clear(); }

private:
  /**
   * @brief Internal data store.
   *
   * Implemented as `std::vector` so it can be swapped for an Arena-based
   * allocator in the future.
   */
  std::vector<std::byte> buf_;
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
