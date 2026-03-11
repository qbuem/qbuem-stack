#include <draco/http/parser.hpp>

#include <cctype>
#include <string>

namespace draco {

// ---------------------------------------------------------------------------
// Helper: parse a hex character → value, or -1 on bad input.
// ---------------------------------------------------------------------------
static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ---------------------------------------------------------------------------
// HttpParser::parse
//
// Two-phase design:
//   Phase 1 – request-line + headers (byte-by-byte state machine).
//   Phase 2 – body (skip-ahead: fixed-length or chunked).
// ---------------------------------------------------------------------------
std::optional<size_t> HttpParser::parse(std::string_view data, Request &req) {
  size_t pos   = 0;
  size_t start = 0;

  // ── Phase 1: request line + headers ──────────────────────────────────────
  while (pos < data.size() &&
         state_ != State::Body      &&
         state_ != State::ChunkSize &&
         state_ != State::Complete  &&
         state_ != State::Error) {

    char c = data[pos];

    switch (state_) {

    case State::Method:
      if (c == ' ') {
        req.set_method(string_to_method(data.substr(start, pos - start)));
        state_ = State::Path;
        start  = pos + 1;
      }
      break;

    case State::Path:
      if (c == ' ') {
        req.set_path(data.substr(start, pos - start));
        state_ = State::Version;
        start  = pos + 1;
      }
      break;

    case State::Version:
      if (c == '\n') {
        state_ = State::HeaderKey;
        start  = pos + 1;
      }
      break;

    case State::HeaderKey:
      if (c == ':') {
        current_header_key_ = data.substr(start, pos - start);
        state_ = State::HeaderValue;
        start  = pos + 1;
        // Skip optional single space after ':'
        if (pos + 1 < data.size() && data[pos + 1] == ' ') {
          ++pos;
          ++start;
        }
      } else if (c == '\n') {
        // Empty header line → end of headers
        headers_complete_ = true;

        // Determine body mode
        std::string_view te = req.header("Transfer-Encoding");
        std::string_view cl = req.header("Content-Length");

        if (te == "chunked") {
          chunked_ = true;
          state_   = State::ChunkSize;
          start    = pos + 1;
        } else if (!cl.empty()) {
          body_length_ = std::stoul(std::string(cl));
          if (body_length_ > MAX_BODY_SIZE) {
            error_status_ = 413;
            state_        = State::Error;
            return std::nullopt;
          }
          state_ = State::Body;
          start  = pos + 1;
        } else {
          state_ = State::Complete;
        }
      }
      // Ignore '\r'
      break;

    case State::HeaderValue:
      if (c == '\r') {
        // ignore
      } else if (c == '\n') {
        std::string_view value = data.substr(start, pos - start - 1); // strip \r
        // Header-injection guard: value must not contain bare \r or \n
        for (char ch : value) {
          if (ch == '\r' || ch == '\n') {
            error_status_ = 400;
            state_        = State::Error;
            return std::nullopt;
          }
        }
        req.add_header(current_header_key_, value);
        state_ = State::HeaderKey;
        start  = pos + 1;
      }
      break;

    default:
      break;
    }

    ++pos;
  }

  if (state_ == State::Error)    return std::nullopt;
  if (state_ == State::Complete) return pos;

  // ── Phase 2a: fixed-length body ──────────────────────────────────────────
  if (state_ == State::Body) {
    // `start` was set to (pos of first body byte) when we left phase 1.
    // But `pos` may have been incremented one extra time after the while loop.
    // Re-derive body start: it is wherever `start` was last set in phase 1.
    size_t body_start = start;
    size_t avail = (body_start < data.size()) ? (data.size() - body_start) : 0;

    if (avail < body_length_) {
      return body_start; // need more data
    }

    req.set_body(data.substr(body_start, body_length_));
    state_ = State::Complete;
    return body_start + body_length_;
  }

  // ── Phase 2b: chunked body ───────────────────────────────────────────────
  if (state_ == State::ChunkSize || state_ == State::ChunkData ||
      state_ == State::ChunkDataEnd) {
    // `pos` is already positioned at start of first chunk-size line
    // (or wherever parsing left off in a previous partial call).

    while (state_ != State::Complete && state_ != State::Error) {

      if (state_ == State::ChunkSize) {
        // Find end of chunk-size line (\r\n)
        size_t eol = data.find("\r\n", pos);
        if (eol == std::string_view::npos)
          return pos; // need more data

        // Parse hex size, ignore chunk extensions (anything after ';')
        std::string_view size_tok = data.substr(pos, eol - pos);
        size_t ext = size_tok.find(';');
        if (ext != std::string_view::npos)
          size_tok = size_tok.substr(0, ext);

        chunk_remaining_ = 0;
        for (char hc : size_tok) {
          int d = hex_digit(hc);
          if (d < 0) {
            error_status_ = 400;
            state_        = State::Error;
            return std::nullopt;
          }
          chunk_remaining_ = chunk_remaining_ * 16 + static_cast<size_t>(d);
          if (chunk_remaining_ > MAX_BODY_SIZE) {
            error_status_ = 413;
            state_        = State::Error;
            return std::nullopt;
          }
        }

        pos = eol + 2; // advance past \r\n

        if (chunk_remaining_ == 0) {
          // Last chunk (size 0).  Consume optional trailing headers and \r\n.
          size_t trailer_end = data.find("\r\n", pos);
          if (trailer_end == std::string_view::npos)
            return pos; // need more data for trailing CRLF

          req.set_body(chunk_body_);
          state_ = State::Complete;
          return trailer_end + 2;
        }

        if (chunk_body_.size() + chunk_remaining_ > MAX_BODY_SIZE) {
          error_status_ = 413;
          state_        = State::Error;
          return std::nullopt;
        }

        state_ = State::ChunkData;

      } else if (state_ == State::ChunkData) {
        size_t avail = (pos < data.size()) ? (data.size() - pos) : 0;

        if (avail < chunk_remaining_) {
          // Partial chunk — consume what we have
          chunk_body_.append(data.data() + pos, avail);
          chunk_remaining_ -= avail;
          pos += avail;
          return pos; // need more data
        }

        // Full chunk data available
        chunk_body_.append(data.data() + pos, chunk_remaining_);
        pos  += chunk_remaining_;
        chunk_remaining_ = 0;
        state_ = State::ChunkDataEnd;

      } else { // ChunkDataEnd
        // Consume the \r\n that follows each chunk's data
        if (pos + 2 > data.size())
          return pos; // need more data

        if (data[pos] != '\r' || data[pos + 1] != '\n') {
          error_status_ = 400;
          state_        = State::Error;
          return std::nullopt;
        }

        pos   += 2;
        state_ = State::ChunkSize;
      }
    }

    if (state_ == State::Error)    return std::nullopt;
    if (state_ == State::Complete) return pos;
  }

  // Incomplete (still parsing headers, or unexpected state)
  return pos;
}

} // namespace draco
