#pragma once

/**
 * @file qbuem/codec/http1_codec.hpp
 * @brief HTTP/1.1 request codec — IFrameCodec<http::Request> wrapper
 * @ingroup qbuem_codec
 *
 * An adapter that wraps `HttpParser` in the `IFrameCodec<Request>` interface.
 *
 * ### Behavior Overview
 * - `decode()`: Uses `HttpParser` to incrementally parse HTTP/1.1 requests.
 * - `encode()`: HTTP servers do not encode requests.
 *              Use `http::Response` and `http::Response::serialize()` for response serialization.
 * - `reset()` : Recreates the parser to prepare for parsing the next request.
 *
 * ### Usage Example
 * @code
 * Http1Codec codec;
 * Request req;
 * auto status = codec.decode(recv_buf, req);
 * if (status == DecodeStatus::Complete) {
 *   handle_request(req);
 *   codec.reset(); // keep-alive: prepare for next request
 * } else if (status == DecodeStatus::Error) {
 *   send_400_bad_request();
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/codec/frame_codec.hpp>
#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>

#include <memory_resource>
#include <string_view>

namespace qbuem::codec {

/**
 * @brief Codec that wraps the HTTP/1.1 request parser in the `IFrameCodec<Request>` interface.
 *
 * Internally maintains an `HttpParser` state machine.
 * Calling `reset()` reinitializes the parser so it can parse the next HTTP request.
 * Suitable for processing consecutive requests over HTTP keep-alive connections.
 *
 * ### encode() Notes
 * `encode(Request)` has no meaning in an HTTP server codec.
 * It always returns 0, which corresponds to `DecodeStatus::Error`.
 * Use `http::Response` for HTTP response serialization.
 */
class Http1Codec : public IFrameCodec<Request> {
public:
  /** @brief Default constructor. Initializes the HttpParser. */
  Http1Codec() = default;

  /**
   * @brief Decodes an HTTP/1.1 request from a buffer.
   *
   * Feeds data to the internal `HttpParser` to incrementally parse the request.
   * The completed request is stored in `out`.
   *
   * On success, `buf` advances by the number of consumed bytes.
   * Call `reset()` before parsing the next request.
   *
   * @param[in,out] buf Raw byte view. Advances by consumed bytes on `Complete`.
   * @param[out]    out Decoded HTTP request. Valid only on `Complete`.
   * @returns `Complete` = request complete, `Incomplete` = insufficient data,
   *          `Error` = HTTP parse error (400/413, etc.).
   */
  DecodeStatus decode(BufferView &buf, Request &out) override {
    if (buf.empty()) return DecodeStatus::Incomplete;

    std::string_view sv(reinterpret_cast<const char *>(buf.data()), buf.size());
    auto consumed = parser_.parse(sv, out);

    if (!consumed.has_value()) {
      return DecodeStatus::Error;
    }

    if (!parser_.is_complete()) {
      // All bytes consumed but not yet complete
      buf = buf.subspan(buf.size());
      return DecodeStatus::Incomplete;
    }

    buf = buf.subspan(*consumed);
    return DecodeStatus::Complete;
  }

  /**
   * @brief HTTP request encoding — not supported.
   *
   * HTTP servers do not encode requests.
   * Use `http::Response::serialize()` for response encoding.
   *
   * @returns Always 0 (failure).
   */
  size_t encode(const Request & /*frame*/, iovec * /*vecs*/, size_t /*max_vecs*/,
                std::pmr::memory_resource * /*arena*/) override {
    return 0;
  }

  /**
   * @brief Reinitializes the HTTP parser.
   *
   * Call after processing a previous request on a keep-alive connection to prepare
   * for parsing the next request. Also call for error recovery.
   */
  void reset() override {
    parser_ = HttpParser{};
  }

  /**
   * @brief Returns whether HTTP header parsing is complete.
   *
   * Used when handling `Expect: 100-continue` requests to determine
   * the point at which a `100 Continue` response should be sent.
   *
   * @returns true if the header section has been fully parsed.
   */
  [[nodiscard]] bool headers_complete() const noexcept {
    return parser_.headers_complete();
  }

  /**
   * @brief Returns the HTTP status code corresponding to a parse error.
   *
   * - 400: Malformed HTTP request syntax
   * - 413: Payload exceeds maximum allowed size
   *
   * @returns HTTP error status code (400 or 413).
   */
  [[nodiscard]] int error_status() const noexcept {
    return parser_.error_status();
  }

private:
  /** @brief HTTP/1.1 request parser state machine. */
  HttpParser parser_;
};

} // namespace qbuem::codec

/** @} */ // end of qbuem_codec (http1)
