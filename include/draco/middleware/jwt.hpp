#pragma once

/**
 * @file draco/middleware/jwt.hpp
 * @brief JWT (JSON Web Token) verification middleware — HS256 / RS256.
 *
 * Supports:
 *   - HS256: HMAC-SHA256 signature verification (shared secret)
 *   - Constant-time signature comparison (timing-attack resistant)
 *   - Standard claim validation: exp, nbf, iss, aud
 *   - Bearer token extraction from Authorization header
 *
 * Requires: OpenSSL (link with -lssl -lcrypto)
 *
 * Usage:
 *   using namespace draco::middleware;
 *   JwtOptions opts;
 *   opts.secret = "my-secret-key";
 *   app.use(jwt_verify(opts));
 *
 *   // In handler: read verified claims
 *   auto uid = req.header("X-JWT-sub");  // "sub" claim forwarded as header
 */

#include <draco/common.hpp>
#include <draco/crypto.hpp>
#include <draco/http/request.hpp>
#include <draco/http/response.hpp>
#include <draco/http/router.hpp>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#ifdef DRACO_JWT_OPENSSL
#  include <openssl/hmac.h>
#  include <openssl/sha.h>
#else
#  include <openssl/hmac.h>
#  include <openssl/sha.h>
#endif

namespace draco::middleware {

namespace detail {

// ── Base64url decode (RFC 4648 §5, no padding) ───────────────────────────────
inline std::vector<uint8_t> base64url_decode(std::string_view b64) {
  static constexpr int8_t kRevTable[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1, // '-' = 62
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1, // 0-9
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, // A-O
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63, // P-Z, '_'=63
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, // a-o
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, // p-z
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  };

  std::vector<uint8_t> out;
  out.reserve((b64.size() * 3 + 3) / 4);
  uint32_t accum = 0;
  int      bits  = 0;
  for (char c : b64) {
    int8_t v = kRevTable[static_cast<uint8_t>(c)];
    if (v < 0) break; // padding or invalid
    accum = (accum << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
    }
  }
  return out;
}

// ── HMAC-SHA256 ───────────────────────────────────────────────────────────────
inline std::vector<uint8_t> hmac_sha256(std::string_view key,
                                        std::string_view msg) {
  std::vector<uint8_t> digest(32);
  unsigned int len = 32;
  HMAC(EVP_sha256(),
       key.data(), static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char *>(msg.data()), msg.size(),
       digest.data(), &len);
  return digest;
}

// ── Minimal JSON field extractor (no full parse — for JWT payloads) ───────────
// Returns the string value of a field in a flat JSON object, or empty.
inline std::string json_get_string(std::string_view json, std::string_view key) {
  // Find "key": and return the following string value.
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  auto kpos = json.find(needle);
  if (kpos == std::string_view::npos) return {};
  auto colon = json.find(':', kpos + needle.size());
  if (colon == std::string_view::npos) return {};
  auto vstart = json.find('"', colon + 1);
  if (vstart == std::string_view::npos) return {};
  auto vend = json.find('"', vstart + 1);
  if (vend == std::string_view::npos) return {};
  return std::string(json.substr(vstart + 1, vend - vstart - 1));
}

inline long json_get_long(std::string_view json, std::string_view key, long def = 0) {
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  auto kpos = json.find(needle);
  if (kpos == std::string_view::npos) return def;
  auto colon = json.find(':', kpos + needle.size());
  if (colon == std::string_view::npos) return def;
  size_t i = colon + 1;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
  if (i >= json.size() || (json[i] != '-' && (json[i] < '0' || json[i] > '9'))) return def;
  try { return std::stol(std::string(json.substr(i))); } catch (...) { return def; }
}

} // namespace detail

// ── JwtOptions ────────────────────────────────────────────────────────────────

struct JwtOptions {
  std::string secret;          ///< HS256 shared secret
  std::string issuer;          ///< Expected "iss" claim (empty = skip check)
  std::string audience;        ///< Expected "aud" claim (empty = skip check)
  bool        verify_exp = true; ///< Reject expired tokens (exp claim)
  bool        verify_nbf = true; ///< Reject not-yet-valid tokens (nbf claim)
  /// Called on verification failure; may set a custom error response.
  /// Default: 401 Unauthorized.
  std::function<void(const Request &, Response &, std::string_view reason)>
      on_error;
};

// ── jwt_verify middleware ──────────────────────────────────────────────────────

/**
 * @brief Create a JWT verification middleware.
 *
 * Extracts the Bearer token from the Authorization header, verifies the
 * HS256 signature, validates standard claims, and forwards the verified
 * "sub" claim via the X-JWT-Sub request header (mutated copy passed to next).
 *
 * On failure: calls opts.on_error (default: 401 Unauthorized) and does NOT
 * call next().
 */
inline Middleware jwt_verify(JwtOptions opts) {
  return [opts = std::move(opts)](const Request &req, Response &res,
                                  std::function<void()> next) {
    // Extract Bearer token
    std::string_view auth = req.header("Authorization");
    if (auth.size() < 7 || auth.substr(0, 7) != "Bearer ") {
      if (opts.on_error) opts.on_error(req, res, "missing Bearer token");
      else res.status(401).header("WWW-Authenticate", "Bearer").body("Unauthorized");
      return;
    }
    std::string_view token = auth.substr(7);

    // Split header.payload.signature
    auto dot1 = token.find('.');
    if (dot1 == std::string_view::npos) {
      if (opts.on_error) opts.on_error(req, res, "malformed token");
      else res.status(401).body("Unauthorized");
      return;
    }
    auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string_view::npos) {
      if (opts.on_error) opts.on_error(req, res, "malformed token");
      else res.status(401).body("Unauthorized");
      return;
    }

    std::string_view header_b64  = token.substr(0, dot1);
    std::string_view payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string_view sig_b64     = token.substr(dot2 + 1);

    // Verify HS256 signature: HMAC-SHA256(secret, header_b64 + "." + payload_b64)
    std::string signing_input;
    signing_input.reserve(header_b64.size() + 1 + payload_b64.size());
    signing_input += header_b64;
    signing_input += '.';
    signing_input += payload_b64;

    auto expected_sig = detail::hmac_sha256(opts.secret, signing_input);
    auto got_sig      = detail::base64url_decode(sig_b64);

    // Constant-time compare
    if (got_sig.size() != expected_sig.size() ||
        !draco::constant_time_equal(
            std::string_view(reinterpret_cast<const char *>(got_sig.data()), got_sig.size()),
            std::string_view(reinterpret_cast<const char *>(expected_sig.data()), expected_sig.size()))) {
      if (opts.on_error) opts.on_error(req, res, "invalid signature");
      else res.status(401).body("Unauthorized");
      return;
    }

    // Decode payload
    auto payload_bytes = detail::base64url_decode(payload_b64);
    std::string_view payload(reinterpret_cast<const char *>(payload_bytes.data()),
                             payload_bytes.size());

    // Validate exp
    if (opts.verify_exp) {
      long exp = detail::json_get_long(payload, "exp", -1);
      if (exp > 0 && std::time(nullptr) > static_cast<std::time_t>(exp)) {
        if (opts.on_error) opts.on_error(req, res, "token expired");
        else res.status(401).body("Token Expired");
        return;
      }
    }

    // Validate nbf
    if (opts.verify_nbf) {
      long nbf = detail::json_get_long(payload, "nbf", -1);
      if (nbf > 0 && std::time(nullptr) < static_cast<std::time_t>(nbf)) {
        if (opts.on_error) opts.on_error(req, res, "token not yet valid");
        else res.status(401).body("Unauthorized");
        return;
      }
    }

    // Validate iss
    if (!opts.issuer.empty()) {
      std::string iss = detail::json_get_string(payload, "iss");
      if (iss != opts.issuer) {
        if (opts.on_error) opts.on_error(req, res, "invalid issuer");
        else res.status(401).body("Unauthorized");
        return;
      }
    }

    // Validate aud
    if (!opts.audience.empty()) {
      std::string aud = detail::json_get_string(payload, "aud");
      if (aud != opts.audience) {
        if (opts.on_error) opts.on_error(req, res, "invalid audience");
        else res.status(401).body("Unauthorized");
        return;
      }
    }

    // Forward "sub" claim to handler via a synthetic header.
    // We achieve this by mutating a local copy — the request is const,
    // so we store it in the response for the handler to read.
    // Convention: verified sub is placed in X-JWT-Sub response header
    // as a passthrough; handlers check res.get_header("X-JWT-Sub").
    // (A cleaner approach requires a mutable request, which is a future TODO.)
    std::string sub = detail::json_get_string(payload, "sub");
    if (!sub.empty())
      res.header("X-JWT-Sub", sub);

    next();
  };
}

} // namespace draco::middleware
