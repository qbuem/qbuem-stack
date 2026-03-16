/**
 * @file crypto_url_example.cpp
 * @brief 암호화 유틸리티 + URL 인코딩/디코딩 예제.
 *
 * ## 커버리지 — crypto.hpp
 * - constant_time_equal()         — 타이밍 안전 문자열 비교
 * - random_fill()              — CSPRNG 랜덤 바이트 생성
 * - hw_entropy_fill()          — 하드웨어 엔트로피 (RDRAND 또는 폴백)
 * - hw_seed_fill()             — 시딩용 엔트로피 (RDSEED 또는 폴백)
 * - generate_csrf_token()      — URL-safe Base64 CSRF 토큰
 * - secure_token_hex()         — 16진수 보안 토큰
 * - rdrand64() / rdseed64()    — x86 RDRAND / RDSEED (지원 시)
 * - has_rdrand() / has_rdseed()— CPU 기능 감지
 *
 * ## 커버리지 — url.hpp
 * - url_decode()               — percent-decoding (+ → 공백 포함)
 * - url_encode()               — percent-encoding
 */

#include <qbuem/crypto.hpp>
#include <qbuem/url.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace qbuem;

// ─────────────────────────────────────────────────────────────────────────────
// §1  URL 인코딩 / 디코딩
// ─────────────────────────────────────────────────────────────────────────────

static void demo_url() {
    std::printf("── §1  URL 인코딩/디코딩 ──\n");

    // url_decode
    struct Case { const char* encoded; const char* expected; };
    std::vector<Case> decode_cases = {
        {"hello%20world%21",  "hello world!"},
        {"q=foo+bar",         "q=foo bar"},
        {"%EA%B0%80%EB%82%98", "\xEA\xB0\x80\xEB\x82\x98"},  // 한글 "가나"
        {"%2F%3F%23",         "/?#"},
        {"no-encoding",       "no-encoding"},
    };

    for (auto& c : decode_cases) {
        std::string result = url_decode(c.encoded);
        bool ok = (result == c.expected);
        std::printf("  decode(\"%s\") = %s %s\n",
                    c.encoded, result.c_str(), ok ? "✓" : "✗");
    }
    std::printf("\n");

    // url_encode
    struct EncCase { const char* raw; const char* expected; };
    std::vector<EncCase> encode_cases = {
        {"hello world!",  "hello%20world%21"},
        {"a=b&c=d",       "a%3Db%26c%3Dd"},
        {"safe-._~",      "safe-._~"},
        {"/path?q=v#frag","/%3Fq%3Dv%23frag"},
    };

    for (auto& c : encode_cases) {
        std::string result = url_encode(c.raw);
        std::printf("  encode(\"%s\") = %s\n", c.raw, result.c_str());
    }
    std::printf("\n");

    // 왕복 검증: encode → decode == original
    std::string original = "hello world /path?key=value&other=한글#frag";
    std::string encoded  = url_encode(original);
    std::string decoded  = url_decode(encoded);
    std::printf("  왕복: encode→decode 동일: %s\n\n",
                (decoded == original) ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  CSPRNG — random_fill
// ─────────────────────────────────────────────────────────────────────────────

static std::string to_hex(const std::byte* data, size_t len) {
    std::ostringstream ss;
    for (size_t i = 0; i < len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<unsigned>(data[i]);
    return ss.str();
}

static void demo_random() {
    std::printf("── §2  CSPRNG random_fill ──\n");

    // 16바이트 랜덤 생성
    std::array<std::byte, 16> buf1{}, buf2{};
    random_fill(buf1.data(), buf1.size());
    random_fill(buf2.data(), buf2.size());

    std::printf("  random[0]: %s\n", to_hex(buf1.data(), 16).c_str());
    std::printf("  random[1]: %s\n", to_hex(buf2.data(), 16).c_str());

    // 두 버퍼가 다를 확률은 압도적으로 높음
    bool different = (std::memcmp(buf1.data(), buf2.data(), 16) != 0);
    std::printf("  두 난수 다름: %s\n\n", different ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  하드웨어 엔트로피 (RDRAND / RDSEED)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_hw_entropy() {
    std::printf("── §3  하드웨어 엔트로피 ──\n");

    std::printf("  RDRAND 지원: %s\n", has_rdrand() ? "yes" : "no (폴백 사용)");
    std::printf("  RDSEED 지원: %s\n", has_rdseed() ? "yes" : "no (폴백 사용)");

    // hw_entropy_fill: RDRAND 또는 getrandom 폴백
    std::array<std::byte, 32> entropy{};
    hw_entropy_fill(entropy.data(), entropy.size());
    std::printf("  hw_entropy(32B): %s\n",
                to_hex(entropy.data(), 8).c_str()); // 앞 8바이트만 출력

    // hw_seed_fill: RDSEED 또는 폴백
    std::array<std::byte, 16> seed{};
    hw_seed_fill(seed.data(), seed.size());
    std::printf("  hw_seed(16B):    %s\n\n",
                to_hex(seed.data(), 8).c_str());

    // RDRAND 직접 호출 (지원 시)
    if (has_rdrand()) {
        uint64_t r = 0;
        bool ok = rdrand64(r);
        std::printf("  rdrand64(): ok=%s value=0x%016llx\n\n",
                    ok ? "yes" : "no",
                    static_cast<unsigned long long>(r));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  보안 토큰 생성
// ─────────────────────────────────────────────────────────────────────────────

static void demo_tokens() {
    std::printf("── §4  보안 토큰 생성 ──\n");

    // CSRF 토큰 (URL-safe Base64)
    std::string csrf1 = csrf_token(256);
    std::string csrf2 = csrf_token(256);
    std::printf("  CSRF 토큰 1: %s\n", csrf1.c_str());
    std::printf("  CSRF 토큰 2: %s\n", csrf2.c_str());
    std::printf("  CSRF 토큰 다름: %s\n", (csrf1 != csrf2) ? "yes" : "no");

    // random_bytes()로 16진수 토큰 생성
    auto to_hex = [](const std::string& raw) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (unsigned char c : raw) oss << std::setw(2) << (int)c;
        return oss.str();
    };
    std::string hex32 = to_hex(random_bytes(16));  // 16바이트 → 32자 hex
    std::string hex64 = to_hex(random_bytes(32));  // 32바이트 → 64자 hex
    std::printf("  hex16: %s (%zu자)\n", hex32.c_str(), hex32.size());
    std::printf("  hex32: %s... (%zu자)\n",
                hex64.substr(0, 16).c_str(), hex64.size());
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  타이밍 안전 비교
// ─────────────────────────────────────────────────────────────────────────────

static void demo_constant_time() {
    std::printf("── §5  타이밍 안전 문자열 비교 ──\n");

    std::string token_a = "correct_token_secret_value";
    std::string token_b = "correct_token_secret_value";
    std::string token_c = "wrong_token_different_value";
    std::string token_d = "short";

    // constant_time_eq: 타이밍 사이드채널 공격 방지
    std::printf("  a == b: %s\n",
                constant_time_equal(token_a, token_b) ? "true" : "false");
    std::printf("  a == c: %s\n",
                constant_time_equal(token_a, token_c) ? "true" : "false");
    std::printf("  a == d: %s (길이 다름)\n",
                constant_time_equal(token_a, token_d) ? "true" : "false");
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem Crypto + URL 유틸리티 예제 ===\n\n");

    demo_url();
    demo_random();
    demo_hw_entropy();
    demo_tokens();
    demo_constant_time();

    std::printf("=== 완료 ===\n");
    return 0;
}
