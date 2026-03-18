#pragma once

/**
 * @file qbuem/http/inline_request.hpp
 * @brief Stack-based inline buffer optimisation for small request headers
 * @defgroup qbuem_inline_request Inline Request Buffer
 * @ingroup qbuem_http
 *
 * Most HTTP request headers are 2 KiB or smaller (typical GET, POST).
 * `InlineRequestBuffer<N>` stores headers up to N bytes directly on the
 * stack (or inline array) with no heap allocation.
 *
 * ## Application points
 * When the HTTP parser receives raw bytes:
 * 1. If the header is N bytes or smaller → store directly in `InlineRequestBuffer<N>`
 * 2. If the header exceeds N → fall back to a `std::string` heap allocation
 *
 * ## Usage example
 * ```cpp
 * // Received raw HTTP data
 * std::string_view raw = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
 *
 * qbuem::http::InlineRequestBuffer<2048> buf;
 * bool fits = buf.try_push(raw);
 * if (fits) {
 *     // Parse without heap allocation
 *     HttpParser parser;
 *     Request req;
 *     parser.parse(buf.view(), req);
 * }
 * ```
 *
 * @{
 */

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace qbuem::http {

/**
 * @brief Fixed-size inline buffer — avoids heap allocation for small HTTP headers.
 *
 * @tparam N  Inline buffer size in bytes. Default 2 KiB.
 *            If the header exceeds N, `try_push()` returns false.
 */
template <size_t N = 2048>
class InlineRequestBuffer {
public:
  /**
   * @brief Append data to the inline buffer.
   *
   * @param data Byte slice to append.
   * @returns true — fits in the buffer. false — exceeds N, heap allocation required.
   */
  bool try_push(std::string_view data) noexcept {
    if (used_ + data.size() > N) return false;
    std::memcpy(buf_.data() + used_, data.data(), data.size());
    used_ += data.size();
    return true;
  }

  /**
   * @brief Append a single byte to the buffer.
   */
  bool try_push(char c) noexcept {
    if (used_ >= N) return false;
    buf_[used_++] = static_cast<std::byte>(c);
    return true;
  }

  /**
   * @brief Return a view of the data stored so far.
   */
  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view{
        reinterpret_cast<const char*>(buf_.data()), used_};
  }

  /// @brief Number of bytes stored.
  [[nodiscard]] size_t size() const noexcept { return used_; }

  /// @brief Remaining available space in bytes.
  [[nodiscard]] size_t remaining() const noexcept { return N - used_; }

  /// @brief Returns true if the buffer is empty.
  [[nodiscard]] bool empty() const noexcept { return used_ == 0; }

  /// @brief Reset the buffer for reuse.
  void reset() noexcept { used_ = 0; }

  /// @brief Maximum capacity of the inline buffer.
  static constexpr size_t capacity() noexcept { return N; }

private:
  std::array<std::byte, N> buf_{};
  size_t                   used_{0};
};

/**
 * @brief Adaptive buffer that automatically selects between small and large paths.
 *
 * Uses the inline stack buffer for data up to `N` bytes; falls back to
 * `std::string` when the threshold is exceeded.
 *
 * @tparam N  Inline threshold in bytes (default 2 KiB).
 */
template <size_t N = 2048>
class AdaptiveRequestBuffer {
public:
  /**
   * @brief Append data to the buffer.
   * @param data Bytes to append.
   */
  void push(std::string_view data) {
    if (using_inline_) {
      if (inline_buf_.try_push(data)) return;
      // Overflow → heap fallback
      heap_buf_.reserve(inline_buf_.size() + data.size());
      heap_buf_.append(
          reinterpret_cast<const char*>(inline_buf_.view().data()),
          inline_buf_.size());
      heap_buf_.append(data.data(), data.size());
      using_inline_ = false;
    } else {
      heap_buf_.append(data.data(), data.size());
    }
  }

  /**
   * @brief Return a view of the buffer contents.
   */
  [[nodiscard]] std::string_view view() const noexcept {
    if (using_inline_) return inline_buf_.view();
    return heap_buf_;
  }

  /// @brief Returns true if the heap allocation is being used.
  [[nodiscard]] bool is_heap() const noexcept { return !using_inline_; }

  /// @brief Total number of bytes stored.
  [[nodiscard]] size_t size() const noexcept {
    return using_inline_ ? inline_buf_.size() : heap_buf_.size();
  }

  /// @brief Reset the buffer.
  void reset() noexcept {
    inline_buf_.reset();
    heap_buf_.clear();
    using_inline_ = true;
  }

private:
  InlineRequestBuffer<N> inline_buf_;
  std::string            heap_buf_;
  bool                   using_inline_{true};
};

} // namespace qbuem::http

/** @} */
