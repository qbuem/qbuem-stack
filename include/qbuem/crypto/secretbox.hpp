#pragma once

/**
 * @file qbuem/crypto/secretbox.hpp
 * @brief Misuse-resistant high-level AEAD + password hashing.
 * @defgroup qbuem_secretbox Secretbox (easy AEAD)
 * @ingroup qbuem_crypto
 *
 * The raw `chacha20_poly1305_seal/open` primitives require the caller to supply
 * a unique 12-byte nonce by hand — and nonce reuse with the same key is a
 * catastrophic, silent failure (it leaks the keystream and allows forgery).
 * This header removes that footgun:
 *
 * - `seal_easy()` generates a fresh random nonce from the CSPRNG and packs
 *   `nonce(12) || ciphertext || tag(16)` into one buffer.
 * - `open_easy()` unpacks and verifies it.
 *
 * It also provides `password_hash()` / `verify_password()` over PBKDF2-HMAC-
 * SHA-256 with a random salt and a self-describing PHC-style string, so callers
 * never hand-roll salt/iteration/encoding plumbing.
 *
 * @code
 * auto key = qbuem::crypto::random_bytes<32>().value();
 * auto box = qbuem::crypto::seal_easy(key, plaintext).value();   // nonce||ct||tag
 * auto pt  = qbuem::crypto::open_easy(key, box).value();
 *
 * auto phc = qbuem::crypto::password_hash("hunter2").value();
 * bool ok  = qbuem::crypto::verify_password("hunter2", phc);     // constant-time
 * @endcode
 * @{
 */

#include <qbuem/crypto/base64.hpp>
#include <qbuem/crypto/chacha20_poly1305.hpp>
#include <qbuem/crypto/pbkdf2.hpp>
#include <qbuem/crypto/random.hpp>
#include <qbuem/crypto/secure_zero.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qbuem::crypto {

/** @brief Bytes prepended (nonce) + appended (tag) by seal_easy(). */
inline constexpr size_t kSecretboxOverhead = 12 /*nonce*/ + 16 /*tag*/;

/**
 * @brief Encrypt + authenticate with an auto-generated random nonce.
 *
 * @param key       32-byte secret key.
 * @param plaintext Data to encrypt.
 * @param aad       Optional additional authenticated data (not encrypted).
 * @returns `nonce(12) || ciphertext || tag(16)`, or a CSPRNG error.
 */
[[nodiscard]] inline Result<std::vector<uint8_t>>
seal_easy(const AeadKey& key, std::span<const uint8_t> plaintext,
          std::span<const uint8_t> aad = {}) {
  auto nonce_r = random_bytes<12>();
  if (!nonce_r) return std::unexpected(nonce_r.error());
  const AeadNonce nonce = *nonce_r;

  std::vector<uint8_t> out(kSecretboxOverhead + plaintext.size());
  std::memcpy(out.data(), nonce.data(), 12);

  AeadTag tag{};
  auto r = chacha20_poly1305_seal(
      key, nonce, aad, plaintext,
      std::span<uint8_t>(out.data() + 12, plaintext.size()), tag);
  if (!r) return std::unexpected(r.error());
  std::memcpy(out.data() + 12 + plaintext.size(), tag.data(), 16);
  return out;
}

/**
 * @brief Verify + decrypt a buffer produced by seal_easy().
 *
 * @param key    32-byte secret key (must match the seal key).
 * @param sealed `nonce(12) || ciphertext || tag(16)`.
 * @param aad    Optional additional authenticated data (must match seal).
 * @returns The plaintext, `errc::bad_message` on auth failure / truncation.
 */
[[nodiscard]] inline Result<std::vector<uint8_t>>
open_easy(const AeadKey& key, std::span<const uint8_t> sealed,
          std::span<const uint8_t> aad = {}) {
  if (sealed.size() < kSecretboxOverhead)
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  AeadNonce nonce{};
  std::memcpy(nonce.data(), sealed.data(), 12);
  const size_t ct_len = sealed.size() - kSecretboxOverhead;
  AeadTag tag{};
  std::memcpy(tag.data(), sealed.data() + 12 + ct_len, 16);

  std::vector<uint8_t> pt(ct_len);
  auto r = chacha20_poly1305_open(
      key, nonce, aad, std::span<const uint8_t>(sealed.data() + 12, ct_len), tag,
      std::span<uint8_t>(pt.data(), ct_len));
  if (!r) return std::unexpected(r.error());
  return pt;
}

// ─── Password hashing (PBKDF2-HMAC-SHA-256) ──────────────────────────────────

/** @brief Default PBKDF2 iteration count (OWASP 2023 baseline). */
inline constexpr uint32_t kPasswordHashIterations = 600'000;

/**
 * @brief Hash a password into a self-describing PHC-style string.
 *
 * Format: `pbkdf2_sha256$<iterations>$<salt_b64url>$<hash_b64url>` (16-byte
 * random salt, 32-byte derived key). Store this string; verify with
 * verify_password(). Run on an offload thread — PBKDF2 is CPU-bound.
 */
[[nodiscard]] inline Result<std::string>
password_hash(std::string_view password,
              uint32_t iterations = kPasswordHashIterations) {
  auto salt_r = random_bytes<16>();
  if (!salt_r) return std::unexpected(salt_r.error());
  const auto salt = *salt_r;

  const auto pw = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(password.data()), password.size());
  std::array<uint8_t, 32> hash{};
  pbkdf2_hmac_sha256(pw, salt, iterations, hash);

  std::string out = "pbkdf2_sha256$";
  out += std::to_string(iterations);
  out += '$';
  out += base64url_encode(salt);
  out += '$';
  out += base64url_encode(hash);
  secure_zero(hash.data(), hash.size());
  return out;
}

/**
 * @brief Verify a password against a string produced by password_hash().
 *
 * Constant-time digest comparison. Returns false on any parse error or mismatch.
 */
[[nodiscard]] inline bool verify_password(std::string_view password,
                                          std::string_view phc) {
  // Split into exactly 4 '$'-separated fields.
  std::array<std::string_view, 4> f{};
  size_t idx = 0, start = 0;
  for (size_t i = 0; i <= phc.size(); ++i) {
    if (i == phc.size() || phc[i] == '$') {
      if (idx >= 4) return false; // too many fields
      f[idx++] = phc.substr(start, i - start);
      start = i + 1;
    }
  }
  if (idx != 4 || f[0] != "pbkdf2_sha256") return false;

  uint32_t iters = 0;
  auto [ptr, ec] = std::from_chars(f[1].data(), f[1].data() + f[1].size(), iters);
  if (ec != std::errc{} || ptr != f[1].data() + f[1].size() || iters == 0)
    return false;

  auto salt_r = base64url_decode(f[2]);
  auto hash_r = base64url_decode(f[3]);
  if (!salt_r || !hash_r) return false;
  const std::string& salt = *salt_r;
  const std::string& hash = *hash_r;
  if (hash.size() != 32) return false;

  const auto pw = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(password.data()), password.size());
  std::array<uint8_t, 32> computed{};
  pbkdf2_hmac_sha256(
      pw,
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(salt.data()),
                               salt.size()),
      iters, computed);

  volatile uint8_t diff = 0;
  for (size_t i = 0; i < 32; ++i)
    diff |= computed[i] ^ static_cast<uint8_t>(hash[i]);
  secure_zero(computed.data(), computed.size());
  return diff == 0;
}

} // namespace qbuem::crypto

/** @} */
