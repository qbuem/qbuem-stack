#pragma once

/**
 * @file qbuem/io/read_buf.hpp
 * @brief Compile-time fixed-size ring buffer for socket receive operations.
 * @ingroup qbuem_io_buffers
 *
 * `ReadBuf<N>` holds an N-byte ring buffer on the stack.
 * It can be used as a socket receive buffer without heap allocation, and
 * prevents false sharing via cache-line alignment (`alignas(64)`).
 *
 * ### Design Principles
 * - N is a compile-time constant — fixed-size array allocated on the stack
 * - `write_head()` -> `commit()` -> `readable()` -> `consume()` cycle
 * - `compact()` moves remaining data to the front when write_pos reaches the end
 * @{
 */

#include <qbuem/common.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>

namespace qbuem {

/**
 * @brief Compile-time fixed-size ring buffer for socket receive operations.
 *
 * Internal storage is aligned to a cache line (`alignas(64)`) and
 * incurs no heap allocation whatsoever.
 *
 * ### Basic Usage Pattern
 * @code
 * ReadBuf<4096> buf;
 * // 1. Read from socket
 * ssize_t n = ::read(fd, buf.write_head(), buf.writable_size());
 * if (n > 0) buf.commit(static_cast<size_t>(n));
 * // 2. Consume data
 * auto view = buf.readable();   // span<const uint8_t>
 * // ... parse view ...
 * buf.consume(parsed_bytes);
 * // 3. Compact when space is low
 * if (buf.writable_size() < 512) buf.compact();
 * @endcode
 *
 * @tparam N Total buffer size in bytes. Must be greater than 0.
 */
template <size_t N>
struct ReadBuf {
  static_assert(N > 0, "ReadBuf size must be greater than 0");

  /**
   * @brief Internal storage.
   *
   * Aligned to a cache line (64 bytes) to prevent false sharing and
   * SIMD load penalties.
   */
  alignas(64) std::byte storage[N];

  /** @brief Next write position (offset where data read from the socket will be stored). */
  size_t write_pos = 0;

  /** @brief Next read position (offset of the first byte to be consumed by the application). */
  size_t read_pos = 0;

  // ─── Write Interface ──────────────────────────────────────────────────────

  /**
   * @brief Returns a pointer to the write head for direct socket reads.
   *
   * @returns `storage + write_pos` — start of unused space.
   * @warning Verify `writable_size() > 0` before use.
   */
  [[nodiscard]] std::byte *write_head() noexcept {
    return storage + write_pos;
  }

  /**
   * @brief Returns the number of bytes available for writing.
   *
   * Equal to `N - write_pos`. Increases after a call to `compact()`.
   *
   * @returns Number of writable bytes.
   */
  [[nodiscard]] size_t writable_size() const noexcept {
    return N - write_pos;
  }

  /**
   * @brief Marks n bytes as written (advances write_pos).
   *
   * @param n Number of bytes just read from the socket.
   * @pre `n <= writable_size()`.
   */
  void commit(size_t n) noexcept {
    assert(n <= writable_size() && "ReadBuf::commit out of range");
    write_pos += n;
  }

  // ─── Read Interface ───────────────────────────────────────────────────────

  /**
   * @brief Returns a view of the data available for consumption.
   *
   * @returns Read-only view over the range `[read_pos, write_pos)`.
   */
  [[nodiscard]] BufferView readable() const noexcept {
    return {reinterpret_cast<const uint8_t *>(storage + read_pos),
            write_pos - read_pos};
  }

  /**
   * @brief Marks n bytes as consumed (advances read_pos).
   *
   * @param n Number of bytes just consumed by the application.
   * @pre `n <= size()`.
   */
  void consume(size_t n) noexcept {
    assert(n <= size() && "ReadBuf::consume out of range");
    read_pos += n;
  }

  // ─── State and Utilities ──────────────────────────────────────────────────

  /**
   * @brief Returns the number of bytes pending consumption.
   *
   * @returns `write_pos - read_pos`.
   */
  [[nodiscard]] size_t size() const noexcept {
    return write_pos - read_pos;
  }

  /**
   * @brief Returns true if there is no data pending consumption.
   *
   * @returns true if `size() == 0`.
   */
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  /**
   * @brief Returns true if the buffer is full (no writable space remaining).
   *
   * @returns true if `writable_size() == 0`.
   */
  [[nodiscard]] bool full() const noexcept { return writable_size() == 0; }

  /**
   * @brief Moves remaining data to the front of the buffer to reclaim write space.
   *
   * Call this when `write_pos == N` and there is still unconsumed data.
   * After compaction, `read_pos = 0` and `write_pos = size()`.
   * This is a no-op if `read_pos` is already 0.
   */
  void compact() noexcept {
    if (read_pos == 0)
      return;
    size_t remaining = size();
    if (remaining > 0)
      std::memmove(storage, storage + read_pos, remaining);
    write_pos = remaining;
    read_pos  = 0;
  }

  /**
   * @brief Clears the buffer completely (resets read and write positions to 0).
   */
  void reset() noexcept {
    read_pos  = 0;
    write_pos = 0;
  }
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
