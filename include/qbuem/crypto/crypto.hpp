#pragma once

/**
 * @file qbuem/crypto/crypto.hpp
 * @brief Umbrella header — includes all qbuem cryptographic primitives.
 * @ingroup qbuem_crypto
 *
 * ## Module overview
 *
 * All implementations are header-only, zero-dependency (C++23 stdlib only),
 * and zero-allocation on hot paths.
 *
 * ### Layer 1 — Primitives (safe to implement from spec)
 *
 * | Header              | Algorithms                               | Standard      |
 * |---------------------|------------------------------------------|---------------|
 * | `random.hpp`        | CSPRNG, RDRAND, RDSEED                   | POSIX         |
 * | `sha256.hpp`        | SHA-256, SHA-224                         | FIPS 180-4    |
 * | `sha512.hpp`        | SHA-512, SHA-384, SHA-512/256, SHA-512/224 | FIPS 180-4  |
 * | `hmac.hpp`          | HMAC-SHA-256, HMAC-SHA-512               | RFC 2104      |
 * | `pbkdf2.hpp`        | PBKDF2-HMAC-SHA-256/512                  | RFC 2898      |
 * | `hkdf.hpp`          | HKDF-SHA-256/512 (Extract + Expand)      | RFC 5869      |
 * | `base64.hpp`        | Base64, Base64url (encode + decode)      | RFC 4648      |
 *
 * ### Layer 2 — Stream cipher + MAC (ARX; constant-time by design)
 *
 * | Header                    | Algorithms              | Standard   |
 * |---------------------------|-------------------------|------------|
 * | `chacha20.hpp`            | ChaCha20 stream cipher  | RFC 8439   |
 * | `poly1305.hpp`            | Poly1305 MAC            | RFC 8439   |
 * | `chacha20_poly1305.hpp`   | ChaCha20-Poly1305 AEAD  | RFC 8439   |
 *
 * ### Layer 3 — Hardware AES (safe only with AES-NI; no software fallback)
 *
 * | Header        | Algorithms                   | Requirement      |
 * |---------------|------------------------------|------------------|
 * | `aes_gcm.hpp` | AES-128-GCM, AES-256-GCM    | AES-NI + PCLMUL  |
 *
 * ## Architecture decision: what is NOT included
 *
 * The following algorithms are **intentionally excluded** from this
 * zero-dependency core:
 *
 * - **Software AES**: Table-based AES is vulnerable to cache-timing attacks.
 *   Use `aes_gcm.hpp` (AES-NI only) or `chacha20_poly1305.hpp` (always safe).
 * - **TLS handshake / certificate validation**: These require a trusted
 *   certificate store and complex state machines.  Use the optional
 *   `qbuem-stack::tls` component (wraps OpenSSL/mbedTLS).
 * - **Elliptic curve key exchange (X25519/ECDH)**: Correct constant-time
 *   implementation requires significant effort.  Future work: `x25519.hpp`.
 * - **RSA/DSA/ECDSA signatures**: Requires arbitrary-precision arithmetic
 *   with timing-safe Montgomery multiplication.
 *
 * ## SIMD acceleration summary
 *
 * | Algorithm     | x86-64                     | AArch64              |
 * |---------------|----------------------------|----------------------|
 * | SHA-256       | SHA-NI (`__SHA__`)         | SHA2 (`__ARM_FEATURE_SHA2`) |
 * | SHA-512       | Scalar (no x86 SHA-512 NI) | SHA-512 (ARMv8.2)    |
 * | Base64 encode | (future AVX2)              | NEON vtbl            |
 * | ChaCha20      | (future AVX2)              | NEON 4-block         |
 * | AES-GCM       | AES-NI + PCLMULQDQ         | ARM AES + PMULL      |
 *
 * ## Usage example
 * ```cpp
 * #include <qbuem/crypto/crypto.hpp>
 *
 * // Password hashing (PBKDF2)
 * auto salt = qbuem::crypto::random_bytes<16>().value();
 * auto dk   = qbuem::crypto::pbkdf2_hmac_sha256<32>(password, salt, 600'000);
 *
 * // Symmetric encryption (ChaCha20-Poly1305)
 * auto key   = qbuem::crypto::random_bytes<32>().value();
 * auto nonce = qbuem::crypto::random_bytes<12>().value();
 * auto tag   = qbuem::crypto::chacha20_poly1305_seal(key, nonce, aad, data);
 *
 * // AES-GCM (hardware only)
 * auto ctx = qbuem::crypto::AesGcm256::create(aes_key);
 * if (!ctx) { // use chacha20_poly1305 as fallback
 * }
 *
 * // JWT / token signing (HMAC-SHA-256)
 * auto tag = qbuem::crypto::hmac_sha256(secret, payload);
 * bool ok  = qbuem::crypto::verify_hmac_sha256(secret, payload, expected_tag);
 *
 * // Key derivation (HKDF)
 * auto session_key = qbuem::crypto::hkdf_sha256<32>(shared_secret, salt, "enc");
 * ```
 */

// Layer 1: Primitives
#include <qbuem/crypto/random.hpp>
#include <qbuem/crypto/sha256.hpp>
#include <qbuem/crypto/sha512.hpp>
#include <qbuem/crypto/hmac.hpp>
#include <qbuem/crypto/pbkdf2.hpp>
#include <qbuem/crypto/hkdf.hpp>
#include <qbuem/crypto/base64.hpp>

// Layer 2: Stream cipher + MAC (constant-time ARX)
#include <qbuem/crypto/chacha20.hpp>
#include <qbuem/crypto/poly1305.hpp>
#include <qbuem/crypto/chacha20_poly1305.hpp>

// Layer 3: Hardware AES (AES-NI only; no software fallback)
#include <qbuem/crypto/aes_gcm.hpp>
