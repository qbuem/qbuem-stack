/**
 * @file crypto_url_example.cpp
 * @brief Crypto utilities + URL encoding/decoding example.
 *
 * ## Coverage — crypto.hpp
 * - constant_time_equal()   — timing-safe string comparison
 * - random_fill()           — CSPRNG random byte generation
 * - hw_entropy_fill()       — hardware entropy (RDRAND or fallback)
 * - hw_seed_fill()          — seeding entropy (RDSEED or fallback)
 * - generate_csrf_token()   — URL-safe Base64 CSRF token
 * - secure_token_hex()      — hex security token
 * - rdrand64() / rdseed64() — x86 RDRAND / RDSEED (if supported)
 * - has_rdrand() / has_rdseed() — CPU feature detection
 *
 * ## Coverage — url.hpp
 * - url_decode()            — percent-decoding (+ → space included)
 * - url_encode()            — percent-encoding
 */

#include <qbuem/crypto.hpp>
#include <qbuem/url.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

using namespace qbuem;

// ─────────────────────────────────────────────────────────────────────────────
// §1  URL encoding / decoding
// ─────────────────────────────────────────────────────────────────────────────

static void demo_url() {
    std::printf("-- §1  URL encoding/decoding --\n");

    // url_decode
    struct Case { const char* encoded; const char* expected; };
    std::vector<Case> decode_cases = {
        {"hello%20world%21",  "hello world!"},
        {"q=foo+bar",         "q=foo bar"},
        {"%EA%B0%80%EB%82%98", "\xEA\xB0\x80\xEB\x82\x98"},  // UTF-8 "가나"
        {"%2F%3F%23",         "/?#"},
        {"no-encoding",       "no-encoding"},
    };

    for (auto& c : decode_cases) {
        std::string result = url_decode(c.encoded);
        bool ok = (result == c.expected);
        std::printf("  decode(\"%s\") = %s %s\n",
                    c.encoded, result.c_str(), ok ? "OK" : "FAIL");
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

    // Round-trip: encode -> decode == original
    std::string original = "hello world /path?key=value&other=test#frag";
    std::string encoded  = url_encode(original);
    std::string decoded  = url_decode(encoded);
    std::printf("  round-trip encode->decode same: %s\n\n",
                (decoded == original) ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  CSPRNG — random_fill
// ─────────────────────────────────────────────────────────────────────────────

static std::string to_hex(const std::byte* data, size_t len) {
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
        result += std::format("{:02x}", static_cast<unsigned>(data[i]));
    return result;
}

static void demo_random() {
    std::printf("-- §2  CSPRNG random_fill --\n");

    // Generate 16 random bytes
    std::array<std::byte, 16> buf1{}, buf2{};
    random_fill(buf1.data(), buf1.size());
    random_fill(buf2.data(), buf2.size());

    std::printf("  random[0]: %s\n", to_hex(buf1.data(), 16).c_str());
    std::printf("  random[1]: %s\n", to_hex(buf2.data(), 16).c_str());

    // Two buffers are overwhelmingly likely to differ
    bool different = (std::memcmp(buf1.data(), buf2.data(), 16) != 0);
    std::printf("  two randoms differ: %s\n\n", different ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  Hardware entropy (RDRAND / RDSEED)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_hw_entropy() {
    std::printf("-- §3  Hardware entropy --\n");

    std::printf("  RDRAND supported: %s\n", has_rdrand() ? "yes" : "no (fallback used)");
    std::printf("  RDSEED supported: %s\n", has_rdseed() ? "yes" : "no (fallback used)");

    // hw_entropy_fill: RDRAND or getrandom fallback
    std::array<std::byte, 32> entropy{};
    hw_entropy_fill(entropy.data(), entropy.size());
    std::printf("  hw_entropy(32B): %s\n",
                to_hex(entropy.data(), 8).c_str()); // Print first 8 bytes only

    // hw_seed_fill: RDSEED or fallback
    std::array<std::byte, 16> seed{};
    hw_seed_fill(seed.data(), seed.size());
    std::printf("  hw_seed(16B):    %s\n\n",
                to_hex(seed.data(), 8).c_str());

    // Direct RDRAND call (if supported)
    if (has_rdrand()) {
        uint64_t r = 0;
        bool ok = rdrand64(r);
        std::printf("  rdrand64(): ok=%s value=%s\n\n",
                    ok ? "yes" : "no",
                    std::format("0x{:016x}", r).c_str());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  Security token generation
// ─────────────────────────────────────────────────────────────────────────────

static void demo_tokens() {
    std::printf("-- §4  Security token generation --\n");

    // CSRF token (URL-safe Base64)
    std::string csrf1 = csrf_token(256);
    std::string csrf2 = csrf_token(256);
    std::printf("  CSRF token 1: %s\n", csrf1.c_str());
    std::printf("  CSRF token 2: %s\n", csrf2.c_str());
    std::printf("  CSRF tokens differ: %s\n", (csrf1 != csrf2) ? "yes" : "no");

    // Hex token from random_bytes()
    auto to_hex_str = [](const std::string& raw) {
        std::string result;
        result.reserve(raw.size() * 2);
        for (unsigned char c : raw)
            result += std::format("{:02x}", static_cast<unsigned>(c));
        return result;
    };
    std::string hex32 = to_hex_str(random_bytes(16));  // 16 bytes -> 32 hex chars
    std::string hex64 = to_hex_str(random_bytes(32));  // 32 bytes -> 64 hex chars
    std::printf("  hex16: %s (%zu chars)\n", hex32.c_str(), hex32.size());
    std::printf("  hex32: %s... (%zu chars)\n",
                hex64.substr(0, 16).c_str(), hex64.size());
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  Timing-safe comparison
// ─────────────────────────────────────────────────────────────────────────────

static void demo_constant_time() {
    std::printf("-- §5  Timing-safe string comparison --\n");

    std::string token_a = "correct_token_secret_value";
    std::string token_b = "correct_token_secret_value";
    std::string token_c = "wrong_token_different_value";
    std::string token_d = "short";

    // constant_time_eq: prevents timing side-channel attacks
    std::printf("  a == b: %s\n",
                constant_time_equal(token_a, token_b) ? "true" : "false");
    std::printf("  a == c: %s\n",
                constant_time_equal(token_a, token_c) ? "true" : "false");
    std::printf("  a == d: %s (different lengths)\n",
                constant_time_equal(token_a, token_d) ? "true" : "false");
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem Crypto + URL Utilities Example ===\n\n");

    demo_url();
    demo_random();
    demo_hw_entropy();
    demo_tokens();
    demo_constant_time();

    std::printf("=== Done ===\n");
    return 0;
}
