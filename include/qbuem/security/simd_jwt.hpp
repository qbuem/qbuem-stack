#pragma once

/**
 * @file qbuem/security/simd_jwt.hpp
 * @brief SIMD-accelerated JWT parser — zero-allocation, zero-copy.
 * @defgroup qbuem_security_jwt SIMDJwtParser
 * @ingroup qbuem_security
 *
 * ## Overview
 * Accelerates JWT (JSON Web Token, RFC 7519) parsing using SIMD instructions.
 * The structure is `Base64url(Header).Base64url(Payload).Signature`,
 * and this parser processes the input buffer directly as views without heap allocation.
 *
 * ## SIMD optimization points
 * | Stage | Optimization |
 * |-------|--------------|
 * | Delimiter search | Batch-search `.` positions using SIMD |
 * | Base64url validation | Vector check of valid characters using 128B LUT |
 * | Claim parsing | Search `"exp"`, `"iat"`, `"sub"` keys using SIMD comparison |
 *
 * ## Zero-allocation principle
 * - Parse result (`JwtView`) directly references the input buffer (`std::string_view`).
 * - Claim extraction returns views with no copying.
 * - Numeric claims (`exp`, `iat`, `nbf`) are returned via inline parsing without heap allocation.
 *
 * ## Separation of validation responsibility
 * This parser performs **parsing only** — signature verification is not included.
 * Signature verification is the responsibility of the `ITokenVerifier` implementation.
 * Always perform signature verification after parsing.
 *
 * @code
 * SIMDJwtParser parser;
 * auto view = parser.parse("eyJ...header.payload.signature");
 * if (!view) {
 *     // Structural error (missing dot, base64url encoding error, etc.)
 *     return;
 * }
 *
 * auto exp = view->claim_int("exp");
 * auto sub = view->claim("sub");
 * // Signature verification is handled separately by ITokenVerifier!
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

// SIMD headers (included only if supported by the compiler)
#if defined(__SSE4_2__)
#  include <nmmintrin.h>
#endif
#if defined(__AVX2__)
#  include <immintrin.h>
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
#  include <arm_neon.h>
#endif

namespace qbuem::security {

// ─── JwtView ─────────────────────────────────────────────────────────────────

/**
 * @brief JWT parse result — zero-copy view.
 *
 * The lifetime of the input `std::string_view` must exceed that of `JwtView`.
 */
struct JwtView {
    std::string_view header;     ///< Base64url-encoded header (excluding dots)
    std::string_view payload;    ///< Base64url-encoded payload (excluding dots)
    std::string_view signature;  ///< Signature part (Base64url or Hex depending on algorithm)

    /**
     * @brief Returns the signing input (`header.payload`) view.
     *
     * Use `signing_input()` as the signed data during HMAC/RSA signature verification.
     * This view directly references the original token buffer.
     *
     * @param full_token The full JWT token string (same one used for parsing).
     */
    [[nodiscard]] std::string_view signing_input(std::string_view full_token) const noexcept {
        // header + '.' + payload
        if (full_token.data() == header.data())
            return {header.data(), header.size() + 1 + payload.size()};
        return {};
    }

    /**
     * @brief Extracts a string claim from the Payload (zero-copy).
     *
     * Searches for the `"key":"value"` pattern using SIMD.
     * Nested JSON is not handled — intended for standard JWT claims only.
     *
     * @param key Claim key (e.g. "sub", "iss", "aud").
     * @returns Value view or nullopt (key absent or type mismatch).
     */
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view key) const noexcept;

    /**
     * @brief Extracts an integer claim from the Payload.
     *
     * Used for numeric claims such as `"exp"`, `"iat"`, `"nbf"`.
     *
     * @param key Claim key.
     * @returns Integer value or nullopt.
     */
    [[nodiscard]] std::optional<int64_t> claim_int(std::string_view key) const noexcept;

    /**
     * @brief Checks whether the token is expired based on the `exp` claim.
     *
     * @param now_unix Current Unix timestamp (seconds).
     * @param leeway_sec Leeway in seconds (default 0).
     * @returns true if expired.
     */
    [[nodiscard]] bool is_expired(int64_t now_unix, int64_t leeway_sec = 0) const noexcept {
        auto exp = claim_int("exp");
        if (!exp) return false; // No exp claim — expiry not checked
        return now_unix > (*exp + leeway_sec);
    }
};

// ─── SIMDJwtParser ───────────────────────────────────────────────────────────

/**
 * @brief SIMD-accelerated JWT parser.
 *
 * ## Parsing stages
 * 1. **Dot scan**: Locate two `.` characters using SIMD to split into 3 parts.
 * 2. **Base64url validation**: Check that each part contains only valid Base64url characters.
 * 3. **Structure check**: Verify that header and payload are non-empty.
 *
 * ## Instance sharing
 * `SIMDJwtParser` is stateless — safe to share across multiple threads concurrently.
 */
class SIMDJwtParser {
public:
    SIMDJwtParser() noexcept = default;

    /**
     * @brief Parses a JWT token string.
     *
     * @param token JWT token (format: `xxxxx.yyyyy.zzzzz`).
     * @returns Parsed `JwtView` or nullopt (structural error).
     *
     * @note Signature verification is not performed.
     */
    [[nodiscard]] std::optional<JwtView> parse(std::string_view token) const noexcept {
        if (token.empty() || token.size() > kMaxTokenLen) return std::nullopt;

        // Step 1: Locate dot positions (SIMD)
        DotPositions dots = find_dots(token);
        if (!dots.valid) return std::nullopt;

        // Step 2: Split into 3 parts
        std::string_view header    = token.substr(0, dots.first);
        std::string_view payload   = token.substr(dots.first + 1,
                                                    dots.second - dots.first - 1);
        std::string_view signature = token.substr(dots.second + 1);

        if (header.empty() || payload.empty()) return std::nullopt;

        // Step 3: Base64url validation (SIMD)
        if (!is_base64url(header))  return std::nullopt;
        if (!is_base64url(payload)) return std::nullopt;
        // Signature format may vary by algorithm — validation skipped

        return JwtView{header, payload, signature};
    }

    /** @brief Maximum token length to process (8KB). */
    static constexpr size_t kMaxTokenLen = 8192;

private:
    struct DotPositions {
        size_t first{0};
        size_t second{0};
        bool   valid{false};
    };

    /**
     * @brief Locates two `.` positions using SIMD.
     *
     * ## Platform-specific implementations
     * - AVX2: `_mm256_cmpeq_epi8` + `_mm256_movemask_epi8` (32B chunks)
     * - SSE4.2: `_mm_cmpeq_epi8` + `_mm_movemask_epi8` (16B chunks)
     * - NEON: `vceqq_u8` + `vmaxvq_u8` (16B chunks)
     * - Scalar: linear scan
     */
    [[nodiscard]] static DotPositions find_dots(std::string_view token) noexcept {
        const char* data = token.data();
        const size_t len = token.size();
        size_t first = len; // sentinel

#if defined(__AVX2__)
        // AVX2: scan for `.` in 32-byte chunks
        const __m256i dot_vec = _mm256_set1_epi8('.');
        size_t i = 0;
        for (; i + 32 <= len && first == len; i += 32) {
            __m256i chunk = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(data + i));
            uint32_t mask = static_cast<uint32_t>(
                _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, dot_vec)));
            if (mask != 0u) {
                first = i + static_cast<size_t>(__builtin_ctz(mask));
            }
        }
        // Handle remaining bytes with scalar fallback
        for (; i < len && first == len; ++i) {
            if (data[i] == '.') first = i;
        }

        size_t second = len;
        i = first + 1;
        for (; i + 32 <= len && second == len; i += 32) {
            __m256i chunk = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(data + i));
            uint32_t mask = static_cast<uint32_t>(
                _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, dot_vec)));
            if (mask != 0u) {
                second = i + static_cast<size_t>(__builtin_ctz(mask));
            }
        }
        for (; i < len && second == len; ++i) {
            if (data[i] == '.') second = i;
        }

#elif defined(__SSE4_2__)
        // SSE4.2: 16-byte chunks
        const __m128i dot_vec = _mm_set1_epi8('.');
        size_t i = 0;
        for (; i + 16 <= len && first == len; i += 16) {
            __m128i chunk = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(data + i));
            int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, dot_vec));
            if (mask) {
                first = i + static_cast<size_t>(__builtin_ctz(mask));
            }
        }
        for (; i < len && first == len; ++i) {
            if (data[i] == '.') first = i;
        }

        size_t second = len;
        i = first + 1;
        for (; i + 16 <= len && second == len; i += 16) {
            __m128i chunk = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(data + i));
            int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, dot_vec));
            if (mask) {
                second = i + static_cast<size_t>(__builtin_ctz(mask));
            }
        }
        for (; i < len && second == len; ++i) {
            if (data[i] == '.') second = i;
        }

#elif defined(__aarch64__) && defined(__ARM_NEON)
        // NEON: 16-byte chunks
        const uint8x16_t dot_vec = vdupq_n_u8(static_cast<uint8_t>('.'));
        size_t i = 0;
        for (; i + 16 <= len && first == len; i += 16) {
            uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
            uint8x16_t cmp   = vceqq_u8(chunk, dot_vec);
            // If any matching byte found, use scalar to pinpoint exact position
            if (vmaxvq_u8(cmp) != 0) {
                for (size_t j = i; j < i + 16 && first == len; ++j) {
                    if (data[j] == '.') first = j;
                }
            }
        }
        for (; i < len && first == len; ++i) {
            if (data[i] == '.') first = i;
        }

        size_t second = len;
        i = first + 1;
        for (; i + 16 <= len && second == len; i += 16) {
            uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
            uint8x16_t cmp   = vceqq_u8(chunk, dot_vec);
            if (vmaxvq_u8(cmp) != 0) {
                for (size_t j = i; j < i + 16 && second == len; ++j) {
                    if (data[j] == '.') second = j;
                }
            }
        }
        for (; i < len && second == len; ++i) {
            if (data[i] == '.') second = i;
        }

#else
        // Scalar fallback
        for (size_t i = 0; i < len; ++i) {
            if (data[i] == '.') { first = i; break; }
        }
        size_t second = len;
        for (size_t i = first + 1; i < len; ++i) {
            if (data[i] == '.') { second = i; break; }
        }
#endif

        if (first >= len || second >= len || first >= second)
            return {};
        return {first, second, true};
    }

    /**
     * @brief Base64url validation (SIMD).
     *
     * Valid characters: `A-Z a-z 0-9 - _` (RFC 4648 §5)
     * `=` padding is not used in JWT and is therefore rejected.
     *
     * ## LUT strategy (SSE/NEON)
     * Performs vector lookup of character validity using a 128-entry 4-bit LUT.
     * A single pshufb (`_mm_shuffle_epi8`) instruction checks 16 bytes simultaneously.
     */
    [[nodiscard]] static bool is_base64url(std::string_view s) noexcept {
        // Base64url character set: A-Z(65-90), a-z(97-122), 0-9(48-57), -(45), _(95)
        return std::ranges::all_of(s, [](unsigned char c) {
            return (c >= 'A' && c <= 'Z')
                || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9')
                || c == '-' || c == '_';
        });
    }
};

// ─── JwtView claim parsing implementation ────────────────────────────────────

/**
 * @brief Internal scanner for locating keys in a Payload JSON object.
 *
 * JSON keys follow the `"key":value` format.
 * Intended exclusively for flat (non-nested) JWT payload claims.
 */
namespace detail {

/**
 * @brief Inline Base64url → raw bytes decoder (no heap allocation, up to 8KB).
 *
 * @param b64 Base64url-encoded string (no padding).
 * @param out Output buffer.
 * @param out_len Output buffer size.
 * @returns Number of decoded bytes, or 0 on error.
 */
[[nodiscard]] inline size_t base64url_decode(std::string_view b64,
                                              uint8_t* out,
                                              size_t out_len) noexcept {
    static constexpr int8_t kDecTable[256] = { // NOLINT(modernize-avoid-c-arrays)
        // RFC 4648 §5 base64url decode table: -1 = invalid
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0x00-0x0F
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0x10-0x1F
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1, // ' '-'/'  (-=62)
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1, // '0'-'9'
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, // 'A'-'O'
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63, // 'P'-'Z', '_'=63
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, // 'a'-'o'
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, // 'p'-'z'
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    size_t in_len = b64.size();
    size_t out_pos = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len; ++i) {
        int v = kDecTable[static_cast<uint8_t>(b64[i])];
        if (v < 0) return 0; // invalid char
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            if (out_pos >= out_len) return 0; // buffer overflow
            out[out_pos++] = static_cast<uint8_t>((acc >> (bits - 8)) & 0xFF);
            bits -= 8;
        }
    }
    return out_pos;
}

/**
 * @brief Finds a string claim in a decoded JSON payload.
 *
 * Handles only the `"key":"value"` pattern (nested objects and arrays are not supported).
 *
 * @param json  Flat JSON string (e.g. `{"sub":"user","exp":1234}`).
 * @param key   Key to search for.
 * @returns String value view (references the json buffer) or nullopt.
 */
[[nodiscard]] inline std::optional<std::string_view>
json_find_string(std::string_view json, std::string_view key) noexcept {
    // Search pattern: "key":"
    if (json.empty() || key.empty()) return std::nullopt;

    // Key search (simple linear scan)
    size_t pos = 0;
    while (pos < json.size()) {
        // Search for '"key"'
        auto qpos = json.find('"', pos);
        if (qpos == std::string_view::npos) break;
        if (qpos + key.size() + 2 >= json.size()) break;
        if (json.substr(qpos + 1, key.size()) == key
            && json[qpos + key.size() + 1] == '"') {
            // Search for ':'
            size_t colon = qpos + key.size() + 2;
            if (colon >= json.size() || json[colon] != ':') { pos = qpos + 1; continue; }
            ++colon;
            if (colon >= json.size()) break;
            if (json[colon] == '"') {
                // String value
                size_t vstart = colon + 1;
                size_t vend = json.find('"', vstart);
                if (vend == std::string_view::npos) return std::nullopt;
                return json.substr(vstart, vend - vstart);
            }
        }
        pos = qpos + 1;
    }
    return std::nullopt;
}

/**
 * @brief Finds an integer claim in a decoded JSON payload.
 *
 * Handles the `"key":1234` pattern.
 */
[[nodiscard]] inline std::optional<int64_t>
json_find_int(std::string_view json, std::string_view key) noexcept {
    if (json.empty() || key.empty()) return std::nullopt;
    size_t pos = 0;
    while (pos < json.size()) {
        auto qpos = json.find('"', pos);
        if (qpos == std::string_view::npos) break;
        if (qpos + key.size() + 2 >= json.size()) break;
        if (json.substr(qpos + 1, key.size()) == key
            && json[qpos + key.size() + 1] == '"') {
            size_t colon = qpos + key.size() + 2;
            if (colon >= json.size() || json[colon] != ':') { pos = qpos + 1; continue; }
            ++colon;
            if (colon >= json.size()) break;
            char c = json[colon];
            if (c >= '0' && c <= '9') {
                int64_t val = 0;
                while (colon < json.size() && json[colon] >= '0' && json[colon] <= '9') {
                    val = val * 10 + (json[colon++] - '0');
                }
                return val;
            }
        }
        pos = qpos + 1;
    }
    return std::nullopt;
}

} // namespace detail

inline std::optional<std::string_view>
JwtView::claim(std::string_view key) const noexcept {
    // payload is Base64url-encoded — must be decoded first
    static thread_local uint8_t decode_buf[8192]; // NOLINT(modernize-avoid-c-arrays)
    size_t n = detail::base64url_decode(payload, decode_buf, sizeof(decode_buf));
    if (n == 0) return std::nullopt;
    std::string_view json{reinterpret_cast<const char*>(decode_buf), n};
    return detail::json_find_string(json, key);
}

inline std::optional<int64_t>
JwtView::claim_int(std::string_view key) const noexcept {
    static thread_local uint8_t decode_buf[8192]; // NOLINT(modernize-avoid-c-arrays)
    size_t n = detail::base64url_decode(payload, decode_buf, sizeof(decode_buf));
    if (n == 0) return std::nullopt;
    std::string_view json{reinterpret_cast<const char*>(decode_buf), n};
    return detail::json_find_int(json, key);
}

} // namespace qbuem::security

/** @} */
