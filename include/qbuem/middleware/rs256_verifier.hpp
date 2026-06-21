#pragma once

/**
 * @file qbuem/middleware/rs256_verifier.hpp
 * @brief RS256 (RSASSA-PKCS1-v1.5 + SHA-256) JWT `ITokenVerifier` with JWKS —
 *        drop-in auth for tokens issued by an external IdP (Auth0, Cognito,
 *        Firebase, Okta, …).
 * @ingroup qbuem_middleware
 *
 * Bridges the zero-dependency `crypto::rsa_pkcs1_v15_sha256_verify` to the
 * `ITokenVerifier` port. The signature is checked against the IdP's RSA public
 * key (selected by the token's `kid` from a JWKS key set); `alg` is strictly
 * pinned to `RS256` (rejecting `none`/`HS256` downgrade attacks), and `exp`/`nbf`
 * are enforced. No third-party dependency — an OpenSSL/mbedTLS adapter could
 * replace the RSA step behind this same port without changing callers.
 *
 * @code
 * auto v = std::make_shared<qbuem::middleware::Rs256JwtVerifier>(
 *     qbuem::middleware::Rs256JwtVerifier::from_jwks(jwks_json));
 * v->expect_issuer("https://idp.example.com").expect_audience("my-api");
 * app.use(qbuem::middleware::bearer_auth(v));   // bearer_auth is a sync Middleware
 * @endcode
 *
 * JWKS *fetching* over HTTPS is intentionally out of scope here (the core has no
 * TLS — see docs/saas-readiness.md, edge-terminated TLS). Supply the JWKS
 * document out-of-band / from config; an opt-in HTTPS fetch adapter can come later.
 */

#include <qbuem/crypto/base64.hpp>
#include <qbuem/crypto/rsa.hpp>
#include <qbuem/middleware/jwt_verifier.hpp> // reuse detail::jwtclaims + ITokenVerifier
#include <qbuem/middleware/token_auth.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qbuem::middleware {

/** @brief One RSA public key from a JWK / JWKS document. */
struct JwkRsaKey {
  std::string          kid; ///< Key ID ("" if the JWK had none).
  std::vector<uint8_t> n;   ///< Modulus, big-endian (decoded from the JWK `n`).
  std::vector<uint8_t> e;   ///< Public exponent, big-endian (decoded from `e`).
};

/**
 * @brief Parse a JWKS document (or a single bare JWK) into its RSA public keys.
 *
 * Non-RSA entries and malformed keys are skipped. `n`/`e` are base64url-decoded
 * to big-endian bytes. The input is the IdP's trusted key endpoint, so a minimal
 * scan of the flat JWK fields is sufficient (base64url values contain no braces,
 * so object boundaries are found by brace counting).
 */
[[nodiscard]] inline std::vector<JwkRsaKey> parse_jwks(std::string_view json) {
  std::vector<JwkRsaKey> out;

  auto add_object = [&](std::string_view obj) {
    if (detail::jwtclaims::find_string(obj, "kty") != "RSA") return;
    const std::string n_b64 = detail::jwtclaims::find_string(obj, "n");
    const std::string e_b64 = detail::jwtclaims::find_string(obj, "e");
    if (n_b64.empty() || e_b64.empty()) return;
    auto n = crypto::base64url_decode(n_b64);
    auto e = crypto::base64url_decode(e_b64);
    if (!n || !e) return;
    JwkRsaKey k;
    k.kid = detail::jwtclaims::find_string(obj, "kid");
    k.n.assign(n->begin(), n->end());
    k.e.assign(e->begin(), e->end());
    out.push_back(std::move(k));
  };

  const auto keys_pos = json.find("\"keys\"");
  if (keys_pos == std::string_view::npos) {
    add_object(json); // a single bare JWK
    return out;
  }
  const auto lb = json.find('[', keys_pos);
  if (lb == std::string_view::npos) return out;

  int    depth = 0;
  size_t obj_start = std::string_view::npos;
  for (size_t i = lb + 1; i < json.size(); ++i) {
    const char c = json[i];
    if (c == ']' && depth == 0) break;
    if (c == '{') {
      if (depth == 0) obj_start = i;
      ++depth;
    } else if (c == '}') {
      if (depth > 0) --depth;
      if (depth == 0 && obj_start != std::string_view::npos) {
        add_object(json.substr(obj_start, i - obj_start + 1));
        obj_start = std::string_view::npos;
      }
    }
  }
  return out;
}

/**
 * @brief ITokenVerifier validating RS256 JWTs against a JWKS RSA key set.
 *
 * Rejects: malformed token, `alg != RS256` (incl. `none`/`HS256`), unknown `kid`,
 * bad signature, expired (`exp`), not-yet-valid (`nbf`). Extracts
 * `sub`/`iss`/`aud`/`exp`/`nbf`.
 */
class Rs256JwtVerifier : public ITokenVerifier {
public:
  /** @brief From a parsed JWKS key set. */
  explicit Rs256JwtVerifier(std::vector<JwkRsaKey> keys, long leeway_seconds = 0)
      : keys_(std::move(keys)), leeway_(leeway_seconds) {}

  /** @brief From a single raw RSA public key (modulus + exponent, big-endian). */
  Rs256JwtVerifier(std::vector<uint8_t> modulus, std::vector<uint8_t> exponent,
                   long leeway_seconds = 0)
      : leeway_(leeway_seconds) {
    keys_.push_back(JwkRsaKey{"", std::move(modulus), std::move(exponent)});
  }

  /** @brief Build from a JWKS JSON document. */
  [[nodiscard]] static Rs256JwtVerifier from_jwks(std::string_view jwks_json,
                                                  long leeway_seconds = 0) {
    return Rs256JwtVerifier(parse_jwks(jwks_json), leeway_seconds);
  }

  /** @brief Number of RSA keys loaded. */
  [[nodiscard]] size_t key_count() const noexcept { return keys_.size(); }

  /**
   * @brief Require the token's `iss` to equal @p issuer (RFC 8725 §3.1).
   * Empty (default) = no issuer check. Fluent; set before first use.
   */
  Rs256JwtVerifier& expect_issuer(std::string issuer) {
    expected_issuer_ = std::move(issuer);
    return *this;
  }

  /**
   * @brief Require the token's `aud` to equal @p audience (RFC 8725 §3.2).
   * Empty (default) = no audience check. Note: only a string `aud` is matched;
   * array audiences are not yet supported. Fluent; set before first use.
   */
  Rs256JwtVerifier& expect_audience(std::string audience) {
    expected_audience_ = std::move(audience);
    return *this;
  }

  std::optional<TokenClaims> verify(std::string_view token) noexcept override {
    // Split "header.payload.signature" — exactly three base64url parts.
    const auto d1 = token.find('.');
    if (d1 == std::string_view::npos) return std::nullopt;
    const auto d2 = token.find('.', d1 + 1);
    if (d2 == std::string_view::npos) return std::nullopt;
    const std::string_view h_b64 = token.substr(0, d1);
    const std::string_view p_b64 = token.substr(d1 + 1, d2 - (d1 + 1));
    const std::string_view s_b64 = token.substr(d2 + 1);
    if (s_b64.find('.') != std::string_view::npos) return std::nullopt;
    const std::string_view signing_input = token.substr(0, d2); // header.payload

    // Header: pin alg == RS256 (reject none/HS256 downgrade), read kid.
    auto header = crypto::base64url_decode(h_b64);
    if (!header) return std::nullopt;
    if (detail::jwtclaims::find_string(*header, "alg") != "RS256")
      return std::nullopt;
    const std::string kid = detail::jwtclaims::find_string(*header, "kid");

    const JwkRsaKey* key = find_key(kid);
    if (key == nullptr) return std::nullopt;

    // Verify the signature over the ASCII signing input.
    auto sig = crypto::base64url_decode(s_b64);
    if (!sig) return std::nullopt;
    const auto* in_p = reinterpret_cast<const uint8_t*>(signing_input.data());
    const auto* sig_p = reinterpret_cast<const uint8_t*>(sig->data());
    if (!crypto::rsa_pkcs1_v15_sha256_verify(
            key->n, key->e,
            std::span<const uint8_t>(in_p, signing_input.size()),
            std::span<const uint8_t>(sig_p, sig->size())))
      return std::nullopt;

    // Signature is valid → trust the payload; enforce exp/nbf, extract claims.
    auto payload = crypto::base64url_decode(p_b64);
    if (!payload) return std::nullopt;
    const std::string& json = *payload;

    const long now = static_cast<long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const long exp = detail::jwtclaims::find_number(json, "exp");
    if (exp >= 0 && now > exp + leeway_) return std::nullopt; // expired
    const long nbf = detail::jwtclaims::find_number(json, "nbf");
    if (nbf >= 0 && now + leeway_ < nbf) return std::nullopt; // not yet valid

    TokenClaims c;
    c.subject  = detail::jwtclaims::find_string(json, "sub");
    c.issuer   = detail::jwtclaims::find_string(json, "iss");
    c.audience = detail::jwtclaims::find_string(json, "aud");
    c.exp = exp;
    c.nbf = nbf;

    // RFC 8725 §3.1/§3.2: validate issuer/audience when configured.
    if (!expected_issuer_.empty() && c.issuer != expected_issuer_)
      return std::nullopt;
    if (!expected_audience_.empty() && c.audience != expected_audience_)
      return std::nullopt;
    return c;
  }

private:
  // kid selection. With MULTIPLE keys we require a matching kid (true JWKS key
  // rotation). With a SINGLE configured key we use it regardless of the token's
  // kid — this is safe because the RSA signature check is the real gate: a token
  // signed by any other key simply fails verification. (Single-key mode is the
  // raw-key constructor or a one-entry JWKS.)
  [[nodiscard]] const JwkRsaKey* find_key(std::string_view kid) const noexcept {
    if (keys_.size() == 1) return &keys_[0];
    if (kid.empty()) return nullptr;
    for (const auto& k : keys_)
      if (k.kid == kid) return &k;
    return nullptr;
  }

  std::vector<JwkRsaKey> keys_;
  long                   leeway_ = 0;
  std::string            expected_issuer_;   // empty = no iss check
  std::string            expected_audience_; // empty = no aud check
};

} // namespace qbuem::middleware
