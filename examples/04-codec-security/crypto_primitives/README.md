# crypto_primitives

**Category:** Codec & Security
**File:** `crypto_primitives_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates all zero-dependency cryptographic primitives in `include/qbuem/crypto/` — hashing, message authentication, key derivation, Base64 encoding, and authenticated encryption — with **no external dependencies** (C++23 stdlib only).

## Scenario

A backend service needs to:
1. Hash request payloads for deduplication
2. Generate and verify HMAC-SHA-256 signatures on API tokens
3. Derive per-session encryption keys from a shared ECDH secret using HKDF
4. Hash user passwords for storage using PBKDF2
5. Encrypt sensitive data with ChaCha20-Poly1305 (always available) or AES-256-GCM (hardware)
6. Encode binary tokens as URL-safe Base64 for cookies and headers

## Architecture

```
qbuem::crypto namespace
──────────────────────────────────────────────────────────────
Layer 1: Hashing
  sha256() / sha512()      → std::array<uint8_t, 32/64>
  Sha256Context::update()  → streaming interface
  sha224() / sha384()      → truncated variants

Layer 1: Message Authentication
  hmac_sha256(key, msg)    → Sha256Digest (32 bytes)
  verify_hmac_sha256(...)  → bool (constant-time)
  HmacSha256{key}          → streaming; reset/reuse

Layer 1: Key Derivation
  pbkdf2_hmac_sha256<N>()  → std::array<uint8_t, N>
  hkdf_extract_sha256()    → PRK (Sha256Digest)
  hkdf_expand_sha256()     → writes OKM to span<uint8_t>

Layer 2: Encoding
  base64_encode()          → std::string (with '=' padding)
  base64url_encode()       → std::string (no padding)
  base64_decode()          → Result<std::string>

Layer 3: AEAD
  chacha20_poly1305_seal() → AeadTag (always available)
  chacha20_poly1305_open() → Result<void>
  AesGcm256::create()      → Result<AesGcm256> (AES-NI required)
  AesGcm256::seal/open()

Layer 0: CSPRNG
  random_bytes<N>()        → Result<std::array<uint8_t, N>>
  hw_random_fill()         → RDRAND or kernel fallback
  hw_seed_fill()           → RDSEED or kernel fallback
```

## Key APIs

| API | Standard | Output | Notes |
|-----|----------|--------|-------|
| `sha256(data)` | FIPS 180-4 | 32 bytes | SHA-NI / ARM SHA2 / scalar |
| `sha512(data)` | FIPS 180-4 | 64 bytes | ARM SHA-512 / scalar |
| `hmac_sha256(key, msg)` | RFC 2104 | 32 bytes | constant-time verify available |
| `pbkdf2_hmac_sha256<N>(pw, salt, iter)` | RFC 2898 | N bytes | OWASP: ≥ 600 000 iterations |
| `hkdf_extract_sha256(salt, ikm)` | RFC 5869 | PRK (32 B) | condense entropy |
| `hkdf_expand_sha256(prk, info, out)` | RFC 5869 | out.size() bytes | sub-key derivation |
| `base64_encode(data)` | RFC 4648 §4 | string | with '=' padding |
| `base64url_encode(data)` | RFC 4648 §5 | string | no '+' '/' '=' |
| `chacha20_poly1305_seal(key, nonce, aad, pt, ct, tag)` | RFC 8439 | 16-byte tag | constant-time, no AES-NI needed |
| `AesGcm256::create(key)` | NIST SP 800-38D | context | requires AES-NI |
| `random_bytes<N>()` | POSIX / x86 | N bytes | getrandom/RDRAND |

## How to Build and Run

```bash
# Configure with Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build only this example
cmake --build build --target crypto_primitives_example

# Run
./build/examples/04-codec-security/crypto_primitives/crypto_primitives_example
```

## Expected Output

```
=== qbuem Cryptographic Primitives Example ===

-- §1  SHA-256 / SHA-512 --
  SHA-256("hello world") = b94d27b9934d3e08a52e52d7da7dabfac484efe04294e576...
  SHA-512("hello world") = 309ecc489c12d6eb4cc40f50c902f2b4d0ed77ee511a7c7...
  streaming == one-shot: yes
  SHA-224("abc") = 23097d223405d8228642a477bda255b32aadbce4bda0b3f7...
  SHA-384("abc") = cb00753f45a35e8bb5a03d699ac65007272c32ab0eded163...

-- §2  HMAC-SHA-256 / HMAC-SHA-512 --
  HMAC-SHA-256 tag = <32-byte hex>
  verify (correct): OK
  verify (tampered): correctly rejected
  streaming == one-shot: yes

-- §3  PBKDF2-HMAC-SHA-256 (password hashing) --
  salt     = <random 16-byte hex>
  dk[32B]  = <derived key hex>
  deterministic: yes

-- §4  HKDF-SHA-256 (session key derivation) --
  PRK      = <32-byte PRK hex>
  enc_key  = <32-byte enc key hex>
  auth_key = <32-byte auth key hex>
  keys differ: yes

-- §5  Base64 / Base64url encoding --
  base64("f")      = Zg==   OK
  base64("foobar") = Zm9vYmFy OK
  binary round-trip: yes
  base64url(random 16B) = <22-char url-safe string> (no +/= chars: yes)

-- §6  ChaCha20-Poly1305 AEAD --
  plaintext  = "Confidential: Transfer $1,000,000 to Alice"
  ciphertext = <hex>...
  poly1305   = <16-byte tag>
  decrypted  = "Confidential: Transfer $1,000,000 to Alice"
  round-trip: yes
  tamper detection: OK (forgery rejected)

-- §7  AES-256-GCM (hardware AEAD) --
  (skipped if AES-NI not available — ChaCha20-Poly1305 used as fallback)

-- §8  CSPRNG & Hardware Entropy --
  RDRAND available: yes/no
  RDSEED available: yes/no
  random_bytes<32>  = <hex>...
  hw_random<32>     = <hex>...
  session token     = <43-char Base64url string> (43 chars)

=== Done ===
```

## Security Notes

### Algorithm Selection Guide

| Use case | Recommended API | Reason |
|----------|----------------|--------|
| Password storage | `pbkdf2_hmac_sha256<32>(pw, salt, 600'000)` | Slow by design; OWASP 2023 minimum |
| Session key derivation | `hkdf_sha256` | Fast; takes arbitrary IKM |
| Symmetric encryption | `chacha20_poly1305_seal` | Always safe; constant-time |
| Symmetric encryption (HW) | `AesGcm256` with ChaCha20 fallback | ~5× faster when AES-NI available |
| API request signing | `hmac_sha256` + `verify_hmac_sha256` | Timing-safe; RFC 2104 |
| Session token generation | `random_bytes<32>` + `base64url_encode` | 256-bit entropy |

### Nonce Management (ChaCha20-Poly1305 / AES-GCM)

A 12-byte nonce **must never be reused** with the same key. Recommended strategies:
- Random nonce (96-bit): call `random_bytes<12>()` per message
- Counter nonce: prefix 4 fixed bytes + 8-byte message counter (monotonic)

### Zero-Allocation Design

All functions operate on caller-provided `std::span<uint8_t>` or return
`std::array<uint8_t, N>` — no heap allocation occurs in any crypto hot path.
