#pragma once

/**
 * @file qbuem/crypto/jwt.hpp
 * @brief Zero-dependency JWT HS256 sign + verify (HMAC-SHA-256).
 * @defgroup qbuem_jwt JWT (HS256)
 * @ingroup qbuem_crypto
 *
 * The library ships HMAC-SHA-256 + a SIMD JWT *parser* but previously had no
 * *signer/verifier*, so users hand-rolled signing-input reconstruction +
 * base64url + a constant-time compare — a misuse magnet (alg=none acceptance,
 * non-constant-time compares, signing-input mismatch). This provides a correct,
 * dependency-free HS256 implementation:
 *
 * - `encode_jwt_hs256(payload_json, key)` → `header.payload.signature`.
 * - `verify_jwt_hs256(token, key)` → verified payload JSON (or an error). The
 *   `alg` header is strictly checked == "HS256" (so `none` and asymmetric algs
 *   are rejected), and the signature is compared in constant time.
 *
 * The payload is opaque JSON: the caller builds/parses it (e.g. with qbuem-json
 * at the application layer) — keeping this header zero-dependency.
 *
 * @code
 * auto key = qbuem::crypto::random_bytes<32>().value();
 * auto tok = qbuem::crypto::encode_jwt_hs256(R"({"sub":"u1","exp":9999999999})", key);
 * auto pl  = qbuem::crypto::verify_jwt_hs256(tok, key);   // Result<std::string>
 * @endcode
 * @{
 */

#include <qbuem/crypto/base64.hpp>
#include <qbuem/crypto/hmac.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace qbuem::crypto {

namespace detail::jwt {
inline std::span<const uint8_t> as_bytes(std::string_view s) noexcept {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
} // namespace detail::jwt

/**
 * @brief Sign a JWT with HS256 (HMAC-SHA-256).
 *
 * @param payload_json The JWT payload as a JSON object string (claims).
 * @param key          HMAC secret key.
 * @returns `base64url(header).base64url(payload).base64url(HMAC)`.
 */
[[nodiscard]] inline std::string
encode_jwt_hs256(std::string_view payload_json, std::span<const uint8_t> key) {
  static constexpr std::string_view kHeader = R"({"alg":"HS256","typ":"JWT"})";
  std::string signing_input = base64url_encode(detail::jwt::as_bytes(kHeader));
  signing_input += '.';
  signing_input += base64url_encode(detail::jwt::as_bytes(payload_json));

  const Sha256Digest mac =
      hmac_sha256(key, detail::jwt::as_bytes(signing_input));
  std::string out = std::move(signing_input);
  out += '.';
  out += base64url_encode(std::span<const uint8_t>(mac.data(), mac.size()));
  return out;
}

/** @overload Accepts a string_view key. */
[[nodiscard]] inline std::string
encode_jwt_hs256(std::string_view payload_json, std::string_view key) {
  return encode_jwt_hs256(payload_json, detail::jwt::as_bytes(key));
}

/**
 * @brief Verify an HS256 JWT and return its payload JSON.
 *
 * Strictly requires the header `alg` to be `"HS256"` (rejects `none` and any
 * asymmetric algorithm — the classic JWT downgrade attack). The signature is
 * verified in constant time. This checks the SIGNATURE only; expiry/audience
 * validation is the caller's responsibility on the returned payload.
 *
 * @param token Compact JWT (`header.payload.signature`).
 * @param key   HMAC secret key (must match the signing key).
 * @returns The decoded payload JSON on success; `errc::bad_message` if the
 *          structure, `alg`, or signature is invalid.
 */
[[nodiscard]] inline Result<std::string>
verify_jwt_hs256(std::string_view token, std::span<const uint8_t> key) {
  const auto bad = [] {
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  };

  // Split into exactly 3 dot-separated segments.
  const size_t d1 = token.find('.');
  if (d1 == std::string_view::npos) return bad();
  const size_t d2 = token.find('.', d1 + 1);
  if (d2 == std::string_view::npos) return bad();
  if (token.find('.', d2 + 1) != std::string_view::npos) return bad();

  const std::string_view header_b64  = token.substr(0, d1);
  const std::string_view payload_b64 = token.substr(d1 + 1, d2 - d1 - 1);
  const std::string_view sig_b64     = token.substr(d2 + 1);
  const std::string_view signing_input = token.substr(0, d2);

  // Header must declare alg == HS256 (reject "none"/RS256/ES256/...).
  auto header = base64url_decode(header_b64);
  if (!header) return bad();
  if (header->find("\"HS256\"") == std::string::npos) return bad();
  if (header->find("\"alg\"") == std::string::npos) return bad();

  // Recompute the MAC and compare to the provided signature in constant time.
  auto sig = base64url_decode(sig_b64);
  if (!sig || sig->size() != 32) return bad();
  const Sha256Digest mac =
      hmac_sha256(key, detail::jwt::as_bytes(signing_input));
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < 32; ++i)
    diff |= mac[i] ^ static_cast<uint8_t>((*sig)[i]);
  if (diff != 0) return bad();

  return base64url_decode(payload_b64);
}

/** @overload Accepts a string_view key. */
[[nodiscard]] inline Result<std::string>
verify_jwt_hs256(std::string_view token, std::string_view key) {
  return verify_jwt_hs256(token, detail::jwt::as_bytes(key));
}

} // namespace qbuem::crypto

/** @} */
