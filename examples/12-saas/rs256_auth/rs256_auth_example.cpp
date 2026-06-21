/**
 * @file examples/12-saas/rs256_auth/rs256_auth_example.cpp
 * @brief How to verify external-IdP (Auth0/Cognito/Firebase/Okta) RS256 JWTs.
 *
 * Demonstrates the SaaS auth path end-to-end, in-memory (no network):
 *   1. Load the IdP's public keys from a JWKS document (`parse_jwks`).
 *   2. Build an `Rs256JwtVerifier` and pin issuer/audience (RFC 8725).
 *   3. Verify a valid token → extract claims.
 *   4. Reject: wrong audience, tampered token, and an `alg:none` downgrade.
 *
 * In a real service you attach it to the App with one line:
 *   auto v = std::make_shared<Rs256JwtVerifier>(Rs256JwtVerifier::from_jwks(jwks));
 *   v->expect_issuer("https://idp.example.com").expect_audience("my-api");
 *   app.use(qbuem::middleware::bearer_auth(v));   // bearer_auth is a sync Middleware
 *
 * The JWKS + JWT below were minted offline with OpenSSL (RSA-2048); the private
 * key was discarded. qbuem verifies them with its own zero-dependency RS256.
 */

#include <qbuem/qbuem_stack.hpp> // umbrella now exposes the middleware/crypto SaaS surface

#include <print>
#include <string_view>

using qbuem::middleware::Rs256JwtVerifier;

namespace {
constexpr std::string_view kJwks =
    "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"test-key-1\",\"alg\":\"RS256\",\"use\":\"sig\","
    "\"n\":\"vPHlEZq3UPoPKBTaoH0sC5kEYfi8eMvUWHT5ONWj2qSLVBV7K7EtS1T-tR3IbNGghSMtUOh1nMyFv38p8PsNn52zyGAhVM7D7l5hBDvWRfDcmI-dwyPoVp7DOc6mxT-R4Ot-yS5eMl3DRsZ9sxhfpPZpNupK2J8YKst_EUKp8AmxbOjj6Z2e-mT0iK86pTTBPJi1OCha6Ged7Dzvvz7DWceNnaefkH78vSr-i_hP2EBbbnKt780uDGjJLTCsMIYkCEl5vSp8eP-kZWFcY9i_P3W1PFlUs2QYiMsv_qImIn06hIOAlZFohVc-KYaKSii7UG3OIWZ5nSiHVVcHFiQkww\","
    "\"e\":\"AQAB\"}]}";

// {"sub":"user-42","iss":"https://idp.example.com","aud":"qbuem","exp":4102444800}
constexpr std::string_view kJwt =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6InRlc3Qta2V5LTEifQ."
    "eyJzdWIiOiJ1c2VyLTQyIiwiaXNzIjoiaHR0cHM6Ly9pZHAuZXhhbXBsZS5jb20iLCJhdWQiOiJxYnVlbSIsImV4cCI6NDEwMjQ0NDgwMH0."
    "MKU0bwlJwIx8sqWD0qM0T4JJRhjpiew8Vcj282NDkiv91D68QJ44dlJ4J80LIXhfwkunZdLpZ1ILWDlHPVLhGxbagHb0SERTpzf7sqFaDQC3ATPyIK9mvr11CqkWUbRnn0_uTIx1txzB13M9p8AwpLlSe95YdIpdBNzMToMGs-AH549hZTOZHqfNmAsNh2qKUhzm_gJBy-50xEsgFqioZwjQ-DRLRSI4AbCUEUAR2T-kUEct5qkyJ7cAipJwwluE995lTVairPoVHZwjl_wDQ5maaigGv0PlRwfnAhew7pcGnLf7RnECteuChcSNDPF6EserxYsOeMfCY906eoBTDQ";

int passed = 0, failed = 0;
void check(std::string_view what, bool ok) {
  std::println("  [{}] {}", ok ? "PASS" : "FAIL", what);
  ok ? ++passed : ++failed;
}
} // namespace

int main() {
  std::println("=== RS256 / JWKS auth (external IdP tokens) ===\n");

  // 1. Build the verifier from the IdP's JWKS, pinning issuer + audience.
  auto verifier = Rs256JwtVerifier::from_jwks(kJwks);
  verifier.expect_issuer("https://idp.example.com").expect_audience("qbuem");
  std::println("Loaded {} RSA key(s) from JWKS.\n", verifier.key_count());

  // 2. Verify a valid token.
  if (auto claims = verifier.verify(kJwt)) {
    std::println("Valid token. Claims:");
    std::println("  sub = {}", claims->subject);
    std::println("  iss = {}", claims->issuer);
    std::println("  aud = {}", claims->audience);
    std::println("  exp = {}\n", claims->exp);
    check("valid token accepted", true);
    check("subject extracted", claims->subject == "user-42");
  } else {
    check("valid token accepted", false);
  }

  // 3. Reject wrong audience (RFC 8725 §3.2).
  {
    auto v = Rs256JwtVerifier::from_jwks(kJwks);
    v.expect_audience("some-other-api");
    check("wrong audience rejected", !v.verify(kJwt).has_value());
  }

  // 4. Reject a tampered token.
  {
    std::string t(kJwt);
    t[t.find('.') + 5] ^= 0x01; // flip a payload byte
    check("tampered token rejected", !verifier.verify(t).has_value());
  }

  // 5. Reject an alg:none downgrade attack.
  {
    const std::string none_hdr =
        qbuem::crypto::base64url_encode(std::string_view("{\"alg\":\"none\"}"));
    check("alg:none downgrade rejected",
          !verifier.verify(none_hdr + ".eyJzdWIiOiJ4In0.").has_value());
  }

  std::println("\n{} passed, {} failed", passed, failed);
  return failed == 0 ? 0 : 1;
}
