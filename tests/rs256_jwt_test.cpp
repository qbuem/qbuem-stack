/**
 * @file tests/rs256_jwt_test.cpp
 * @brief End-to-end tests for Rs256JwtVerifier (RS256 JWT + JWKS auth).
 *
 * The JWT, JWKS and expired-JWT below were minted offline with the OpenSSL CLI
 * (RSA-2048; `openssl dgst -sha256 -sign`) — the private key was discarded, and
 * the token/JWKS are public by definition. The test is pure C++ (qbuem's own
 * zero-dependency RS256 verifier); OpenSSL is not used at build/run time.
 */

#include <qbuem/crypto/base64.hpp>
#include <qbuem/middleware/rs256_verifier.hpp>

#include <gtest/gtest.h>
#include <string>
#include <string_view>

using qbuem::middleware::Rs256JwtVerifier;
using qbuem::middleware::parse_jwks;

namespace {

// Valid RS256 JWT: {"sub":"user-42","iss":"https://idp.example.com",
//                   "aud":"qbuem","exp":4102444800}  (exp = 2100-01-01)
constexpr std::string_view kJwt =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6InRlc3Qta2V5LTEifQ."
    "eyJzdWIiOiJ1c2VyLTQyIiwiaXNzIjoiaHR0cHM6Ly9pZHAuZXhhbXBsZS5jb20iLCJhdWQiOiJxYnVlbSIsImV4cCI6NDEwMjQ0NDgwMH0."
    "MKU0bwlJwIx8sqWD0qM0T4JJRhjpiew8Vcj282NDkiv91D68QJ44dlJ4J80LIXhfwkunZdLpZ1ILWDlHPVLhGxbagHb0SERTpzf7sqFaDQC3ATPyIK9mvr11CqkWUbRnn0_uTIx1txzB13M9p8AwpLlSe95YdIpdBNzMToMGs-AH549hZTOZHqfNmAsNh2qKUhzm_gJBy-50xEsgFqioZwjQ-DRLRSI4AbCUEUAR2T-kUEct5qkyJ7cAipJwwluE995lTVairPoVHZwjl_wDQ5maaigGv0PlRwfnAhew7pcGnLf7RnECteuChcSNDPF6EserxYsOeMfCY906eoBTDQ";

// JWKS containing the matching RSA public key (kid test-key-1).
constexpr std::string_view kJwks =
    "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"test-key-1\",\"alg\":\"RS256\",\"use\":\"sig\","
    "\"n\":\"vPHlEZq3UPoPKBTaoH0sC5kEYfi8eMvUWHT5ONWj2qSLVBV7K7EtS1T-tR3IbNGghSMtUOh1nMyFv38p8PsNn52zyGAhVM7D7l5hBDvWRfDcmI-dwyPoVp7DOc6mxT-R4Ot-yS5eMl3DRsZ9sxhfpPZpNupK2J8YKst_EUKp8AmxbOjj6Z2e-mT0iK86pTTBPJi1OCha6Ged7Dzvvz7DWceNnaefkH78vSr-i_hP2EBbbnKt780uDGjJLTCsMIYkCEl5vSp8eP-kZWFcY9i_P3W1PFlUs2QYiMsv_qImIn06hIOAlZFohVc-KYaKSii7UG3OIWZ5nSiHVVcHFiQkww\","
    "\"e\":\"AQAB\"}]}";

// Same key, but exp = 1000000000 (2001-09-09) → expired.
constexpr std::string_view kJwtExpired =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6InRlc3Qta2V5LTEifQ."
    "eyJzdWIiOiJ1c2VyLTQyIiwiZXhwIjoxMDAwMDAwMDAwfQ."
    "DGHG-2o28J8NHn22rI_-YtS_1d1W-ZsiOTc4cNynb2ZYwn9wR9V6lf7AEtOGCVRQFJKjudaI0kvk04yehBStHuGtcLX6sM_oeaBkvY7gKOwD8Bb0J0fhn8K9asaDsSkEpUp45Zls4RebL0ZJrFijiAIo-9pFXkJzcDOXD0r2_uU2DfjDa4fg_hd9Ug6wCMFEu2dhIhmS4mumDYBqiZuWLx9SV4OSWtfSjL8VtN24-mClEKgueT9JL0Z_c3Nu18SeaX8zNVwNl4I0_-flg5Tk5jRDjtAtQyFqX9jvM-SqJWEZoDi_ofoxdFkyoSQlZtIOeTfTRMQxjEL-8BKBf8ztTg";

} // namespace

TEST(Rs256Jwt, ParsesJwks) {
  auto keys = parse_jwks(kJwks);
  ASSERT_EQ(keys.size(), 1u);
  EXPECT_EQ(keys[0].kid, "test-key-1");
  EXPECT_EQ(keys[0].n.size(), 256u); // RSA-2048 modulus
  EXPECT_EQ(keys[0].e.size(), 3u);   // 65537 = 01 00 01
}

TEST(Rs256Jwt, ValidTokenVerifies) {
  auto v = Rs256JwtVerifier::from_jwks(kJwks);
  auto claims = v.verify(kJwt);
  ASSERT_TRUE(claims.has_value());
  EXPECT_EQ(claims->subject, "user-42");
  EXPECT_EQ(claims->issuer, "https://idp.example.com");
  EXPECT_EQ(claims->audience, "qbuem");
  EXPECT_EQ(claims->exp, 4102444800L);
}

TEST(Rs256Jwt, TamperedPayloadRejected) {
  auto v = Rs256JwtVerifier::from_jwks(kJwks);
  std::string t(kJwt);
  // Flip a character inside the payload segment (between the two dots).
  const auto d1 = t.find('.');
  t[d1 + 5] = (t[d1 + 5] == 'A') ? 'B' : 'A';
  EXPECT_FALSE(v.verify(t).has_value());
}

TEST(Rs256Jwt, ExpiredTokenRejected) {
  auto v = Rs256JwtVerifier::from_jwks(kJwks);
  EXPECT_FALSE(v.verify(kJwtExpired).has_value());
}

TEST(Rs256Jwt, AlgNoneRejected) {
  auto v = Rs256JwtVerifier::from_jwks(kJwks);
  const std::string hdr = qbuem::crypto::base64url_encode(
      std::string_view("{\"alg\":\"none\",\"kid\":\"test-key-1\"}"));
  const std::string token = hdr + ".AAAA.";
  EXPECT_FALSE(v.verify(token).has_value()); // downgrade attack rejected
}

TEST(Rs256Jwt, AlgHs256Rejected) {
  auto v = Rs256JwtVerifier::from_jwks(kJwks);
  const std::string hdr = qbuem::crypto::base64url_encode(
      std::string_view("{\"alg\":\"HS256\",\"kid\":\"test-key-1\"}"));
  const std::string token = hdr + ".AAAA.BBBB";
  EXPECT_FALSE(v.verify(token).has_value()); // alg confusion rejected
}

TEST(Rs256Jwt, WrongKeyRejected) {
  // A verifier holding a different (bogus) key must reject the real token.
  std::vector<uint8_t> bogus_n(256, 0xAB);
  std::vector<uint8_t> e = {0x01, 0x00, 0x01};
  Rs256JwtVerifier v(std::move(bogus_n), std::move(e));
  EXPECT_FALSE(v.verify(kJwt).has_value());
}

TEST(Rs256Jwt, MalformedTokensRejected) {
  auto v = Rs256JwtVerifier::from_jwks(kJwks);
  EXPECT_FALSE(v.verify("not-a-jwt").has_value());
  EXPECT_FALSE(v.verify("only.two").has_value());
  EXPECT_FALSE(v.verify("").has_value());
}

TEST(Rs256Jwt, IssuerValidated) {
  auto good = Rs256JwtVerifier::from_jwks(kJwks);
  good.expect_issuer("https://idp.example.com");
  EXPECT_TRUE(good.verify(kJwt).has_value()); // iss matches

  auto bad = Rs256JwtVerifier::from_jwks(kJwks);
  bad.expect_issuer("https://evil.example.com");
  EXPECT_FALSE(bad.verify(kJwt).has_value()); // iss mismatch rejected (RFC 8725)
}

TEST(Rs256Jwt, AudienceValidated) {
  auto good = Rs256JwtVerifier::from_jwks(kJwks);
  good.expect_audience("qbuem");
  EXPECT_TRUE(good.verify(kJwt).has_value()); // aud matches

  auto bad = Rs256JwtVerifier::from_jwks(kJwks);
  bad.expect_audience("other-api");
  EXPECT_FALSE(bad.verify(kJwt).has_value()); // aud mismatch rejected (RFC 8725)
}
