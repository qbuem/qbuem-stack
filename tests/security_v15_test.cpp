/**
 * @file security_v15_test.cpp
 * @brief v1.5.0 security component unit tests.
 *
 * Coverage:
 * - SIMDJwtParser: structural parsing, error cases, claim extraction, expiry check
 * - Hardware Entropy: RDRAND/RDSEED availability query, hw_entropy_fill functionality
 * - crypto.hpp: constant_time_equal (existing), RDRAND extension
 * - kTLS sendfile: function declaration accessibility (including platform fallback)
 */

#include <qbuem/security/simd_jwt.hpp>
#include <qbuem/crypto.hpp>
#include <qbuem/io/ktls.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// SIMDJwtParser
// ─────────────────────────────────────────────────────────────────────────────

using namespace qbuem::security;

// RFC 7519 example token (no signature verification — structural parsing only)
// header: {"alg":"HS256","typ":"JWT"}
// payload: {"sub":"1234567890","name":"John Doe","iat":1516239022}
static constexpr std::string_view kSampleJwt =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
    "."
    "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ"
    "."
    "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";

TEST(SIMDJwtParser, ParseValidToken) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    EXPECT_FALSE(view->header.empty());
    EXPECT_FALSE(view->payload.empty());
    EXPECT_FALSE(view->signature.empty());
}

TEST(SIMDJwtParser, HeaderIsFirstPart) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->header, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9");
}

TEST(SIMDJwtParser, SignatureIsLastPart) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->signature, "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c");
}

TEST(SIMDJwtParser, SigningInputPointsIntoOriginal) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    auto si = view->signing_input(kSampleJwt);
    // header.payload (including '.')
    size_t expected_len = view->header.size() + 1 + view->payload.size();
    EXPECT_EQ(si.size(), expected_len);
    EXPECT_EQ(si.data(), kSampleJwt.data()); // zero-copy: reference to original
}

TEST(SIMDJwtParser, ParseEmptyReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse("").has_value());
}

TEST(SIMDJwtParser, ParseNoDotReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse("nodotatall").has_value());
}

TEST(SIMDJwtParser, ParseOneDotReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse("header.payload").has_value());
}

TEST(SIMDJwtParser, ParseInvalidBase64UrlHeaderReturnsNullopt) {
    SIMDJwtParser parser;
    // '+' is an invalid character in Base64url
    EXPECT_FALSE(parser.parse("inv+lid.payload.sig").has_value());
}

TEST(SIMDJwtParser, ParseEmptyHeaderReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse(".payload.sig").has_value());
}

TEST(SIMDJwtParser, ParseEmptyPayloadReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse("header..sig").has_value());
}

TEST(SIMDJwtParser, ClaimIntIat) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    auto iat = view->claim_int("iat");
    ASSERT_TRUE(iat.has_value());
    EXPECT_EQ(*iat, 1516239022LL);
}

TEST(SIMDJwtParser, ClaimStringSub) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    auto sub = view->claim("sub");
    ASSERT_TRUE(sub.has_value());
    EXPECT_EQ(*sub, "1234567890");
}

TEST(SIMDJwtParser, ClaimMissingReturnsNullopt) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    EXPECT_FALSE(view->claim("exp").has_value());
}

TEST(SIMDJwtParser, IsExpiredNoExpClaim) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    // no exp claim → not considered expired
    EXPECT_FALSE(view->is_expired(9999999999LL));
}

// ─────────────────────────────────────────────────────────────────────────────
// Hardware Entropy
// ─────────────────────────────────────────────────────────────────────────────

TEST(HardwareEntropy, HasRdrandReturnsBool) {
    // Only verifies return type and callability (regardless of CPU type)
    bool r = qbuem::has_rdrand();
    (void)r; // value differs by hardware
    SUCCEED();
}

TEST(HardwareEntropy, HasRdseedReturnsBool) {
    bool r = qbuem::has_rdseed();
    (void)r;
    SUCCEED();
}

TEST(HardwareEntropy, HwEntropyFillProducesNonZeroBuffer) {
    // Fill 32 bytes of entropy and verify not all zeros
    std::array<uint8_t, 32> buf{};
    EXPECT_NO_THROW(qbuem::hw_entropy_fill(buf.data(), buf.size()));

    // Probability of all zeros is extremely low (basic cryptographic randomness check)
    bool all_zero = true;
    for (auto b : buf) if (b != 0) { all_zero = false; break; }
    EXPECT_FALSE(all_zero);
}

TEST(HardwareEntropy, HwEntropyFill1Byte) {
    uint8_t byte = 0xAA;
    EXPECT_NO_THROW(qbuem::hw_entropy_fill(&byte, 1));
    // Only verify that 1-byte injection succeeds
    SUCCEED();
}

TEST(HardwareEntropy, HwEntropyFill64Bytes) {
    std::array<uint8_t, 64> buf{};
    EXPECT_NO_THROW(qbuem::hw_entropy_fill(buf.data(), buf.size()));
    SUCCEED();
}

TEST(HardwareEntropy, RdrandConsecutiveDiffer) {
    if (!qbuem::has_rdrand()) GTEST_SKIP() << "RDRAND not supported";
    uint64_t a = 0, b = 0;
    EXPECT_TRUE(qbuem::rdrand64(a));
    EXPECT_TRUE(qbuem::rdrand64(b));
    // Probability of two consecutive values being equal is 2^-64 — effectively zero
    EXPECT_NE(a, b);
}

TEST(HardwareEntropy, HwSeedFillProducesNonZero) {
    std::array<uint8_t, 32> buf{};
    EXPECT_NO_THROW(qbuem::hw_seed_fill(buf.data(), buf.size()));
    bool all_zero = true;
    for (auto b : buf) if (b != 0) { all_zero = false; break; }
    EXPECT_FALSE(all_zero);
}

// ─────────────────────────────────────────────────────────────────────────────
// kTLS sendfile API accessibility
// ─────────────────────────────────────────────────────────────────────────────

TEST(KtlsSendfile, FunctionAccessible) {
    // Only verifies function existence without actual fds (compilation test)
    // On non-Linux: returns errc::not_supported
    off_t off = 0;
    auto r = qbuem::io::ktls_sendfile(-1, -1, off, 0);
    // Linux: invalid fd → EBADF
    // non-Linux: not_supported
    EXPECT_FALSE(r.has_value()); // error in either case
}

TEST(KtlsSendfile, AllVariantAccessible) {
    auto r = qbuem::io::ktls_sendfile_all(-1, -1);
    EXPECT_FALSE(r.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// constant_time_equal (existing — regression test)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConstantTimeEqual, EqualStrings) {
    EXPECT_TRUE(qbuem::constant_time_equal("hello", "hello"));
}

TEST(ConstantTimeEqual, DifferentStrings) {
    EXPECT_FALSE(qbuem::constant_time_equal("hello", "world"));
}

TEST(ConstantTimeEqual, DifferentLengths) {
    EXPECT_FALSE(qbuem::constant_time_equal("short", "longer_string"));
}

TEST(ConstantTimeEqual, EmptyStrings) {
    EXPECT_TRUE(qbuem::constant_time_equal("", ""));
}
