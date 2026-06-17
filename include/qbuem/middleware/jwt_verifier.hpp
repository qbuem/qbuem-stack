#pragma once

/**
 * @file qbuem/middleware/jwt_verifier.hpp
 * @brief HS256 JWT ITokenVerifier — drop-in auth for the token_auth middleware.
 * @ingroup qbuem_middleware
 *
 * Bridges the zero-dependency `crypto::verify_jwt_hs256` to the
 * `ITokenVerifier` interface so a service can require JWT auth without pulling
 * in OpenSSL (the previous documented path). Signature + `alg` are verified by
 * the crypto layer; this adapter additionally enforces `exp`/`nbf` and extracts
 * the standard claims into `TokenClaims`.
 *
 * @code
 * auto verifier = std::make_shared<qbuem::middleware::HmacJwtVerifier>(secret);
 * app.use_async(qbuem::middleware::bearer_auth(verifier));
 * @endcode
 */

#include <qbuem/crypto/jwt.hpp>
#include <qbuem/middleware/token_auth.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace qbuem::middleware {

namespace detail::jwtclaims {

// Minimal flat-JSON claim extraction. The signature guarantees the payload was
// produced by the trusted issuer, so a naive scan of standard top-level claims
// is sufficient (an attacker cannot alter the payload without breaking the MAC).
inline std::string find_string(std::string_view json, std::string_view key) {
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  auto pos = json.find(needle);
  if (pos == std::string_view::npos) return {};
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
    ++pos;
  if (pos >= json.size() || json[pos] != '"') return {};
  ++pos;
  std::string out;
  while (pos < json.size() && json[pos] != '"') {
    if (json[pos] == '\\' && pos + 1 < json.size()) ++pos; // skip escape
    out.push_back(json[pos]);
    ++pos;
  }
  return out;
}

inline long find_number(std::string_view json, std::string_view key) {
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  auto pos = json.find(needle);
  if (pos == std::string_view::npos) return -1;
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
    ++pos;
  long val = 0;
  bool any = false;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    val = val * 10 + (json[pos] - '0');
    ++pos;
    any = true;
  }
  return any ? val : -1;
}

} // namespace detail::jwtclaims

/**
 * @brief ITokenVerifier that validates HS256 JWTs with the library's own HMAC.
 *
 * Rejects: bad signature, `alg != HS256` (incl. `none`), expired (`exp`),
 * not-yet-valid (`nbf`). Extracts `sub`/`iss`/`aud`/`exp`/`nbf`.
 */
class HmacJwtVerifier : public ITokenVerifier {
public:
  /** @param secret HMAC signing secret (shared with the issuer). */
  explicit HmacJwtVerifier(std::string secret) : secret_(std::move(secret)) {}

  /** @param leeway_seconds Clock-skew tolerance applied to exp/nbf checks. */
  HmacJwtVerifier(std::string secret, long leeway_seconds)
      : secret_(std::move(secret)), leeway_(leeway_seconds) {}

  std::optional<TokenClaims> verify(std::string_view token) noexcept override {
    auto payload = crypto::verify_jwt_hs256(token, secret_);
    if (!payload) return std::nullopt; // bad signature / alg / structure

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
    return c;
  }

private:
  std::string secret_;
  long        leeway_ = 0;
};

} // namespace qbuem::middleware
