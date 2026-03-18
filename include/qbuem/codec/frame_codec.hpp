#pragma once

/**
 * @file qbuem/codec/frame_codec.hpp
 * @brief Frame codec abstract interface — IFrameCodec<Frame>
 * @defgroup qbuem_codec Frame Codecs
 * @ingroup qbuem_net
 *
 * This header defines the top-level abstraction for protocol framing.
 *
 * - `DecodeStatus` : Enumeration representing decoding results
 * - `IFrameCodec<Frame>` : Pure virtual interface that all codecs must implement
 *
 * Concrete codec implementations are located in separate headers:
 * - `<qbuem/codec/length_prefix_codec.hpp>` : 4-byte length prefix framing
 * - `<qbuem/codec/line_codec.hpp>` : Line delimiter framing (RESP, SMTP, etc.)
 * - `<qbuem/codec/http1_codec.hpp>` : HTTP/1.1 request codec
 *
 * ### Design Principles
 * - Zero virtual overhead: CRTP pattern can also be considered for static dispatch on the hot path
 * - Incremental parsing: decode() returns `Incomplete` on partial data
 * - Arena-based encoding: pass `pmr::memory_resource` to encode() for heap-free encoding
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <cstddef>
#include <memory_resource>
#include <sys/uio.h>

namespace qbuem::codec {

// ─── DecodeStatus ─────────────────────────────────────────────────────────────

/**
 * @brief Enumeration representing frame decoding result status.
 *
 * Used as the return value of `IFrameCodec::decode()`.
 *
 * - `Complete`   : One frame has been fully decoded. Result stored in `out`.
 * - `Incomplete` : Insufficient data; cannot complete the frame. More data needed.
 * - `Error`      : Protocol violation or parse error.
 *
 * @code
 * LineCodec codec;
 * Line line;
 * auto status = codec.decode(buf, line);
 * switch (status) {
 *   case DecodeStatus::Complete:   handle(line); break;
 *   case DecodeStatus::Incomplete: recv_more(); break;
 *   case DecodeStatus::Error:      close_conn(); break;
 * }
 * @endcode
 */
enum class DecodeStatus {
  Complete,   ///< Frame fully decoded
  Incomplete, ///< Insufficient data — retry after receiving more data
  Error       ///< Protocol error — connection must be closed
};

// ─── IFrameCodec<Frame> ───────────────────────────────────────────────────────

/**
 * @brief Abstract frame codec interface.
 *
 * Each codec either converts a raw byte stream into protocol frames (decode)
 * or serializes frames into an iovec array (encode).
 *
 * State is maintained internally, so decode() can be called multiple times
 * to feed data incrementally. Call `reset()` to reinitialize state for parsing
 * the next frame.
 *
 * ### Usage Example
 * @code
 * LengthPrefixedCodec codec;
 * LengthPrefixedFrame frame;
 * auto status = codec.decode(buf, frame);
 * if (status == DecodeStatus::Complete) {
 *   handle(frame);
 *   codec.reset();
 * }
 * @endcode
 *
 * @tparam Frame The frame type this codec handles.
 */
template <typename Frame>
class IFrameCodec {
public:
  /** @brief Virtual destructor. Safely releases resources of derived classes. */
  virtual ~IFrameCodec() = default;

  /**
   * @brief Decode a frame from the buffer.
   *
   * On success (`Complete`), advances `buf`'s `data`/`size` by the number of consumed bytes.
   * State is maintained internally, so re-calling after `Incomplete` with more data is valid.
   *
   * @param[in,out] buf Raw byte view to decode. Advances by consumed bytes on `Complete`.
   * @param[out]    out Decoded frame output. Valid only on `Complete`.
   * @returns `Complete` = frame finished, `Incomplete` = insufficient data, `Error` = parse error.
   */
  virtual DecodeStatus decode(BufferView &buf, Frame &out) = 0;

  /**
   * @brief Encode a frame into an iovec array (for scatter-gather writes).
   *
   * Supports zero-copy scatter-gather writes.
   * Returned iovec entries are valid for the lifetime of `arena`.
   *
   * @param frame    Frame to encode.
   * @param vecs     Output iovec array.
   * @param max_vecs Maximum number of entries in the iovec array.
   * @param arena    Memory resource for temporary allocations (e.g. header serialization).
   *                 If nullptr, uses an internal static buffer.
   * @returns Number of iovec entries used. 0 indicates encoding failure.
   */
  virtual size_t encode(const Frame &frame, iovec *vecs, size_t max_vecs,
                        std::pmr::memory_resource *arena) = 0;

  /**
   * @brief Reset the decoder's internal state.
   *
   * Call this after an error or after completing frame processing to prepare
   * for parsing the next frame. The encode() state is unaffected.
   */
  virtual void reset() = 0;
};

} // namespace qbuem::codec

/** @} */ // end of qbuem_codec
