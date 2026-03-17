# crypto_url

**Category:** Codec & Security
**File:** `crypto_url_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates all cryptographic primitives and URL encoding/decoding utilities in qbuem-stack — with **zero external dependencies**. Covers CSPRNG, hardware entropy (RDRAND/RDSEED), CSRF token generation, and constant-time string comparison.

## Scenario

A web application needs to generate CSRF tokens for form submissions, produce cryptographically random session identifiers, compare authentication tokens in a side-channel-safe way, and encode/decode URL parameters including UTF-8 text.

## Architecture Diagram

```
  URL Encoding / Decoding
  ──────────────────────────────────────────────────────────
  Raw string  ──► url_encode() ──► percent-encoded string
  Encoded     ──► url_decode() ──► original string
  Round-trip: encode → decode == original  ✓

  CSPRNG (Cryptographically Secure PRNG)
  ──────────────────────────────────────────────────────────
  random_fill(buf, size)
  └─ reads from /dev/urandom (or getrandom syscall on Linux)
  └─ output: unpredictable byte sequence

  Hardware Entropy
  ──────────────────────────────────────────────────────────
  has_rdrand() → CPU feature detection
  rdrand64()   → RDRAND instruction (x86_64)
  hw_entropy_fill() → RDRAND or getrandom fallback
  hw_seed_fill()    → RDSEED or getrandom fallback

  Token Generation
  ──────────────────────────────────────────────────────────
  csrf_token(bits)    → URL-safe Base64 string
  random_bytes(n)     → raw random byte string

  Timing-Safe Comparison
  ──────────────────────────────────────────────────────────
  constant_time_equal(a, b)
  └─ compares all bytes regardless of first mismatch
  └─ prevents timing side-channel attacks
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `url_encode(str)` | Percent-encode a raw string |
| `url_decode(str)` | Percent-decode (handles `+` → space) |
| `random_fill(buf, size)` | Fill buffer with CSPRNG bytes |
| `hw_entropy_fill(buf, size)` | Fill with hardware entropy (RDRAND fallback) |
| `hw_seed_fill(buf, size)` | Fill with seeding entropy (RDSEED fallback) |
| `has_rdrand()` / `has_rdseed()` | CPU feature detection |
| `rdrand64(out)` | Single RDRAND call |
| `csrf_token(bits)` | URL-safe Base64 CSRF token |
| `random_bytes(n)` | Raw random byte string |
| `constant_time_equal(a, b)` | Timing-safe string comparison |

## Input / Output

### URL Encoding

| Input | Expected Output |
|-------|----------------|
| `"hello world!"` | `"hello%20world%21"` |
| `"q=foo+bar"` | decoded: `"q=foo bar"` |
| `"가나"` (UTF-8) | `"%EA%B0%80%EB%82%98"` |
| `"safe-._~"` | `"safe-._~"` (unreserved chars unchanged) |

### Security Tokens

| Operation | Output Example |
|-----------|----------------|
| `csrf_token(256)` | `"aB3x..."` (43 chars, URL-safe Base64) |
| `random_bytes(16)` → hex | 32 hex chars |
| `constant_time_equal(a, a)` | `true` |
| `constant_time_equal(a, b)` | `false` |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target crypto_url_example
./build/examples/04-codec-security/crypto_url/crypto_url_example
```

## Expected Output

```
=== qbuem Crypto + URL Utility Example ===

── §1  URL encoding/decoding ──
  decode("hello%20world%21") = hello world! ✓
  decode("q=foo+bar")        = q=foo bar ✓
  ...
  round-trip: encode→decode equal: yes

── §2  CSPRNG random_fill ──
  random[0]: a3f2...   (16 hex bytes)
  random[1]: 9c7b...   (different)
  two values different: yes

── §3  Hardware Entropy ──
  RDRAND supported: yes/no
  hw_entropy(32B): 4b2e...

── §4  Secure Token Generation ──
  CSRF token 1: aB3x...
  CSRF tokens different: yes

── §5  Timing-safe comparison ──
  a == b: true
  a == c: false
  a == d: false (different length)

=== Done ===
```

## Notes

- `csrf_token(256)` produces 256 bits of entropy encoded as URL-safe Base64 (43 characters).
- `constant_time_equal` always compares all bytes up to `min(len_a, len_b)` to prevent timing oracles; returns `false` immediately when lengths differ.
- On systems without RDRAND/RDSEED, the implementation silently falls back to `getrandom(2)`.
