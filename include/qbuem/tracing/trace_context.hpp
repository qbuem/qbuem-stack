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
  uint8_t bytes[16]{};

  /**
   * @brief Generates a new TraceId using a cryptographically secure random number.
   *
   * Uses `qbuem::random_bytes()` internally.
   * @returns A new, valid TraceId.
   */
  static TraceId generate() {
    TraceId id;
    auto raw = qbuem::random_bytes(16);
    std::memcpy(id.bytes, raw.data(), 16);
    return id;
  }

  /**
   * @brief Checks whether this TraceId is valid.
   *
   * Per the W3C spec, all bytes being 0x00 indicates an invalid identifier.
   * @returns true if valid, false if all bytes are zero.
   */
  bool is_valid() const noexcept {
    for (uint8_t b : bytes) {
      if (b != 0x00) return true;
    }
    return false;
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
    static constexpr char kHex[] = "0123456789abcdef";
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
 * @brief 64-bit 스팬 식별자.
 *
 * W3C Trace Context 표준에서 추적 트리 내 단일 작업 단위를 식별합니다.
 * 각 스팬은 고유한 SpanId를 가집니다.
 *
 * ### 유효성
 * 8바이트가 모두 0x00이면 유효하지 않습니다 (W3C 규격).
 */
struct SpanId {
  /** @brief 64-bit 스팬 ID 원시 바이트. */
  uint8_t bytes[8]{};

  /**
   * @brief 암호학적으로 안전한 난수로 새 SpanId를 생성합니다.
   *
   * 내부적으로 `qbuem::random_bytes()`를 사용합니다.
   * @returns 유효한 새 SpanId.
   */
  static SpanId generate() {
    SpanId id;
    auto raw = qbuem::random_bytes(8);
    std::memcpy(id.bytes, raw.data(), 8);
    return id;
  }

  /**
   * @brief 이 SpanId가 유효한지 확인합니다.
   *
   * W3C 규격에 따라 모든 바이트가 0x00이면 유효하지 않습니다.
   * @returns 유효하면 true, 모두 0이면 false.
   */
  bool is_valid() const noexcept {
    for (uint8_t b : bytes) {
      if (b != 0x00) return true;
    }
    return false;
  }

  /**
   * @brief 소문자 16진수 문자열로 변환합니다 (16자).
   *
   * 예: `"00f067aa0ba902b7"`
   *
   * @param buf  출력 버퍼 포인터.
   * @param n    버퍼 크기 (최소 17 — 16자 + null terminator).
   * @returns 기록된 문자 수 (null 제외). 버퍼 부족 시 0.
   */
  size_t to_chars(char* buf, size_t n) const {
    if (n < 17) return 0;
    static constexpr char kHex[] = "0123456789abcdef";
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
 * @brief W3C Trace Context — traceparent 헤더 직렬화/역직렬화 및 스팬 계층 관리.
 *
 * `traceparent` 헤더 형식:
 * ```
 * 00-{trace_id(32)}-{parent_span_id(16)}-{flags(2)}
 * ```
 *
 * ### flags 비트
 * - bit 0 (0x01): sampled — 이 추적이 샘플링됨을 수신자에게 알림.
 *
 * ### 사용 예시
 * @code
 * // 인바운드 HTTP 요청에서 traceparent 파싱
 * auto result = TraceContext::from_traceparent(req.header("traceparent"));
 * TraceContext ctx = result ? *result : TraceContext::generate();
 *
 * // 아웃바운드 호출 시 child span 생성 후 전파
 * auto child = ctx.child_span();
 * outbound_req.set_header("traceparent", child.to_traceparent());
 * @endcode
 */
struct TraceContext {
  TraceId trace_id;         ///< 128-bit 전역 추적 식별자
  SpanId  parent_span_id;   ///< 64-bit 부모 스팬 식별자 (현재 스팬 ID)
  uint8_t flags = 1;        ///< 추적 플래그 (기본값: sampled=1)

  /**
   * @brief 새 루트 스팬 TraceContext를 생성합니다.
   *
   * 새 TraceId와 SpanId를 난수로 생성합니다.
   * @returns 유효한 루트 스팬 TraceContext (sampled 플래그 설정됨).
   */
  static TraceContext generate() {
    TraceContext ctx;
    ctx.trace_id       = TraceId::generate();
    ctx.parent_span_id = SpanId::generate();
    ctx.flags          = 1;
    return ctx;
  }

  /**
   * @brief 현재 컨텍스트에서 child span을 생성합니다.
   *
   * 동일한 trace_id를 유지하고, 새 SpanId를 생성합니다.
   * 새 스팬의 parent_span_id는 현재 스팬의 SpanId가 됩니다.
   *
   * @returns 새 SpanId를 가진 child TraceContext.
   */
  TraceContext child_span() const {
    TraceContext child;
    child.trace_id       = trace_id;
    child.parent_span_id = SpanId::generate();
    child.flags          = flags;
    return child;
  }

  /**
   * @brief W3C traceparent 헤더 문자열로 직렬화합니다.
   *
   * 형식: `"00-{trace_id}-{parent_span_id}-{flags}"`
   * 예:   `"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"`
   *
   * @returns traceparent 헤더 문자열 (55자 고정).
   */
  std::string to_traceparent() const {
    // "00-" + 32 + "-" + 16 + "-" + 2 = 55자
    char buf[56];
    buf[0] = '0'; buf[1] = '0'; buf[2] = '-';
    trace_id.to_chars(buf + 3, 33);
    buf[35] = '-';
    parent_span_id.to_chars(buf + 36, 17);
    buf[52] = '-';
    static constexpr char kHex[] = "0123456789abcdef";
    buf[53] = kHex[(flags >> 4) & 0x0F];
    buf[54] = kHex[flags & 0x0F];
    buf[55] = '\0';
    return std::string(buf, 55);
  }

  /**
   * @brief W3C traceparent 헤더 문자열을 파싱합니다.
   *
   * 유효한 형식: `"00-{32자 hex}-{16자 hex}-{2자 hex}"`
   * version 00만 지원합니다.
   *
   * @param header 파싱할 traceparent 헤더 문자열.
   * @returns 성공 시 TraceContext, 형식 오류 시 에러 Result.
   */
  static Result<TraceContext> from_traceparent(std::string_view header) {
    // 최소 길이: "00-{32}-{16}-{2}" = 55자
    if (header.size() < 55) {
      return unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    // version 확인 ("00")
    if (header[0] != '0' || header[1] != '0') {
      return unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    if (header[2] != '-' || header[35] != '-' || header[52] != '-') {
      return unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    auto hex_digit = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };

    TraceContext ctx;

    // trace_id 파싱 (32자)
    for (size_t i = 0; i < 16; ++i) {
      int hi = hex_digit(header[3 + i * 2]);
      int lo = hex_digit(header[3 + i * 2 + 1]);
      if (hi < 0 || lo < 0)
        return unexpected(std::make_error_code(std::errc::invalid_argument));
      ctx.trace_id.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }

    // parent_span_id 파싱 (16자)
    for (size_t i = 0; i < 8; ++i) {
      int hi = hex_digit(header[36 + i * 2]);
      int lo = hex_digit(header[36 + i * 2 + 1]);
      if (hi < 0 || lo < 0)
        return unexpected(std::make_error_code(std::errc::invalid_argument));
      ctx.parent_span_id.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }

    // flags 파싱 (2자)
    int fhi = hex_digit(header[53]);
    int flo = hex_digit(header[54]);
    if (fhi < 0 || flo < 0)
      return unexpected(std::make_error_code(std::errc::invalid_argument));
    ctx.flags = static_cast<uint8_t>((fhi << 4) | flo);

    // 유효성 검사
    if (!ctx.trace_id.is_valid() || !ctx.parent_span_id.is_valid())
      return unexpected(std::make_error_code(std::errc::invalid_argument));

    return ctx;
  }

  /**
   * @brief 이 추적이 샘플링되는지 확인합니다.
   * @returns flags의 bit 0이 설정되어 있으면 true.
   */
  bool is_sampled() const noexcept { return (flags & 0x01) != 0; }
};

} // namespace qbuem::tracing

/** @} */
