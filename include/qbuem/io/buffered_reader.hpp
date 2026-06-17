#pragma once

/**
 * @file qbuem/io/buffered_reader.hpp
 * @brief Buffered, delimiter-aware reader over any async byte stream.
 * @defgroup qbuem_buffered_reader Buffered Reader
 * @ingroup qbuem_io
 *
 * `TcpStream` (and friends) expose raw `read()`/`read_exact()` but no
 * `read_until`/`read_line` — a one-byte-at-a-time read_until would issue a
 * syscall per byte. `BufferedReader<Stream>` owns a small buffer, reads in
 * chunks, and scans the buffer for the delimiter, so a line read costs ~one
 * syscall per chunk, not per byte. Leftover bytes past the delimiter are
 * retained for the next call.
 *
 * `Stream` only needs `Task<Result<size_t>> read(std::span<std::byte>)`.
 *
 * @code
 * qbuem::BufferedReader reader{stream};
 * auto line = co_await reader.read_line();   // includes the trailing '\n'
 * if (line) handle(*line);
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace qbuem {

/**
 * @brief Buffered reader adapter over an async byte stream.
 * @tparam Stream Any type with `Task<Result<size_t>> read(std::span<std::byte>)`.
 */
template <class Stream>
class BufferedReader {
public:
  /**
   * @param stream    Underlying stream (must outlive the reader).
   * @param chunk     Per-read chunk size (bytes pulled from the stream at once).
   */
  explicit BufferedReader(Stream& stream, size_t chunk = 4096)
      : stream_(stream), chunk_(chunk == 0 ? 1 : chunk) {}

  /**
   * @brief Read up to and including the next `delim`.
   *
   * @param delim     Delimiter byte.
   * @param max_bytes Cap on a single token (protects against an unbounded line).
   * @returns The bytes through `delim` (inclusive); `connection_reset` on EOF
   *          before the delimiter; `message_size` if `max_bytes` is exceeded.
   */
  [[nodiscard]] Task<Result<std::string>> read_until(char delim,
                                                     size_t max_bytes = 64 * 1024) {
    for (;;) {
      for (size_t i = start_; i < buf_.size(); ++i) {
        if (static_cast<char>(buf_[i]) == delim) {
          std::string out(reinterpret_cast<const char*>(buf_.data() + start_),
                          i - start_ + 1);
          start_ = i + 1;
          if (start_ == buf_.size()) { buf_.clear(); start_ = 0; }
          co_return out;
        }
      }
      if (buf_.size() - start_ > max_bytes)
        co_return std::unexpected(std::make_error_code(std::errc::message_size));

      // Compact consumed prefix before growing, then pull another chunk.
      if (start_ > 0) {
        buf_.erase(buf_.begin(),
                   buf_.begin() + static_cast<std::ptrdiff_t>(start_));
        start_ = 0;
      }
      const size_t off = buf_.size();
      buf_.resize(off + chunk_);
      auto n = co_await stream_.read(
          std::span<std::byte>(buf_.data() + off, chunk_));
      if (!n) { buf_.resize(off); co_return std::unexpected(n.error()); }
      if (*n == 0) {
        buf_.resize(off);
        co_return std::unexpected(
            std::make_error_code(std::errc::connection_reset));
      }
      buf_.resize(off + *n);
    }
  }

  /** @brief Read a line terminated by '\n' (the '\n' is included). */
  [[nodiscard]] Task<Result<std::string>> read_line(size_t max_bytes = 64 * 1024) {
    co_return co_await read_until('\n', max_bytes);
  }

  /** @brief Bytes currently buffered (already read from the stream, unconsumed). */
  [[nodiscard]] std::span<const std::byte> buffered() const noexcept {
    return {buf_.data() + start_, buf_.size() - start_};
  }

private:
  Stream&               stream_;
  size_t                chunk_;
  std::vector<std::byte> buf_;
  size_t                start_ = 0;
};

} // namespace qbuem

/** @} */
