#pragma once

/**
 * @file qbuem/codec/line_codec.hpp
 * @brief Line-delimiter-based frame codec — RESP, SMTP, POP3, IMAP, etc.
 * @ingroup qbuem_codec
 *
 * A codec that treats text lines delimited by `\n` or `\r\n` as frames.
 *
 * ### Key Features
 * - zero-copy: `Line::data` directly references the original receive buffer (`string_view`)
 * - CRLF mode: for Redis RESP, SMTP, HTTP/1.x headers, etc.
 * - LF mode: for simple text protocols
 *
 * ### Caution
 * `Line::data` depends on the lifetime of the `buf` passed to `decode()`.
 * If `buf` is destroyed or overwritten, `Line::data` becomes a dangling view.
 *
 * @code
 * LineCodec codec(true); // CRLF mode
 * Line line;
 * auto status = codec.decode(recv_buf, line);
 * if (status == DecodeStatus::Complete) {
 *   // line.data references recv_buf
 *   process(line.data);
 *   codec.reset();
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/codec/frame_codec.hpp>

#include <cstring>
#include <memory_resource>
#include <string_view>

namespace qbuem::codec {

// ─── Line ─────────────────────────────────────────────────────────────────────

/**
 * @brief A view type representing a single text line with the delimiter stripped.
 *
 * `data` references the original buffer in a zero-copy manner.
 * The trailing `\n` or `\r\n` is not included.
 *
 * @warning The lifetime of the buffer referenced by `data` must exceed this object.
 */
struct Line {
  /** @brief View referencing the line content (delimiter excluded). Zero-copy reference to the original buffer. */
  std::string_view data;
};

// ─── LineCodec ────────────────────────────────────────────────────────────────

/**
 * @brief Frame codec based on a line delimiter (`\n` or `\r\n`).
 *
 * Implements `IFrameCodec<Line>`.
 * `decode()` searches the buffer for a delimiter and returns a zero-copy `string_view`.
 *
 * ### CRLF vs LF Mode
 * - `crlf=true`: uses `\r\n` as delimiter. Suitable for Redis RESP, SMTP, HTTP headers.
 * - `crlf=false`: uses `\n` only. Suitable for simple text streams.
 *
 * ### encode() Behavior
 * iovec[0] = line content, iovec[1] = delimiter (`\n` or `\r\n`).
 * The delimiter references a static string literal, so no additional allocation occurs.
 */
class LineCodec : public IFrameCodec<Line> {
public:
  /**
   * @brief Constructs a LineCodec.
   *
   * @param crlf true for `\r\n` delimiter mode, false for `\n`-only mode.
   */
  explicit LineCodec(bool crlf = true) : crlf_(crlf) {}

  /**
   * @brief Decodes one line from the buffer (zero-copy).
   *
   * Searches `buf` for `\n` (LF mode) or `\r\n` (CRLF mode).
   * On match, sets `out.data` to a `string_view` referencing the content before the delimiter.
   * On success, `buf` advances past the delimiter and consumed bytes.
   *
   * @param[in,out] buf Raw byte view. Advances by consumed bytes on `Complete`.
   * @param[out]    out Decoded line view. Lifetime depends on `buf`.
   * @returns `Complete` | `Incomplete`. `Error` is never returned.
   */
  DecodeStatus decode(BufferView &buf, Line &out) override {
    const auto *data = reinterpret_cast<const char *>(buf.data());
    size_t       sz  = buf.size();

    // Search for \n position
    const char *lf = static_cast<const char *>(std::memchr(data, '\n', sz));
    if (!lf) return DecodeStatus::Incomplete;

    size_t lf_pos = static_cast<size_t>(lf - data);

    // Determine line end (strip \r in CRLF mode)
    size_t line_end = lf_pos;
    if (crlf_ && lf_pos > 0 && data[lf_pos - 1] == '\r') {
      --line_end;
    }

    out.data = std::string_view(data, line_end);

    // Advance buf past the delimiter
    buf = buf.subspan(lf_pos + 1);
    return DecodeStatus::Complete;
  }

  /**
   * @brief Encodes a line into an iovec array.
   *
   * iovec[0] = line content (zero-copy, direct reference to `line.data`)
   * iovec[1] = delimiter (`\n` or `\r\n`, references a static buffer)
   *
   * `max_vecs` must be at least 2.
   * `arena` is not used by this codec (no additional allocation needed).
   *
   * @param line     Line to encode (delimiter not included).
   * @param vecs     Output iovec array.
   * @param max_vecs Size of the iovec array (minimum 2 required).
   * @param arena    Unused. May be nullptr.
   * @returns Number of iovec entries used (2). Returns 0 if `max_vecs < 2`.
   */
  size_t encode(const Line &line, iovec *vecs, size_t max_vecs,
                std::pmr::memory_resource * /*arena*/) override {
    if (max_vecs < 2) return 0;

    vecs[0].iov_base = const_cast<char *>(line.data.data());
    vecs[0].iov_len  = line.data.size();

    // Delimiter — uses static literal buffer (no additional allocation)
    if (crlf_) {
      static const char kCRLF[] = "\r\n";
      vecs[1].iov_base = const_cast<char *>(kCRLF);
      vecs[1].iov_len  = 2;
    } else {
      static const char kLF[] = "\n";
      vecs[1].iov_base = const_cast<char *>(kLF);
      vecs[1].iov_len  = 1;
    }

    return 2;
  }

  /**
   * @brief Resets the codec state.
   *
   * `LineCodec` has no internal state due to its zero-copy design
   * that depends on an external buffer. This function exists for interface compatibility.
   */
  void reset() override {
    // No internal parser state — zero-copy design
  }

private:
  /** @brief CRLF mode flag. true uses `\r\n`, false uses `\n` as delimiter. */
  bool crlf_;
};

} // namespace qbuem::codec

/** @} */ // end of qbuem_codec (line)
