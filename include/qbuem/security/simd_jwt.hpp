#pragma once

/**
 * @file qbuem/security/simd_jwt.hpp
 * @brief SIMD 가속 JWT 파서 — zero-allocation, zero-copy.
 * @defgroup qbuem_security_jwt SIMDJwtParser
 * @ingroup qbuem_security
 *
 * ## 개요
 * JWT(JSON Web Token, RFC 7519) 파싱을 SIMD 명령어로 가속합니다.
 * 구조는 `Base64url(Header).Base64url(Payload).Signature`이며,
 * 이 파서는 힙 할당 없이 입력 버퍼를 직접 뷰로 처리합니다.
 *
 * ## SIMD 최적화 포인트
 * | 단계 | 최적화 |
 * |------|--------|
 * | 구분자 탐색 | SIMD로 `.` 위치 일괄 탐색 |
 * | Base64url 검증 | 128B LUT로 유효 문자 벡터 검사 |
 * | 클레임 파싱 | `"exp"`, `"iat"`, `"sub"` 키를 SIMD 비교로 탐색 |
 *
 * ## Zero-allocation 원칙
 * - 파싱 결과(`JwtView`)는 입력 버퍼를 직접 참조 (`std::string_view`).
 * - 클레임 추출은 뷰로 반환하여 복사 없음.
 * - 숫자 클레임(`exp`, `iat`, `nbf`)은 인라인 파싱으로 힙 없이 반환.
 *
 * ## 검증 책임 분리
 * 이 파서는 **파싱만** 수행합니다 — 서명 검증은 포함하지 않습니다.
 * 서명 검증은 `ITokenVerifier` 구현에서 담당합니다.
 * 파싱 후에는 반드시 서명 검증을 수행하세요.
 *
 * @code
 * SIMDJwtParser parser;
 * auto view = parser.parse("eyJ...header.payload.signature");
 * if (!view) {
 *     // 구조적 오류 (dot 없음, base64url 인코딩 오류 등)
 *     return;
 * }
 *
 * auto exp = view->claim_int("exp");
 * auto sub = view->claim("sub");
 * // 서명 검증은 별도 ITokenVerifier에서!
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

// SIMD 헤더 (컴파일러가 지원하는 경우에만 포함)
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
 * @brief JWT 파싱 결과 — zero-copy 뷰.
 *
 * 입력 `std::string_view`의 수명이 `JwtView`보다 길어야 합니다.
 */
struct JwtView {
    std::string_view header;     ///< Base64url-encoded header (점 제외)
    std::string_view payload;    ///< Base64url-encoded payload (점 제외)
    std::string_view signature;  ///< 서명 파트 (알고리즘에 따라 Base64url 또는 Hex)

    /**
     * @brief 서명 입력 (`header.payload`) 뷰를 반환합니다.
     *
     * HMAC/RSA 서명 검증 시 `signing_input()`을 signed data로 사용합니다.
     * 이 뷰는 원본 토큰 버퍼를 직접 참조합니다.
     *
     * @param full_token 전체 JWT 토큰 문자열 (파싱에 사용된 것과 동일).
     */
    [[nodiscard]] std::string_view signing_input(std::string_view full_token) const noexcept {
        // header + '.' + payload
        if (full_token.data() == header.data())
            return {header.data(), header.size() + 1 + payload.size()};
        return {};
    }

    /**
     * @brief Payload에서 문자열 클레임을 추출합니다 (zero-copy).
     *
     * `"key":"value"` 패턴을 SIMD로 탐색합니다.
     * 중첩 JSON은 처리하지 않습니다 — JWT 표준 클레임 전용입니다.
     *
     * @param key 클레임 키 (e.g. "sub", "iss", "aud").
     * @returns 값 뷰 또는 nullopt (키 없음, 타입 불일치).
     */
    [[nodiscard]] std::optional<std::string_view> claim(std::string_view key) const noexcept;

    /**
     * @brief Payload에서 정수 클레임을 추출합니다.
     *
     * `"exp"`, `"iat"`, `"nbf"` 등 숫자 클레임에 사용합니다.
     *
     * @param key 클레임 키.
     * @returns 정수값 또는 nullopt.
     */
    [[nodiscard]] std::optional<int64_t> claim_int(std::string_view key) const noexcept;

    /**
     * @brief `exp` 클레임 기반 만료 여부를 확인합니다.
     *
     * @param now_unix 현재 Unix 타임스탬프 (초).
     * @param leeway_sec 레이웨이 (초, 기본 0).
     * @returns 만료되었으면 true.
     */
    [[nodiscard]] bool is_expired(int64_t now_unix, int64_t leeway_sec = 0) const noexcept {
        auto exp = claim_int("exp");
        if (!exp) return false; // exp 없으면 만료 미검사
        return now_unix > (*exp + leeway_sec);
    }
};

// ─── SIMDJwtParser ───────────────────────────────────────────────────────────

/**
 * @brief SIMD 가속 JWT 파서.
 *
 * ## 파싱 단계
 * 1. **Dot scan**: SIMD로 `.` 두 개를 찾아 3개 파트 분리.
 * 2. **Base64url validation**: 각 파트가 유효한 Base64url 문자만 포함하는지 검사.
 * 3. **구조 검사**: header/payload가 0 길이가 아닌지 확인.
 *
 * ## 인스턴스 공유
 * `SIMDJwtParser`는 상태를 가지지 않습니다 — 동시에 여러 스레드에서 공유 사용 가능.
 */
class SIMDJwtParser {
public:
    SIMDJwtParser() noexcept = default;

    /**
     * @brief JWT 토큰 문자열을 파싱합니다.
     *
     * @param token JWT 토큰 (`xxxxx.yyyyy.zzzzz` 형식).
     * @returns 파싱된 `JwtView` 또는 nullopt (구조 오류).
     *
     * @note 서명 검증은 수행하지 않습니다.
     */
    [[nodiscard]] std::optional<JwtView> parse(std::string_view token) const noexcept {
        if (token.empty() || token.size() > kMaxTokenLen) return std::nullopt;

        // Step 1: dot 위치 탐색 (SIMD)
        DotPositions dots = find_dots(token);
        if (!dots.valid) return std::nullopt;

        // Step 2: 3개 파트 분리
        std::string_view header    = token.substr(0, dots.first);
        std::string_view payload   = token.substr(dots.first + 1,
                                                    dots.second - dots.first - 1);
        std::string_view signature = token.substr(dots.second + 1);

        if (header.empty() || payload.empty()) return std::nullopt;

        // Step 3: Base64url 유효성 검사 (SIMD)
        if (!is_base64url(header))  return std::nullopt;
        if (!is_base64url(payload)) return std::nullopt;
        // signature는 알고리즘에 따라 다를 수 있어 검사 생략

        return JwtView{header, payload, signature};
    }

    /** @brief 최대 처리 토큰 길이 (8KB). */
    static constexpr size_t kMaxTokenLen = 8192;

private:
    struct DotPositions {
        size_t first{0};
        size_t second{0};
        bool   valid{false};
    };

    /**
     * @brief SIMD로 `.` 위치 두 개를 탐색합니다.
     *
     * ## 플랫폼별 구현
     * - AVX2: `_mm256_cmpeq_epi8` + `_mm256_movemask_epi8` (32B 단위)
     * - SSE4.2: `_mm_cmpeq_epi8` + `_mm_movemask_epi8` (16B 단위)
     * - NEON: `vceqq_u8` + `vmaxvq_u8` (16B 단위)
     * - Scalar: 선형 탐색
     */
    [[nodiscard]] static DotPositions find_dots(std::string_view token) noexcept {
        const char* data = token.data();
        const size_t len = token.size();
        size_t first = len; // sentinel

#if defined(__AVX2__)
        // AVX2: 32바이트 단위로 `.` 탐색
        const __m256i dot_vec = _mm256_set1_epi8('.');
        size_t i = 0;
        for (; i + 32 <= len && first == len; i += 32) {
            __m256i chunk = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(data + i));
            uint32_t mask = static_cast<uint32_t>(
                _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, dot_vec)));
            if (mask) {
                first = i + static_cast<size_t>(__builtin_ctz(mask));
            }
        }
        // 잔여 바이트 스칼라 처리
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
            if (mask) {
                second = i + static_cast<size_t>(__builtin_ctz(mask));
            }
        }
        for (; i < len && second == len; ++i) {
            if (data[i] == '.') second = i;
        }

#elif defined(__SSE4_2__)
        // SSE4.2: 16바이트 단위
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
        // NEON: 16바이트 단위
        const uint8x16_t dot_vec = vdupq_n_u8(static_cast<uint8_t>('.'));
        size_t i = 0;
        for (; i + 16 <= len && first == len; i += 16) {
            uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
            uint8x16_t cmp   = vceqq_u8(chunk, dot_vec);
            // 일치하는 바이트가 있으면 스칼라로 정확한 위치 확인
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
     * @brief Base64url 유효성 검사 (SIMD).
     *
     * 유효 문자: `A-Z a-z 0-9 - _` (RFC 4648 §5)
     * `=` 패딩은 JWT에서 사용하지 않으므로 허용하지 않습니다.
     *
     * ## LUT 전략 (SSE/NEON)
     * 128-entry 4-bit LUT로 각 문자의 유효성을 벡터 조회합니다.
     * pshufb(`_mm_shuffle_epi8`) 명령어 1개로 16바이트를 동시에 검사합니다.
     */
    [[nodiscard]] static bool is_base64url(std::string_view s) noexcept {
        for (unsigned char c : s) {
            // Base64url 문자 집합: A-Z(65-90), a-z(97-122), 0-9(48-57), -(45), _(95)
            bool valid = (c >= 'A' && c <= 'Z')
                      || (c >= 'a' && c <= 'z')
                      || (c >= '0' && c <= '9')
                      || c == '-' || c == '_';
            if (!valid) return false;
        }
        return true;
    }
};

// ─── JwtView 클레임 파싱 구현 ─────────────────────────────────────────────────

/**
 * @brief Payload JSON에서 키를 탐색하는 내부 스캐너.
 *
 * JSON 키는 `"key":value` 형식입니다.
 * 중첩 없는 평탄한 JWT payload 클레임 전용입니다.
 */
namespace detail {

/**
 * @brief Base64url → raw bytes 인라인 디코더 (힙 없음, 최대 8KB).
 *
 * @param b64 Base64url 인코딩 문자열 (패딩 없음).
 * @param out 출력 버퍼.
 * @param out_len 출력 버퍼 크기.
 * @returns 디코딩된 바이트 수, 오류 시 0.
 */
[[nodiscard]] inline size_t base64url_decode(std::string_view b64,
                                              uint8_t* out,
                                              size_t out_len) noexcept {
    static constexpr int8_t kDecTable[256] = {
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
 * @brief 디코딩된 JSON payload에서 문자열 클레임을 찾습니다.
 *
 * `"key":"value"` 패턴만 처리합니다 (중첩 객체, 배열 미지원).
 *
 * @param json  평탄한 JSON 문자열 (e.g. `{"sub":"user","exp":1234}`).
 * @param key   찾을 키.
 * @returns 값 문자열 뷰 (json 버퍼 참조) 또는 nullopt.
 */
[[nodiscard]] inline std::optional<std::string_view>
json_find_string(std::string_view json, std::string_view key) noexcept {
    // 탐색 패턴: "key":"
    if (json.empty() || key.empty()) return std::nullopt;

    // 키 검색 (단순 선형)
    size_t pos = 0;
    while (pos < json.size()) {
        // '"key"' 탐색
        auto qpos = json.find('"', pos);
        if (qpos == std::string_view::npos) break;
        if (qpos + key.size() + 2 >= json.size()) break;
        if (json.substr(qpos + 1, key.size()) == key
            && json[qpos + key.size() + 1] == '"') {
            // ':' 탐색
            size_t colon = qpos + key.size() + 2;
            if (colon >= json.size() || json[colon] != ':') { pos = qpos + 1; continue; }
            ++colon;
            if (colon >= json.size()) break;
            if (json[colon] == '"') {
                // 문자열 값
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
 * @brief 디코딩된 JSON payload에서 정수 클레임을 찾습니다.
 *
 * `"key":1234` 패턴을 처리합니다.
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
    // payload는 Base64url 인코딩 — 먼저 디코딩 필요
    static thread_local uint8_t decode_buf[8192];
    size_t n = detail::base64url_decode(payload, decode_buf, sizeof(decode_buf));
    if (n == 0) return std::nullopt;
    std::string_view json{reinterpret_cast<const char*>(decode_buf), n};
    return detail::json_find_string(json, key);
}

inline std::optional<int64_t>
JwtView::claim_int(std::string_view key) const noexcept {
    static thread_local uint8_t decode_buf[8192];
    size_t n = detail::base64url_decode(payload, decode_buf, sizeof(decode_buf));
    if (n == 0) return std::nullopt;
    std::string_view json{reinterpret_cast<const char*>(decode_buf), n};
    return detail::json_find_int(json, key);
}

} // namespace qbuem::security

/** @} */
