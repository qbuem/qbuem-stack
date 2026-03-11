#pragma once

#include <draco/http/request.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace draco {

/**
 * @brief State-machine HTTP/1.1 parser.
 *
 * Supports:
 *   - Fixed-length bodies (Content-Length)
 *   - Chunked Transfer Encoding (Transfer-Encoding: chunked)
 *   - max_body_size enforcement (default 1 MiB → 413)
 *   - Expect: 100-continue detection via headers_complete()
 *
 * Usage:
 *   The parser is persistent across multiple parse() calls on the same
 *   accumulation buffer.  Re-create for each new request.
 */
class HttpParser {
public:
  enum class State {
    Method,
    Path,
    Version,
    HeaderKey,
    HeaderValue,
    Body,         // fixed-length body (Content-Length)
    ChunkSize,    // reading hex chunk-size line
    ChunkData,    // reading chunk_remaining_ bytes of chunk data
    ChunkDataEnd, // consuming \r\n that follows chunk data
    Complete,
    Error
  };

  // Default max body size: 1 MiB
  static constexpr size_t MAX_BODY_SIZE   = 1u * 1024u * 1024u;
  // Default max total header section size: 8 KiB
  static constexpr size_t MAX_HEADER_SIZE = 8u * 1024u;

  HttpParser() = default;

  /**
   * Parse bytes from @p data into @p req.
   *
   * @return Number of bytes consumed on success (even if not yet complete),
   *         std::nullopt on a hard parse error (bad syntax, payload too large).
   */
  std::optional<size_t> parse(std::string_view data, Request &req);

  bool is_complete()      const { return state_ == State::Complete; }
  State state()           const { return state_; }

  /**
   * True once all headers have been read (body may still be pending).
   * Useful for sending 100 Continue before the body arrives.
   */
  bool headers_complete() const { return headers_complete_; }

  /**
   * HTTP status code to send when the parser returns nullopt:
   *   400 — bad syntax / header injection
   *   413 — Payload Too Large
   */
  int error_status() const { return error_status_; }

private:
  State       state_              = State::Method;
  bool        headers_complete_   = false;
  bool        chunked_            = false;

  std::string_view current_header_key_;
  size_t      body_length_        = 0;  // Content-Length value
  size_t      chunk_remaining_    = 0;  // bytes remaining in current chunk
  std::string chunk_body_;              // dechunked body accumulator
  int         error_status_       = 0;
  size_t      header_bytes_       = 0; // total bytes consumed by headers so far
};

} // namespace draco
