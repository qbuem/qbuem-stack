#pragma once

/**
 * @file qbuem/codec/length_prefix_codec.hpp
 * @brief 4-byte big-endian length-prefix frame codec
 * @ingroup qbuem_codec
 *
 * This header provides a concrete implementation of `IFrameCodec<LengthPrefixedFrame>`.
 *
 * ## Frame Format
 * ```
 * ┌─────────────────────────────┬───────────────────────────────┐
 * │  length (4 bytes, big-endian) │   payload (length bytes)    │
 * └─────────────────────────────┴───────────────────────────────┘
 * ```
 *
 * ## Usage Example
 * @code
 * LengthPrefixedCodec codec;
 * LengthPrefixedFrame frame;
 * auto status = codec.decode(recv_buf, frame);
 * if (status == DecodeStatus::Complete) {
 *   // payload data is present in frame.payload
 *   process(frame.payload);
 *   codec.reset();
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/codec/frame_codec.hpp>

#include <arpa/inet.h> // ntohl, htonl
#include <cstring>
#include <memory_resource>
#include <vector>

namespace qbuem::codec {

// ─── LengthPrefixedFrame ──────────────────────────────────────────────────────

/**
 * @brief A single frame for the length-prefix protocol.
 *
 * `length` is stored in host byte order.
 * `LengthPrefixedCodec::encode()` automatically converts to big-endian for network transmission.
 */
struct LengthPrefixedFrame {
  /** @brief Payload length (host byte order). Serialized as big-endian during encode(). */
  uint32_t length = 0;

  /** @brief Payload data. */
  std::vector<std::byte> payload;
};

// ─── LengthPrefixedCodec ──────────────────────────────────────────────────────

/**
 * @brief Frame codec based on a 4-byte big-endian length prefix.
 *
 * Implements `IFrameCodec<LengthPrefixedFrame>`.
 * Internally maintains a header/body parsing state machine.
 *
 * ### State Machine
 * ```
 * Header (waiting for 4 bytes)
 *   → Body (waiting for length bytes)
 *     → Complete (frame complete, call reset() to return to Header)
 * ```
 *
 * ### Encoding
 * `encode()` sets iovec[0] to the 4-byte big-endian header and iovec[1] to the payload.
 * Allocates the header buffer via `arena`, or uses an internal buffer if arena is nullptr.
 */
class LengthPrefixedCodec : public IFrameCodec<LengthPrefixedFrame> {
public:
  /** @brief Default constructor. Initializes state to Header. */
  LengthPrefixedCodec() = default;

  /**
   * @brief Decodes a length-prefixed frame from a buffer.
   *
   * Processed in two stages:
   * 1. Header stage: reads the 4-byte big-endian length value.
   * 2. Body stage: reads `pending_length_` bytes to complete the payload.
   *
   * On success, `buf` advances by the number of consumed bytes.
   *
   * @param[in,out] buf Raw byte view. Advances by consumed bytes on `Complete`.
   * @param[out]    out Decoded frame. Valid only on `Complete`.
   * @returns `Complete` | `Incomplete` | `Error`.
   */
  DecodeStatus decode(BufferView &buf, LengthPrefixedFrame &out) override {
    size_t consumed = 0;

    if (state_ == ParseState::Header) {
      // Accumulate data into header buffer
      while (header_received_ < kHeaderSize && consumed < buf.size()) {
        header_buf_[header_received_++] = buf[consumed++];
      }

      if (header_received_ < kHeaderSize) {
        buf = buf.subspan(consumed);
        return DecodeStatus::Incomplete;
      }

      // Convert 4-byte big-endian to host byte order
      uint32_t net_len = 0;
      std::memcpy(&net_len, header_buf_, kHeaderSize);
      pending_length_ = ntohl(net_len);

      state_ = ParseState::Body;
      body_buf_.clear();
      body_buf_.reserve(pending_length_);
    }

    // Body stage: receive pending_length_ bytes
    size_t remaining = pending_length_ - body_buf_.size();
    size_t available = buf.size() - consumed;
    size_t to_copy   = std::min(remaining, available);

    const auto *src = reinterpret_cast<const std::byte *>(buf.data() + consumed);
    body_buf_.insert(body_buf_.end(), src, src + to_copy);
    consumed += to_copy;

    buf = buf.subspan(consumed);

    if (body_buf_.size() < pending_length_) {
      return DecodeStatus::Incomplete;
    }

    // Frame complete
    out.length  = pending_length_;
    out.payload = std::move(body_buf_);
    return DecodeStatus::Complete;
  }

  /**
   * @brief Encodes a frame into an iovec array.
   *
   * iovec[0] = 4-byte big-endian length header (uses arena or internal buffer)
   * iovec[1] = payload data (zero-copy)
   *
   * `max_vecs` must be at least 2.
   *
   * @param frame    Frame to encode.
   * @param vecs     Output iovec array.
   * @param max_vecs Size of the iovec array (minimum 2 required).
   * @param arena    Memory resource for header buffer allocation. Uses internal buffer if nullptr.
   * @returns Number of iovec entries used (2). Returns 0 if `max_vecs < 2`.
   */
  size_t encode(const LengthPrefixedFrame &frame, iovec *vecs, size_t max_vecs,
                std::pmr::memory_resource *arena) override {
    if (max_vecs < 2) return 0;

    // Header buffer: use arena if available, otherwise use internal buffer
    uint8_t *hdr_buf;
    if (arena) {
      hdr_buf = static_cast<uint8_t *>(arena->allocate(kHeaderSize, alignof(uint32_t)));
    } else {
      hdr_buf = encode_hdr_buf_;
    }

    uint32_t net_len = htonl(frame.length);
    std::memcpy(hdr_buf, &net_len, kHeaderSize);

    vecs[0].iov_base = hdr_buf;
    vecs[0].iov_len  = kHeaderSize;
    vecs[1].iov_base = const_cast<std::byte *>(frame.payload.data());
    vecs[1].iov_len  = frame.payload.size();

    return 2;
  }

  /**
   * @brief Resets the parser state.
   *
   * Returns to the Header stage to prepare for parsing the next frame.
   */
  void reset() override {
    state_            = ParseState::Header;
    pending_length_   = 0;
    header_received_  = 0;
    body_buf_.clear();
  }

private:
  /** @brief Length header size (bytes). */
  static constexpr size_t kHeaderSize = 4;

  /** @brief State enum representing the parser stage. */
  enum class ParseState { Header, Body };

  /** @brief Current parser state. */
  ParseState state_ = ParseState::Header;

  /** @brief Header receive buffer (4 bytes). */
  uint8_t  header_buf_[kHeaderSize] = {};

  /** @brief Number of header bytes received so far. */
  size_t   header_received_ = 0;

  /** @brief Expected payload length (host byte order). */
  uint32_t pending_length_ = 0;

  /** @brief Payload receive buffer. */
  std::vector<std::byte> body_buf_;

  /** @brief Internal header buffer used by encode() when arena is nullptr. */
  uint8_t encode_hdr_buf_[kHeaderSize] = {};
};

} // namespace qbuem::codec

/** @} */ // end of qbuem_codec (length_prefix)
