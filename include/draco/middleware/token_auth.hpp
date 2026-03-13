#pragma once

/**
 * @file draco/middleware/token_auth.hpp
 * @brief Abstract token authentication interface + Bearer middleware.
 *
 * qbuem-stack provides ZERO external library dependencies.
 * Token verification (JWT HS256/RS256, PASETO, API key, …) is implemented
 * by the application and injected via ITokenVerifier.
 *
 * --- Quick start ---
 *
 * 1. Implement ITokenVerifier in your service (e.g. with OpenSSL for HS256):
 *
 *   #include <openssl/hmac.h>
 *   #include <draco/middleware/token_auth.hpp>
 *
 *   class HS256Verifier : public draco::middleware::ITokenVerifier {
 *   public:
 *     explicit HS256Verifier(std::string secret) : secret_(std::move(secret)) {}
 *
 *     std::optional<draco::middleware::TokenClaims>
 *     verify(std::string_view token) noexcept override {
 *       // 1. Split header.payload.sig
 *       // 2. HMAC-SHA256(secret, header + "." + payload)
 *       // 3. constant_time_equal(computed, sig)
 *       // 4. base64url-decode payload, parse JSON claims
 *       // 5. Check exp / nbf
 *       // Return nullopt on any failure.
 *     }
 *   private:
 *     std::string secret_;
 *   };
 *
 * 2. Register with the middleware:
 *
 *   HS256Verifier verifier("my-secret");
 *   app.use(draco::middleware::bearer_auth(verifier));
 */

#include <draco/http/request.hpp>
#include <draco/http/response.hpp>
#include <draco/http/router.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace draco::middleware {

// ─── TokenClaims ──────────────────────────────────────────────────────────────

/**
 * @brief Verified token claims forwarded to route handlers.
 *
 * Handlers access verified claims via req.header("X-Auth-Sub") etc.
 * after the middleware has run.
 */
struct TokenClaims {
  std::string subject;   ///< "sub" claim (user/device ID)
  std::string issuer;    ///< "iss" claim
  std::string audience;  ///< "aud" claim
  long        exp = -1;  ///< Unix timestamp; -1 = not present
  long        nbf = -1;  ///< Unix timestamp; -1 = not present

  /// Custom claims extracted by the verifier (key → string value).
  std::unordered_map<std::string, std::string> custom;
};

// ─── ITokenVerifier ───────────────────────────────────────────────────────────

/**
 * @brief Abstract interface for token verification.
 *
 * Implement this in your service for whichever scheme you use:
 *   - JWT HS256 / RS256 / ES256 (via OpenSSL, mbedTLS, …)
 *   - PASETO v4
 *   - Opaque API key lookup (database or in-memory map)
 *   - mTLS client certificate validation
 *
 * Thread safety: verify() must be safe to call concurrently from multiple
 * reactor threads.
 */
class ITokenVerifier {
public:
  virtual ~ITokenVerifier() = default;

  /**
   * @brief Verify @p token and return the extracted claims.
   *
   * @param token  Raw token string (without "Bearer " prefix).
   * @returns      Claims on success; std::nullopt on any verification failure.
   *
   * Implementations MUST NOT throw.  Use noexcept and return nullopt on error.
   */
  virtual std::optional<TokenClaims> verify(std::string_view token) noexcept = 0;
};

// ─── bearer_auth middleware ────────────────────────────────────────────────────

/**
 * @brief Options for bearer_auth().
 */
struct BearerAuthOptions {
  /**
   * @brief Called when authentication fails.
   *
   * Default: responds 401 Unauthorized and halts the chain.
   */
  std::function<void(const Request &, Response &, std::string_view reason)>
      on_error;

  /**
   * @brief Header prefix forwarded to handlers on success (default: "X-Auth-").
   *
   * On success, claims are forwarded as synthetic request-side response headers:
   *   X-Auth-Sub  → claims.subject
   *   X-Auth-Iss  → claims.issuer
   *   X-Auth-Aud  → claims.audience
   *
   * Custom claims are forwarded as X-Auth-<Key> (key lowercased).
   *
   * Because draco Request is immutable inside middleware, claims are stored in
   * the Response as X-Auth-* headers for handlers to read via
   * res.get_header("X-Auth-Sub").
   */
  std::string claims_prefix = "X-Auth-";
};

/**
 * @brief Returns a Bearer token authentication middleware.
 *
 * Extracts the Bearer token from the Authorization header, delegates
 * verification to @p verifier, and on success forwards verified claims as
 * response-side X-Auth-* headers readable by the handler.
 *
 * On failure, responds with 401 and halts the middleware chain.
 *
 * @param verifier  Token verifier instance (must outlive the App).
 * @param opts      Optional error handler and header prefix.
 *
 * Example:
 *   HS256Verifier v("secret");
 *   app.use(draco::middleware::bearer_auth(v));
 *
 *   app.get("/me", [](const auto& req, auto& res) {
 *     auto sub = res.get_header("X-Auth-Sub");  // verified subject
 *     res.status(200).body(sub);
 *   });
 */
inline Middleware bearer_auth(ITokenVerifier &verifier,
                               BearerAuthOptions opts = {}) {
  return [&verifier, opts = std::move(opts)](const Request &req,
                                              Response &res) -> bool {
    std::string_view auth = req.header("Authorization");

    // Require "Bearer <token>"
    static constexpr std::string_view kBearer = "Bearer ";
    if (auth.size() <= kBearer.size() ||
        auth.substr(0, kBearer.size()) != kBearer) {
      if (opts.on_error) {
        opts.on_error(req, res, "missing Bearer token");
      } else {
        res.status(401)
           .header("WWW-Authenticate", "Bearer realm=\"qbuem-stack\"")
           .body("Unauthorized");
      }
      return false;
    }

    std::string_view token = auth.substr(kBearer.size());
    auto claims = verifier.verify(token);
    if (!claims) {
      if (opts.on_error) {
        opts.on_error(req, res, "token verification failed");
      } else {
        res.status(401)
           .header("WWW-Authenticate", "Bearer realm=\"qbuem-stack\",error=\"invalid_token\"")
           .body("Unauthorized");
      }
      return false;
    }

    // Forward verified claims as response headers (readable by handler).
    const std::string &pfx = opts.claims_prefix;
    if (!claims->subject.empty())  res.header(pfx + "Sub", claims->subject);
    if (!claims->issuer.empty())   res.header(pfx + "Iss", claims->issuer);
    if (!claims->audience.empty()) res.header(pfx + "Aud", claims->audience);
    for (const auto &[k, v] : claims->custom)
      res.header(pfx + k, v);

    return true; // continue chain
  };
}

} // namespace draco::middleware
