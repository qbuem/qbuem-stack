# Cryptographic Primitives Module

`include/qbuem/crypto/` — Zero-dependency, zero-allocation cryptography for C++23.

---

## 1. Overview

The `qbuem::crypto` namespace provides a complete, production-grade cryptographic
toolkit built entirely on the C++23 standard library. No external dependencies
(no OpenSSL, libsodium, or Botan) are required.

All primitives satisfy the three invariants:

| Invariant | Guarantee |
|-----------|-----------|
| **Zero allocation** | No `new`, no `std::vector` on hot paths; all state in `std::array` or caller-provided `std::span<uint8_t>` |
| **Constant-time** | No secret-dependent branches or table lookups in MAC/compare paths |
| **SIMD-first** | SHA-NI / ARM SHA2 for hashing; AVX2 / NEON for Base64; AES-NI / ARM AES for AES-GCM |

---

## 2. Module Structure

```
include/qbuem/crypto/
├── crypto.hpp              ← Umbrella: include this for everything
│
├── random.hpp              ← CSPRNG (Layer 1)
├── sha256.hpp              ← SHA-256 / SHA-224 (Layer 1)
├── sha512.hpp              ← SHA-512 / SHA-384 / SHA-512/256 / SHA-512/224 (Layer 1)
├── hmac.hpp                ← HMAC-SHA-256 / HMAC-SHA-512 (Layer 1)
├── pbkdf2.hpp              ← PBKDF2-HMAC-SHA-256/512 (Layer 1)
├── hkdf.hpp                ← HKDF-SHA-256/512 extract + expand (Layer 1)
├── base64.hpp              ← Base64 / Base64url encode + decode (Layer 2)
│
├── chacha20.hpp            ← ChaCha20 stream cipher (Layer 2)
├── poly1305.hpp            ← Poly1305 one-time MAC (Layer 2)
├── chacha20_poly1305.hpp   ← ChaCha20-Poly1305 AEAD (Layer 2)
│
└── aes_gcm.hpp             ← AES-128-GCM / AES-256-GCM (Layer 3 — AES-NI only)
```

---

## 3. Algorithms

### 3.1 Hash Functions

| Function | Standard | Output | SIMD path |
|----------|----------|--------|-----------|
| `sha256(data)` | FIPS 180-4 | 32 bytes | SHA-NI (x86) / ARM SHA2 / scalar |
| `sha224(data)` | FIPS 180-4 | 28 bytes | same compression as SHA-256 |
| `sha512(data)` | FIPS 180-4 | 64 bytes | ARMv8.2-SHA512 / scalar |
| `sha384(data)` | FIPS 180-4 | 48 bytes | same compression as SHA-512 |
| `sha512_256(data)` | FIPS 180-4 | 32 bytes | same compression as SHA-512 |
| `sha512_224(data)` | FIPS 180-4 | 28 bytes | same compression as SHA-512 |

**Streaming interface** (avoids copying large inputs):

```cpp
qbuem::crypto::Sha256Context ctx;
ctx.update(header_span);
ctx.update(body_span);
auto digest = ctx.finalize();  // std::array<uint8_t, 32>
```

### 3.2 Message Authentication (HMAC)

| Function | Standard | Tag size | Notes |
|----------|----------|----------|-------|
| `hmac_sha256(key, msg)` | RFC 2104 | 32 bytes | one-shot |
| `hmac_sha512(key, msg)` | RFC 2104 | 64 bytes | one-shot |
| `verify_hmac_sha256(key, msg, tag)` | RFC 2104 | — | constant-time; bool result |
| `verify_hmac_sha512(key, msg, tag)` | RFC 2104 | — | constant-time |
| `HmacSha256{key}` | RFC 2104 | — | streaming; `reset()` + `update()` + `finalize()` |

```cpp
// Sign
auto tag = qbuem::crypto::hmac_sha256("api-secret", payload);

// Verify (timing-safe — no early exit on mismatch)
bool ok = qbuem::crypto::verify_hmac_sha256(
    {key.data(), key.size()}, {msg.data(), msg.size()}, expected_tag);
```

### 3.3 Key Derivation

#### PBKDF2 — Password Hashing

| Function | Standard | Use case |
|----------|----------|---------|
| `pbkdf2_hmac_sha256<N>(pw, salt, iter)` | RFC 2898 | Password storage |
| `pbkdf2_hmac_sha512<N>(pw, salt, iter)` | RFC 2898 | Password storage (higher security) |

Recommended iteration counts (OWASP 2023):
- SHA-256: **600 000** iterations
- SHA-512: **210 000** iterations

```cpp
// Generate a random salt once; store it alongside the hash
auto salt = qbuem::crypto::random_bytes<16>().value();

// Derive 32-byte key
auto dk = qbuem::crypto::pbkdf2_hmac_sha256<32>(password, salt, 600'000);
```

#### HKDF — Key Derivation from Shared Secret

| Function | Standard | Use case |
|----------|----------|---------|
| `hkdf_extract_sha256(salt, ikm)` | RFC 5869 | Condense IKM into PRK |
| `hkdf_expand_sha256(prk, info, out)` | RFC 5869 | Derive multiple sub-keys |
| `hkdf_extract_sha512` / `hkdf_expand_sha512` | RFC 5869 | SHA-512 variant |

```cpp
// Typical TLS-like session key derivation
auto prk = qbuem::crypto::hkdf_extract_sha256(server_nonce, ecdh_shared_secret);

std::array<uint8_t, 32> enc_key{}, auth_key{};
qbuem::crypto::hkdf_expand_sha256(prk, "enc v1",  enc_key);
qbuem::crypto::hkdf_expand_sha256(prk, "auth v1", auth_key);
```

### 3.4 Base64 / Base64url Encoding

| Function | RFC | Padding |
|----------|-----|---------|
| `base64_encode(data)` | RFC 4648 §4 | `=` padded |
| `base64_decode(str)` | RFC 4648 §4 | with/without `=` |
| `base64url_encode(data)` | RFC 4648 §5 | no padding (default) |
| `base64url_decode(str)` | RFC 4648 §5 | with/without `=` |

`base64_decode` / `base64url_decode` return `Result<std::string>` (i.e.,
`std::expected<std::string, std::error_code>`); invalid characters yield an error code.

```cpp
// Standard Base64
std::string enc = qbuem::crypto::base64_encode(binary_data);
auto dec = qbuem::crypto::base64_decode(enc);
if (!dec) handle_error(dec.error());

// URL-safe Base64 for JWT / session tokens
std::string jwt_safe = qbuem::crypto::base64url_encode(signature_bytes);
```

### 3.5 Authenticated Encryption (AEAD)

#### ChaCha20-Poly1305 (recommended — always available)

| Function | Standard | Notes |
|----------|----------|-------|
| `chacha20_poly1305_seal(key, nonce, aad, pt, ct, tag)` | RFC 8439 | separate output buffers |
| `chacha20_poly1305_seal(key, nonce, aad, data)` | RFC 8439 | in-place; returns `AeadTag` |
| `chacha20_poly1305_open(key, nonce, aad, ct, tag, pt)` | RFC 8439 | authenticate-before-decrypt |
| `chacha20_poly1305_open(key, nonce, aad, data, tag)` | RFC 8439 | in-place |

```cpp
using namespace qbuem::crypto;

AeadKey   key   = random_bytes<32>().value();
AeadNonce nonce = random_bytes<12>().value();

// Encrypt
std::vector<uint8_t> ct(plaintext.size());
AeadTag tag{};
chacha20_poly1305_seal(key, nonce, aad, plaintext, ct, tag);

// Decrypt + verify (returns error if tag is invalid)
auto result = chacha20_poly1305_open(key, nonce, aad, ct, tag, plaintext_out);
if (!result) { /* forgery / corruption detected */ }
```

**Security requirements:**
- The nonce must be **unique per (key, message) pair**. Reuse leaks the key stream.
- Use a random 96-bit nonce or a monotonically increasing 64-bit counter.

#### AES-GCM (hardware-only)

`AesGcm128` / `AesGcm256` use AES-NI + PCLMULQDQ on x86-64 or ARM AES + PMULL on AArch64.
No software fallback is provided (table-based AES is vulnerable to cache-timing attacks).

```cpp
if (!qbuem::crypto::has_aes_ni()) {
    // Fall back to ChaCha20-Poly1305 (always safe)
}

auto ctx = qbuem::crypto::AesGcm256::create(aes_key);
if (!ctx) { /* AES-NI unavailable */ }

AesGcmTag tag{};
ctx->seal(nonce, aad, plaintext, ciphertext, tag);
auto r = ctx->open(nonce, aad, ciphertext, tag, plaintext_out);
```

### 3.6 CSPRNG

| Function | Source | Notes |
|----------|--------|-------|
| `random_fill(span<uint8_t>)` | `getrandom(2)` / `arc4random_buf` | kernel CSPRNG |
| `hw_random_fill(span<uint8_t>)` | RDRAND → kernel fallback | fast bulk entropy |
| `hw_seed_fill(span<uint8_t>)` | RDSEED → RDRAND → kernel | thermal TRNG |
| `random_bytes<N>()` | kernel CSPRNG | returns `Result<array<uint8_t,N>>` |
| `has_rdrand()` / `has_rdseed()` | CPUID | compile-time or runtime detection |
| `rdrand64(out)` / `rdseed64(out)` | x86 instruction | single 64-bit sample |

---

## 4. SIMD Acceleration Summary

| Algorithm | x86-64 | AArch64 | Scalar fallback |
|-----------|--------|---------|-----------------|
| SHA-256 | SHA-NI (`__SHA__`) ~4 GB/s | ARM SHA2 (`__ARM_FEATURE_SHA2`) ~3 GB/s | ~200–400 MB/s |
| SHA-512 | Scalar | ARMv8.2-SHA512 | Scalar |
| Base64 encode | AVX2 (`__AVX2__`) ~4 GB/s | NEON vtbl ~3 GB/s | ~600 MB/s |
| ChaCha20 | AVX2 8-block | NEON 4-block | 1-block |
| AES-GCM | AES-NI + PCLMUL | ARM AES + PMULL | Not supported |

---

## 5. API Quick Reference

```cpp
#include <qbuem/crypto/crypto.hpp>

using namespace qbuem::crypto;

// ── Hashing ──────────────────────────────────────────────────────
Sha256Digest d = sha256("message");                          // one-shot
Sha512Digest d = sha512(span<const uint8_t>{buf, len});     // one-shot
Sha256Context ctx; ctx.update(a); ctx.update(b);
Sha256Digest d = ctx.finalize();                             // streaming

// ── HMAC ─────────────────────────────────────────────────────────
Sha256Digest tag = hmac_sha256("key", "message");
bool ok = verify_hmac_sha256({key}, {msg}, expected_tag);   // constant-time

// ── PBKDF2 ───────────────────────────────────────────────────────
auto dk = pbkdf2_hmac_sha256<32>(password, salt, 600'000);

// ── HKDF ─────────────────────────────────────────────────────────
auto prk = hkdf_extract_sha256(salt, ikm);
std::array<uint8_t, 32> okm{};
hkdf_expand_sha256(prk, info, okm);

// ── Base64 ───────────────────────────────────────────────────────
std::string enc = base64_encode(data);
auto dec = base64_decode(enc);          // Result<std::string>
std::string url = base64url_encode(data);

// ── ChaCha20-Poly1305 ─────────────────────────────────────────────
AeadKey key = random_bytes<32>().value();
AeadNonce nonce = random_bytes<12>().value();
AeadTag tag{};
chacha20_poly1305_seal(key, nonce, aad, plaintext, ciphertext, tag);
auto r = chacha20_poly1305_open(key, nonce, aad, ciphertext, tag, plaintext_out);

// ── AES-GCM ──────────────────────────────────────────────────────
if (has_aes_ni()) {
    auto ctx = AesGcm256::create(aes_key).value();
    AesGcmTag tag{};
    ctx.seal(nonce, aad, plaintext, ciphertext, tag);
    ctx.open(nonce, aad, ciphertext, tag, plaintext_out);
}

// ── CSPRNG ───────────────────────────────────────────────────────
auto bytes = random_bytes<32>().value();          // 256-bit key
hw_random_fill(buf);                              // RDRAND + fallback
```

---

## 6. Algorithm Selection Guide

| Use case | API | Notes |
|----------|-----|-------|
| Password storage | `pbkdf2_hmac_sha256<32>(pw, salt, 600'000)` | Slow by design |
| Session key from ECDH | `hkdf_extract_sha256` → `hkdf_expand_sha256` | RFC 5869 |
| API request signing | `hmac_sha256` + `verify_hmac_sha256` | Constant-time |
| Symmetric encryption (portable) | `chacha20_poly1305_seal/open` | Always safe |
| Symmetric encryption (performance) | `AesGcm256` + ChaCha20 fallback | ~5× faster |
| Session token generation | `random_bytes<32>` + `base64url_encode` | 256-bit entropy |
| Content addressing / dedup | `sha256` | Fast, SIMD-accelerated |
| JWT signature | `hmac_sha256` or `base64url_encode(sig)` | HS256 pattern |

---

## 7. What Is Intentionally Excluded

| Algorithm | Reason |
|-----------|--------|
| Software AES | Table lookups create cache-timing side channels |
| TLS handshake / X.509 | Requires certificate store, complex state machine |
| X25519 / ECDH | Constant-time Montgomery multiplication is complex to implement safely |
| RSA / ECDSA | Requires arbitrary-precision arithmetic |

Use the optional `qbuem-stack::tls` component (wraps OpenSSL/mbedTLS) for
TLS handshakes and certificate-based authentication.

---

## 8. Testing

Tests live in `tests/crypto_primitives_test.cpp` and cover:

- **Known-answer tests** (KAT): SHA-256/224, SHA-512/384, HMAC-SHA-256 against NIST FIPS 180-4 and RFC 4231 vectors; HKDF against RFC 5869 Appendix A.1; Base64 against RFC 4648 §10.
- **Streaming vs one-shot equality** for SHA-256, SHA-512, HMAC.
- **Round-trip correctness** for Base64, Base64url, ChaCha20-Poly1305, AES-GCM.
- **Tamper detection** for ChaCha20-Poly1305 (modified ciphertext, tag, and AAD) and AES-GCM.
- **PBKDF2 determinism** — same inputs yield the same derived key.
- **HKDF RFC 5869 A.1** — exact extract PRK and expand OKM values verified.
- **CSPRNG sanity** — output is non-zero and consecutive calls differ.

Build and run with:
```bash
cmake -B build && cmake --build build --target qbuem_crypto_tests
cd build && ctest -R qbuem_crypto_tests --output-on-failure
```

---

## 9. Example

See `examples/04-codec-security/crypto_primitives/` for a runnable walkthrough
of all APIs from hashing through AEAD encryption.
