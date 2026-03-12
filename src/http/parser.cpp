#include <draco/http/parser.hpp>

#include <cctype>
#include <cstring>
#include <string>

// ── SIMD availability detection ───────────────────────────────────────────────
#if defined(__AVX2__)
#  include <immintrin.h>
#  define DRACO_HAS_AVX2 1
#elif defined(__SSE2__)
#  include <emmintrin.h>
#  define DRACO_HAS_SSE2 1
#elif defined(__ARM_NEON)
#  include <arm_neon.h>
#  define DRACO_HAS_NEON 1
#endif

namespace draco {

// ---------------------------------------------------------------------------
// find_header_end — locate "\r\n\r\n" in [data, data+len).
// Returns offset of the first '\r' in the terminator, or SIZE_MAX if not found.
// Uses SIMD to scan 16/32 bytes at a time on x86/ARM.
// ---------------------------------------------------------------------------
static size_t find_header_end(const char *data, size_t len) noexcept {
  if (len < 4) return SIZE_MAX;

#if defined(DRACO_HAS_AVX2)
  const __m256i v_cr = _mm256_set1_epi8('\r');
  size_t i = 0;
  for (; i + 32 <= len; i += 32) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
    uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, v_cr)));
    while (mask) {
      int bit = __builtin_ctz(mask);
      size_t off = i + static_cast<size_t>(bit);
      if (off + 3 < len &&
          data[off+1] == '\n' && data[off+2] == '\r' && data[off+3] == '\n')
        return off;
      mask &= mask - 1;
    }
  }
  // Scalar tail
  for (; i + 3 < len; ++i) {
    if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n')
      return i;
  }
#elif defined(DRACO_HAS_SSE2)
  const __m128i v_cr = _mm_set1_epi8('\r');
  size_t i = 0;
  for (; i + 16 <= len; i += 16) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
    uint32_t mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, v_cr)));
    while (mask) {
      int bit = __builtin_ctz(mask);
      size_t off = i + static_cast<size_t>(bit);
      if (off + 3 < len &&
          data[off+1] == '\n' && data[off+2] == '\r' && data[off+3] == '\n')
        return off;
      mask &= mask - 1;
    }
  }
  for (; i + 3 < len; ++i) {
    if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n')
      return i;
  }
#elif defined(DRACO_HAS_NEON)
  const uint8x16_t v_cr = vdupq_n_u8(static_cast<uint8_t>('\r'));
  size_t i = 0;
  for (; i + 16 <= len; i += 16) {
    uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t *>(data + i));
    uint8x16_t cmp   = vceqq_u8(chunk, v_cr);
    // Extract 16-bit mask via vget_lane
    uint64_t lo, hi;
    vst1_u64(&lo, vreinterpret_u64_u8(vget_low_u8(cmp)));
    vst1_u64(&hi, vreinterpret_u64_u8(vget_high_u8(cmp)));
    uint64_t mask64 = lo | (hi ? ~0ULL : 0ULL); // simplified — use byte scan
    // Scalar check on potential hits
    for (int b = 0; b < 16 && i + b + 3 < len; ++b) {
      if (data[i+b] == '\r' && data[i+b+1] == '\n' &&
          data[i+b+2] == '\r' && data[i+b+3] == '\n')
        return i + static_cast<size_t>(b);
    }
    (void)mask64;
  }
  for (; i + 3 < len; ++i) {
    if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n')
      return i;
  }
#else
  // Scalar fallback — use memmem-equivalent loop
  for (size_t i = 0; i + 3 < len; ++i) {
    if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n')
      return i;
  }
#endif
  return SIZE_MAX;
}

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

  // ── SIMD fast-path: if headers are not yet complete, check whether the
  //    header terminator (\r\n\r\n) has arrived before running the FSM.
  //    This avoids re-running the byte-by-byte FSM on incomplete data.
  if (!headers_complete_ && state_ != State::Body &&
      state_ != State::ChunkSize && state_ != State::Complete &&
      state_ != State::Error) {
    if (find_header_end(data.data(), data.size()) == SIZE_MAX) {
      // Headers incomplete — nothing to parse yet.
      header_bytes_ += data.size();
      if (header_bytes_ > MAX_HEADER_SIZE) {
        error_status_ = 400;
        state_ = State::Error;
        return std::nullopt;
      }
      return data.size(); // consumed but not complete
    }
    // Reset header_bytes_; we'll count properly in the FSM below.
    header_bytes_ = 0;
  }

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

        // ── HTTP Request Smuggling guard ──────────────────────────────────
        // RFC 7230 §3.3.3 rule 3: if both TE and CL are present, reject.
        std::string_view te = req.header("Transfer-Encoding");
        std::string_view cl = req.header("Content-Length");

        if (!te.empty() && !cl.empty()) {
          error_status_ = 400;
          state_        = State::Error;
          return std::nullopt;
        }

        // Determine body mode
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
      } else {
        // Track total header bytes for Large Header Bomb protection
        ++header_bytes_;
        if (header_bytes_ > MAX_HEADER_SIZE) {
          error_status_ = 400;
          state_        = State::Error;
          return std::nullopt;
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
        header_bytes_ += current_header_key_.size() + value.size() + 4; // ": \r\n"
        if (header_bytes_ > MAX_HEADER_SIZE) {
          error_status_ = 400;
          state_        = State::Error;
          return std::nullopt;
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
