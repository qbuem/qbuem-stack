#pragma once

/**
 * @file qbuem/tracing/trace_context.hpp
 * @brief W3C Trace Context standard — TraceId, SpanId, TraceContext definitions.
 * @defgroup qbuem_tracing_trace_context TraceContext
 * @ingroup qbuem_tracing
 *
 * Defines distributed tracing identifiers and context types conforming to
 * W3C Trace Context Level 1 (https://www.w3.org/TR/trace-context/).
 *
 * ## Included types
 * - `TraceId`      : 128-bit global trace identifier (16 bytes)
 * - `SpanId`       : 64-bit span identifier (8 bytes)
 * - `TraceContext` : traceparent header parsing/generation and child span creation
 *
 * ## Usage example
 * @code
 * // Create a root span context
 * auto ctx = qbuem::tracing::TraceContext::generate();
 *
 * // Serialize to traceparent header
 * std::string header = ctx.to_traceparent();
 * // e.g. "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"
 *
 * // Parse from header
 * auto result = qbuem::tracing::TraceContext::from_traceparent(header);
 * if (result) {
 *     auto child = result->child_span();
 * }
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/crypto.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace qbuem::tracing {

// ─── TraceId ──────────────────────────────────────────────────────────────────

/**
 * @brief 128-bit global trace identifier.
 *
 * Identifies an entire distributed trace transaction in the W3C Trace Context standard.
 * All spans within the same trace share the same TraceId.
 *
 * ### Validity
 * All 16 bytes being 0x00 indicates an invalid identifier (per W3C spec).
 */
struct TraceId {
  /** @brief Raw bytes of the 128-bit trace ID. */
  std::array<uint8_t, 16> bytes{};

  /**
   * @brief Generates a new TraceId using a cryptographically secure random number.
   *
   * Uses `qbuem::random_bytes()` internally.
   * @returns A new, valid TraceId.
   */
  static TraceId generate() {
    TraceId id;
    auto raw = qbuem::random_bytes(16);
    std::memcpy(id.bytes.data(), raw.data(), 16);
    return id;
  }

  /**
   * @brief Checks whether this TraceId is valid.
   *
   * Per the W3C spec, all bytes being 0x00 indicates an invalid identifier.
   * @returns true if valid, false if all bytes are zero.
   */
  [[nodiscard]] bool is_valid() const noexcept {
    return std::ranges::any_of(bytes, [](uint8_t b) { return b != 0x00; });
  }

  /**
   * @brief Converts to a lowercase hex string (32 characters).
   *
   * Example: `"4bf92f3577b34da6a3ce929d0e0e4736"`
   *
   * @param buf  Output buffer pointer.
   * @param n    Buffer size (minimum 33 — 32 chars + null terminator).
   * @returns Number of characters written (excluding null). 0 if buffer is too small.
   */
  size_t to_chars(char* buf, size_t n) const {
    if (n < 33) return 0;
    static constexpr char kHex[] = "0123456789abcdef"; // NOLINT(modernize-avoid-c-arrays)
    for (size_t i = 0; i < 16; ++i) {
      buf[i * 2]     = kHex[(bytes[i] >> 4) & 0x0F];
      buf[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    buf[32] = '\0';
    return 32;
  }
};

// ─── SpanId ───────────────────────────────────────────────────────────────────

/**
 * @brief 64-bit span identifier.
 *
 * Identifies a single unit of work within the trace tree in the W3C Trace Context standard.
 * Each span has a unique SpanId.
 *
 * ### Validity
 * All 8 bytes being 0x00 indicates an invalid identifier (per W3C spec).
 */
struct SpanId {
  /** @brief Raw bytes of the 64-bit span ID. */
  std::array<uint8_t, 8> bytes{};

  /**
   * @brief Generates a new SpanId using a cryptographically secure random number.
   *
   * Uses `qbuem::random_bytes()` internally.
   * @returns A new, valid SpanId.
   */
  static SpanId generate() {
    SpanId id;
    auto raw = qbuem::random_bytes(8);
    std::memcpy(id.bytes.data(), raw.data(), 8);
    return id;
  }

  /**
   * @brief Checks whether this SpanId is valid.
   *
   * Per the W3C spec, all bytes being 0x00 indicates an invalid identifier.
   * @returns true if valid, false if all bytes are zero.
   */
  [[nodiscard]] bool is_valid() const noexcept {
    return std::ranges::any_of(bytes, [](uint8_t b) { return b != 0x00; });
  }

  /**
   * @brief Converts to a lowercase hex string (16 characters).
   *
   * Example: `"00f067aa0ba902b7"`
   *
   * @param buf  Output buffer pointer.
   * @param n    Buffer size (minimum 17 — 16 chars + null terminator).
   * @returns Number of characters written (excluding null). 0 if buffer is too small.
   */
  size_t to_chars(char* buf, size_t n) const {
    if (n < 17) return 0;
    static constexpr char kHex[] = "0123456789abcdef"; // NOLINT(modernize-avoid-c-arrays)
    for (size_t i = 0; i < 8; ++i) {
      buf[i * 2]     = kHex[(bytes[i] >> 4) & 0x0F];
      buf[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    buf[16] = '\0';
    return 16;
  }
};

// ─── TraceContext ──────────────────────────────────────────────────────────────

/**
 * @brief W3C Trace Context — traceparent header serialization/deserialization and span hierarchy management.
 *
 * `traceparent` header format:
 * ```
 * 00-{trace_id(32)}-{parent_span_id(16)}-{flags(2)}
 * ```
 *
 * ### flags bits
 * - bit 0 (0x01): sampled — notifies the receiver that this trace is being sampled.
 *
 * ### Usage example
 * @code
 * // Parse traceparent from an inbound HTTP request
 * auto result = TraceContext::from_traceparent(req.header("traceparent"));
 * TraceContext ctx = result ? *result : TraceContext::generate();
 *
 * // Create a child span and propagate it on outbound calls
 * auto child = ctx.child_span();
 * outbound_req.set_header("traceparent", child.to_traceparent());
 * @endcode
 */
struct TraceContext {
  TraceId trace_id;         ///< 128-bit global trace identifier
  SpanId  parent_span_id;   ///< 64-bit parent span identifier (current span ID)
  uint8_t flags = 1;        ///< Trace flags (default: sampled=1)

  /**
   * @brief Generates a new root span TraceContext.
   *
   * Generates a new TraceId and SpanId using random numbers.
   * @returns A valid root span TraceContext (with the sampled flag set).
   */
  static TraceContext generate() {
    TraceContext ctx;
    ctx.trace_id       = TraceId::generate();
    ctx.parent_span_id = SpanId::generate();
    ctx.flags          = 1;
    return ctx;
  }

  /**
   * @brief Creates a child span from the current context.
   *
   * Keeps the same trace_id and generates a new SpanId.
   * The new span's parent_span_id becomes the current span's SpanId.
   *
   * @returns A child TraceContext with a new SpanId.
   */
  [[nodiscard]] TraceContext child_span() const {
    TraceContext child;
    child.trace_id       = trace_id;
    child.parent_span_id = SpanId::generate();
    child.flags          = flags;
    return child;
  }

  /**
   * @brief Serializes to a W3C traceparent header string.
   *
   * Format: `"00-{trace_id}-{parent_span_id}-{flags}"`
   * Example: `"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"`
   *
   * @returns traceparent header string (fixed 55 characters).
   */
  [[nodiscard]] std::string to_traceparent() const {
    // "00-" + 32 + "-" + 16 + "-" + 2 = 55 chars
    char buf[56]; // NOLINT(modernize-avoid-c-arrays)
    buf[0] = '0'; buf[1] = '0'; buf[2] = '-';
    trace_id.to_chars(buf + 3, 33);
    buf[35] = '-';
    parent_span_id.to_chars(buf + 36, 17);
    buf[52] = '-';
    static constexpr char kHex[] = "0123456789abcdef"; // NOLINT(modernize-avoid-c-arrays)
    buf[53] = kHex[(flags >> 4) & 0x0F];
    buf[54] = kHex[flags & 0x0F];
    buf[55] = '\0';
    return std::string(buf, 55);
  }

  /**
   * @brief Parses a W3C traceparent header string.
   *
   * Valid format: `"00-{32 hex chars}-{16 hex chars}-{2 hex chars}"`
   * Only version 00 is supported.
   *
   * @param header traceparent header string to parse.
   * @returns TraceContext on success, error Result on format error.
   */
  static Result<TraceContext> from_traceparent(std::string_view header) {
    // Minimum length: "00-{32}-{16}-{2}" = 55 chars
    if (header.size() < 55) {
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    // Verify version ("00")
    if (header[0] != '0' || header[1] != '0') {
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    if (header[2] != '-' || header[35] != '-' || header[52] != '-') {
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    auto hex_digit = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };

    TraceContext ctx;

    // Parse trace_id (32 chars)
    for (size_t i = 0; i < 16; ++i) {
      int hi = hex_digit(header[3 + i * 2]);
      int lo = hex_digit(header[3 + i * 2 + 1]);
      if (hi < 0 || lo < 0)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      ctx.trace_id.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }

    // Parse parent_span_id (16 chars)
    for (size_t i = 0; i < 8; ++i) {
      int hi = hex_digit(header[36 + i * 2]);
      int lo = hex_digit(header[36 + i * 2 + 1]);
      if (hi < 0 || lo < 0)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      ctx.parent_span_id.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }

    // Parse flags (2 chars)
    int fhi = hex_digit(header[53]);
    int flo = hex_digit(header[54]);
    if (fhi < 0 || flo < 0)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    ctx.flags = static_cast<uint8_t>((fhi << 4) | flo);

    // Validity check
    if (!ctx.trace_id.is_valid() || !ctx.parent_span_id.is_valid())
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    return ctx;
  }

  /**
   * @brief Checks whether this trace is being sampled.
   * @returns true if bit 0 of flags is set.
   */
  [[nodiscard]] bool is_sampled() const noexcept { return (flags & 0x01) != 0; }
};

} // namespace qbuem::tracing

/** @} */
