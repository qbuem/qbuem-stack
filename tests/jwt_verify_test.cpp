/**
 * @file jwt_verify_test.cpp
 * @brief JWT HMAC-SHA256 sign + verify security tests.
 *
 * Security-critical question under test: does a tampered token get rejected?
 *
 * Coverage:
 * - crypto::verify_hmac_sha256 — accepts the correct tag, rejects a flipped
 *   message byte, rejects a flipped tag byte (constant-time MAC verification).
 * - A concrete HS256 ITokenVerifier (built on the library's own SIMDJwtParser +
 *   crypto::hmac_sha256 + crypto::base64url_encode):
 *     * ACCEPTS a token signed with the known secret (returns claims).
 *     * REJECTS a token whose signature byte was flipped.
 *     * REJECTS a token whose payload byte was flipped (signature no longer
 *       matches the signing input).
 *     * REJECTS an expired `exp` claim.
 *     * ACCEPTS a non-expired `exp` claim.
 *     * REJECTS a token verified with the wrong secret.
 *     * REJECTS structurally broken tokens (missing dot, empty).
 *
 * Style mirrors tests/security_v15_test.cpp (gtest TEST(...) macros).
 * All tests are synchronous and terminate immediately (no reactor/async).
 */

#include <qbuem/security/simd_jwt.hpp>
#include <qbuem/middleware/token_auth.hpp>
#include <qbuem/crypto.hpp>          // qbuem::constant_time_equal
#include <qbuem/crypto/hmac.hpp>     // hmac_sha256, verify_hmac_sha256
#include <qbuem/crypto/base64.hpp>   // base64url_encode
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using qbuem::security::SIMDJwtParser;
using qbuem::security::JwtView;
using qbuem::middleware::ITokenVerifier;
using qbuem::middleware::TokenClaims;

// ─────────────────────────────────────────────────────────────────────────────
// Test fixtures: a real HS256 signer + verifier built on qbuem-stack primitives.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Build a real HS256 JWT: base64url(header).base64url(payload).base64url(HMAC).
/// header_json / payload_json are raw JSON object strings.
std::string make_hs256_jwt(std::string_view header_json,
                           std::string_view payload_json,
                           std::string_view secret) {
    const std::string h = qbuem::crypto::base64url_encode(header_json);  // no padding
    const std::string p = qbuem::crypto::base64url_encode(payload_json);
    const std::string signing_input = h + "." + p;

    // HMAC-SHA256(secret, "header.payload")
    const auto tag = qbuem::crypto::hmac_sha256(secret, signing_input);  // 32-byte array
    const std::string sig = qbuem::crypto::base64url_encode(
        std::span<const uint8_t>{tag.data(), tag.size()});

    return signing_input + "." + sig;
}

/**
 * @brief Concrete HS256 verifier implementing ITokenVerifier.
 *
 * Mirrors the documented quick-start flow in token_auth.hpp:
 *   1. parse header.payload.sig (SIMDJwtParser)
 *   2. HMAC-SHA256(secret, header + "." + payload)
 *   3. constant-time compare computed-base64url == sig
 *   4. check exp (if present) against `now`
 *   5. extract sub/iss/aud/exp/nbf into TokenClaims
 * Returns nullopt on any failure.
 */
class HS256Verifier final : public ITokenVerifier {
public:
    HS256Verifier(std::string secret, int64_t now) noexcept
        : secret_(std::move(secret)), now_(now) {}

    std::optional<TokenClaims> verify(std::string_view token) noexcept override {
        SIMDJwtParser parser;
        auto view = parser.parse(token);
        if (!view) return std::nullopt;  // structural error

        // Recompute the expected signature over header.payload.
        const std::string_view signing_input = view->signing_input(token);
        if (signing_input.empty()) return std::nullopt;

        const auto computed_tag = qbuem::crypto::hmac_sha256(secret_, signing_input);
        const std::string expected_sig = qbuem::crypto::base64url_encode(
            std::span<const uint8_t>{computed_tag.data(), computed_tag.size()});

        // Constant-time signature comparison.
        if (!qbuem::constant_time_equal(expected_sig, view->signature))
            return std::nullopt;  // tampered or wrong-key signature

        // exp check (reject expired).
        if (view->is_expired(now_)) return std::nullopt;

        TokenClaims claims;
        if (auto sub = view->claim("sub")) claims.subject = std::string{*sub};
        if (auto iss = view->claim("iss")) claims.issuer = std::string{*iss};
        if (auto aud = view->claim("aud")) claims.audience = std::string{*aud};
        if (auto exp = view->claim_int("exp")) claims.exp = static_cast<long>(*exp);
        if (auto nbf = view->claim_int("nbf")) claims.nbf = static_cast<long>(*nbf);
        return claims;
    }

private:
    std::string secret_;
    int64_t     now_;
};

constexpr std::string_view kSecret = "super-secret-hmac-key";
constexpr std::string_view kHeader = R"({"alg":"HS256","typ":"JWT"})";
// Fixed "now" used by the verifier in expiry tests.
constexpr int64_t kNow = 1'700'000'000;  // 2023-11-14T22:13:20Z

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Crypto-level: verify_hmac_sha256 accepts correct, rejects tampered.
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacVerify, AcceptsCorrectTag) {
    const std::string_view msg = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJhbGljZSJ9";
    const auto tag = qbuem::crypto::hmac_sha256(kSecret, msg);
    EXPECT_TRUE(qbuem::crypto::verify_hmac_sha256(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(kSecret.data()), kSecret.size()},
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(msg.data()), msg.size()},
        tag));
}

TEST(HmacVerify, RejectsFlippedMessageByte) {
    std::string msg = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJhbGljZSJ9";
    const auto tag = qbuem::crypto::hmac_sha256(kSecret, msg);

    // Flip one byte of the message; the original tag must no longer verify.
    msg[5] = static_cast<char>(msg[5] ^ 0x01);
    EXPECT_FALSE(qbuem::crypto::verify_hmac_sha256(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(kSecret.data()), kSecret.size()},
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(msg.data()), msg.size()},
        tag));
}

TEST(HmacVerify, RejectsFlippedTagByte) {
    const std::string_view msg = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJhbGljZSJ9";
    auto tag = qbuem::crypto::hmac_sha256(kSecret, msg);

    // Flip one byte of the tag; verification must fail.
    tag[0] = static_cast<uint8_t>(tag[0] ^ 0x80);
    EXPECT_FALSE(qbuem::crypto::verify_hmac_sha256(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(kSecret.data()), kSecret.size()},
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(msg.data()), msg.size()},
        tag));
}

TEST(HmacVerify, RejectsWrongKey) {
    const std::string_view msg = "header.payload";
    const auto tag = qbuem::crypto::hmac_sha256(kSecret, msg);
    constexpr std::string_view wrong = "WRONG-key";
    EXPECT_FALSE(qbuem::crypto::verify_hmac_sha256(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(wrong.data()), wrong.size()},
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(msg.data()), msg.size()},
        tag));
}

// ─────────────────────────────────────────────────────────────────────────────
// ITokenVerifier (HS256): the security-critical sign+verify round-trip.
// ─────────────────────────────────────────────────────────────────────────────

TEST(Hs256Verifier, AcceptsCorrectlySignedToken) {
    // exp far in the future relative to kNow.
    const std::string payload =
        R"({"sub":"alice","iss":"qbuem","exp":2000000000})";
    const std::string token = make_hs256_jwt(kHeader, payload, kSecret);

    HS256Verifier verifier{std::string{kSecret}, kNow};
    auto claims = verifier.verify(token);

    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject, "alice");
    EXPECT_EQ(claims->issuer, "qbuem");
    EXPECT_EQ(claims->exp, 2000000000L);
}

TEST(Hs256Verifier, RejectsFlippedSignatureByte) {
    const std::string payload = R"({"sub":"alice","exp":2000000000})";
    std::string token = make_hs256_jwt(kHeader, payload, kSecret);

    // Flip the LAST character of the signature (third segment).
    char& last = token.back();
    // Pick a different but still valid base64url character to ensure parsing
    // still succeeds but the signature value changes.
    last = (last == 'A') ? 'B' : 'A';

    HS256Verifier verifier{std::string{kSecret}, kNow};
    auto claims = verifier.verify(token);

    // A tampered signature MUST be rejected.
    EXPECT_FALSE(claims.has_value());
}

TEST(Hs256Verifier, RejectsFlippedPayloadByte) {
    const std::string payload = R"({"sub":"alice","exp":2000000000})";
    std::string token = make_hs256_jwt(kHeader, payload, kSecret);

    // Locate the payload segment (between the two dots) and flip a byte there.
    const auto first_dot = token.find('.');
    ASSERT_NE(first_dot, std::string::npos);
    const auto second_dot = token.find('.', first_dot + 1);
    ASSERT_NE(second_dot, std::string::npos);
    ASSERT_GT(second_dot, first_dot + 1);

    // Flip a payload character to a different valid base64url character.
    const std::size_t mid = first_dot + 1 + (second_dot - first_dot - 1) / 2;
    char& c = token[mid];
    char flipped = (c == 'A') ? 'B' : 'A';
    c = flipped;

    HS256Verifier verifier{std::string{kSecret}, kNow};
    auto claims = verifier.verify(token);

    // Payload tampering invalidates the signature over header.payload.
    EXPECT_FALSE(claims.has_value());
}

TEST(Hs256Verifier, RejectsExpiredToken) {
    // exp is BEFORE kNow → expired.
    const std::string payload =
        R"({"sub":"alice","exp":1600000000})";  // 2020-09-13, < kNow
    const std::string token = make_hs256_jwt(kHeader, payload, kSecret);

    HS256Verifier verifier{std::string{kSecret}, kNow};
    auto claims = verifier.verify(token);

    EXPECT_FALSE(claims.has_value());
}

TEST(Hs256Verifier, AcceptsNonExpiredToken) {
    // exp is AFTER kNow → valid.
    const std::string payload =
        R"({"sub":"bob","exp":1900000000})";  // 2030, > kNow
    const std::string token = make_hs256_jwt(kHeader, payload, kSecret);

    HS256Verifier verifier{std::string{kSecret}, kNow};
    auto claims = verifier.verify(token);

    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject, "bob");
    EXPECT_EQ(claims->exp, 1900000000L);
}

TEST(Hs256Verifier, RejectsWrongSecret) {
    const std::string payload = R"({"sub":"alice","exp":2000000000})";
    const std::string token = make_hs256_jwt(kHeader, payload, kSecret);

    // Verifier configured with a different secret must reject the token.
    HS256Verifier verifier{std::string{"a-completely-different-secret"}, kNow};
    auto claims = verifier.verify(token);

    EXPECT_FALSE(claims.has_value());
}

TEST(Hs256Verifier, RejectsStructurallyBrokenToken) {
    HS256Verifier verifier{std::string{kSecret}, kNow};

    // Missing dots / empty → structural rejection, never accepted.
    EXPECT_FALSE(verifier.verify("").has_value());
    EXPECT_FALSE(verifier.verify("not-a-jwt").has_value());
    EXPECT_FALSE(verifier.verify("only.twoparts").has_value());
}

TEST(Hs256Verifier, RoundTripSignVerifyExtractsSub) {
    // End-to-end: sign then verify; the recovered subject must match the input.
    const std::string payload = R"({"sub":"device-7421","exp":2000000000})";
    const std::string token = make_hs256_jwt(kHeader, payload, kSecret);

    HS256Verifier verifier{std::string{kSecret}, kNow};
    auto claims = verifier.verify(token);

    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject, "device-7421");
}
