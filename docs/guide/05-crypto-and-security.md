# Cryptography & Security

The `qbuem::crypto` module (`include/qbuem/crypto/`) is a self-contained, zero-dependency cryptographic toolkit written in pure C++23 plus per-arch intrinsics. It gives you hashing, MACs, key derivation, AEAD encryption, encoding, and a CSPRNG without pulling in OpenSSL, libsodium, mbedTLS, or any other library. The `qbuem::security` module (`include/qbuem/security/`) builds on top of it with a SIMD JWT parser and a pipeline-stage JWT auth action.

Everything here obeys the stack's four pillars:

* **Zero allocation** — every primitive keeps its state in a fixed-size struct (`std::array`, plain members). One-shot helpers return `std::array<uint8_t, N>`; streaming helpers write into caller-provided spans. The only allocating functions are the convenience `base64*_encode`/`*_decode` overloads that return `std::string` (a non-span-output overload exists for the hot path).
* **Zero copy** — inputs are `std::span<const uint8_t>` / `std::string_view`; the JWT parser returns views into the original token buffer.
* **Zero dependency** — only `<array>`, `<bit>`, `<cstdint>`, `<span>`, `<expected>`, `<system_error>`, etc., plus `<immintrin.h>` / `<arm_neon.h>` / kernel headers.
* **No exceptions** — fallible operations return `Result<T>` (`= std::expected<T, std::error_code>`). You check with `if (!r)` and read `r.error()`. The crypto primitives' Result alias lives in the headers that need it (`random.hpp`, `base64.hpp`, `chacha20_poly1305.hpp`, `aes_gcm.hpp`).

> Include `#include <qbuem/crypto/crypto.hpp>` to pull in the whole module, or include individual headers (e.g. `#include <qbuem/crypto/sha256.hpp>`) for tighter compile times. All symbols live in namespace `qbuem::crypto`.

> **Runnable reference:** `examples/04-codec-security/crypto_primitives/crypto_primitives_example.cpp` exercises SHA, HMAC, PBKDF2, HKDF, Base64, ChaCha20-Poly1305, AES-256-GCM, and the CSPRNG end to end. Build with the `crypto_primitives` example target.

---

## Platform & SIMD reality (read this first)

Performance characteristics differ sharply between architectures. The table below states what is **actually** accelerated today versus what falls back to scalar code. Correctness is identical on every path — only throughput changes.

| Algorithm | x86-64 (Linux) | ARM64 / aarch64 (Jetson, Mac) | Notes |
|---|---|---|---|
| **SHA-256 / SHA-224** | SHA-NI when compiled with `-msha` (`__SHA__`) | ARMv8 SHA2 when `__ARM_FEATURE_SHA2` (e.g. `-march=armv8-a+crypto`) | Both are real hardware paths. Otherwise scalar (~200–400 MB/s). |
| **SHA-512 family** | Scalar only (no x86 SHA-512 NI) | **Scalar** — the ARM SHA-512 path falls back to scalar internally (`compress_block` always calls `compress_scalar`) | Correct everywhere; not hardware-accelerated. |
| **ChaCha20 / -Poly1305** | Scalar (AVX2 path is a stub) | **NEON real** — 4 blocks (256 B) per call when `__ARM_NEON` | Always constant-time regardless of path. |
| **Poly1305** | Scalar (constant-time) | Scalar (constant-time) | 5×26-bit limb arithmetic; no `__uint128_t` dependency. |
| **AES-GCM** | AES-NI + PCLMUL **CTR**, **portable scalar GHASH** | ARM AES (`AESE`/`AESMC`) + PMULL **CTR**, **portable scalar GHASH** | Returns `function_not_supported` if no hardware AES. See AES-GCM section. |
| **Base64 encode** | Scalar (AVX2 path is a stub that returns 0) | **NEON real** (`vqtbl1q_u8`) | Decode is scalar on all platforms. |
| **CSPRNG** | RDRAND (when present) → `getrandom(2)` | `getrandom(2)` (Linux) / `arc4random_buf` (Mac) | RDRAND/RDSEED are x86-only. |

**Enabling the hardware paths.** The default Release flags do not set the SHA-2/
AES feature macros on every toolchain (notably Apple clang's `-march=native` does
*not* define `__ARM_FEATURE_SHA2`), so SHA-256/HMAC fall back to scalar (~192 MB/s
measured). Configure with `-DQBUEM_ENABLE_NATIVE_CRYPTO=ON` to turn the host
hardware paths on: SHA-256/HMAC then run ~2.1 GB/s (~11×), KAT-verified. It is
OFF by default because it makes the binary require those CPU features
(host-targeting build). AES-GCM's GHASH stays scalar regardless (see below), so
AES-256-GCM remains ~14 MB/s; prefer ChaCha20-Poly1305 (NEON, ~640 MB/s) as the
fast AEAD on ARM.

Honest caveats baked into the headers themselves:

* The **AVX2 ChaCha20** and **AVX2 Base64 encode** paths are present but deliberately fall through to scalar (`encode_avx2` returns 0 bytes processed). On x86 you get correct, scalar-speed output. NEON is the genuinely vectorized path.
* **SHA-512** is scalar on both architectures despite the `QBUEM_SHA512_ARM` guard — `detail::sha512::compress_block` unconditionally calls `compress_scalar`. Treat SHA-512/384/512-256/512-224 as correct but not hardware-accelerated.
* The **GHASH** in AES-GCM uses a portable, constant-time `gf_mul128` (bit-by-bit, masked) on **both** x86 and ARM. The PCLMUL/PMULL `ghash_mul` functions exist in the header but the AES-GCM context (`AesGcm<KeyBytes>`) routes through `gf_mul128` so that tags are spec-correct (NIST SP 800-38D) and interoperable. AES *block* encryption is hardware-accelerated; only the GF(2^128) multiply is scalar.

---

## Layer 1 — Hashing: SHA-256 and SHA-512 families

### `Sha256Context` / `sha256()` / `sha224()`

**What it is.** FIPS 180-4 SHA-256 and its truncated variant SHA-224. Provides a streaming context plus one-shot free functions. Header: `<qbuem/crypto/sha256.hpp>`.

**When to use it.** Content hashing, building HMAC (it backs `HmacSha256`), deriving cache keys, integrity checks. Use the **streaming** context when the data arrives in chunks (network frames, file blocks) so you never buffer the whole message; use the **one-shot** `sha256(...)` when you already have a contiguous span.
**When NOT to.** Do not use a bare hash for password storage (use `pbkdf2_hmac_sha256`) or for message authentication (use HMAC — a raw hash is vulnerable to length-extension).

**Types & API.**

| Symbol | Signature | Returns |
|---|---|---|
| `Sha256Digest` | `using = std::array<uint8_t, 32>` | — |
| `Sha224Digest` | `using = std::array<uint8_t, 28>` | — |
| `Sha256Context()` | default ctor → SHA-256 | — |
| `Sha256Context(Variant)` | `Variant::SHA256` \| `Variant::SHA224` | — |
| `.update(std::span<const uint8_t>)` / `.update(std::string_view)` | feed data | `void` |
| `.finalize() const` | compute digest (context preserved — see gotcha) | `Sha256Digest` |
| `.finalize_224() const` | for SHA-224 contexts | `Sha224Digest` |
| `.reset()` | back to initial state | `void` |
| `sha256(span)` / `sha256(string_view)` | one-shot | `Sha256Digest` |
| `sha224(span)` / `sha224(string_view)` | one-shot | `Sha224Digest` |

```cpp
#include <qbuem/crypto/sha256.hpp>
using namespace qbuem::crypto;

// One-shot
Sha256Digest d = sha256(std::string_view("hello world"));

// Streaming — never holds the whole message in memory
Sha256Context ctx;
ctx.update({reinterpret_cast<const uint8_t*>("hello"), 5});
ctx.update({reinterpret_cast<const uint8_t*>(" world"), 6});
Sha256Digest streamed = ctx.finalize();
// streamed == d
```

**Gotchas.**
* `finalize()` is `const` and works on an internal **copy** before padding, so the context stays usable — you can call `update()` again or `finalize()` a second time and get the same digest. This is intentional; it does *not* mutate `state_`.
* `Sha256Context` is `alignas(64)` and 128 bytes — fits in two cache lines, no heap.
* `finalize_224()` is only meaningful if you constructed with `Variant::SHA224`. The one-shot `sha224()` handles this for you.

### `Sha512Context` and the SHA-512 family

**What it is.** FIPS 180-4 SHA-512, SHA-384, SHA-512/256, SHA-512/224 — all driven by one `Sha512Context` parameterized by a `Variant` enum. Header: `<qbuem/crypto/sha512.hpp>`.

**When to use it.** When you need a 512-bit hash, a 384-bit hash (e.g. for an HMAC-SHA-384-shaped protocol via `HmacSha512`-style construction), or the FIPS-truncated 512/256 variant for 64-bit-word performance on machines where 64-bit ops are cheaper than 32-bit-heavy SHA-256. On this stack SHA-512 is **scalar**, so for raw speed on hot paths prefer SHA-256 (which has a real SHA-NI/SHA2 path).

| Variant | Output type | Size | Finalizer |
|---|---|---|---|
| `Variant::SHA512` | `Sha512Digest` | 64 B | `finalize()` |
| `Variant::SHA384` | `Sha384Digest` | 48 B | `finalize_384()` |
| `Variant::SHA512_256` | `Sha512_256Digest` | 32 B | `finalize_512_256()` |
| `Variant::SHA512_224` | `Sha512_224Digest` | 28 B | `finalize_512_224()` |

One-shot helpers: `sha512(...)`, `sha384(...)`, `sha512_256(...)`, `sha512_224(...)` — each accepts a `std::span<const uint8_t>` or `std::string_view`.

```cpp
#include <qbuem/crypto/sha512.hpp>
using namespace qbuem::crypto;

Sha512Digest a = sha512(std::string_view("hello world"));   // 64 bytes

Sha512Context ctx{Sha512Context::Variant::SHA384};
ctx.update(std::string_view("abc"));
Sha384Digest b = ctx.finalize_384();                        // 48 bytes
```

**Gotcha.** Like SHA-256, `finalize*()` works on a copy and leaves the context usable. Block size is 128 bytes; the context buffers up to a full 128-byte block internally.

---

## Layer 1 — HMAC (RFC 2104)

### `HmacSha256` / `HmacSha512`, one-shot `hmac_sha256` / `hmac_sha512`, constant-time verify

**What it is.** Keyed message authentication built generically over any type satisfying the `HashContext` concept. Two ready aliases: `HmacSha256 = HmacContext<Sha256Wrapper>` and `HmacSha512 = HmacContext<Sha512Wrapper>`. Header: `<qbuem/crypto/hmac.hpp>`.

**When to use it.** Authenticating API requests/webhooks, signing/verifying cookies and opaque tokens, deriving HKDF/PBKDF2 (both build on HMAC internally), or implementing a JWT HS256 `ITokenVerifier`. The context **pre-computes the ipad/opad key blocks once** so re-MACing many messages under the same key skips key processing — ideal for a hot verification loop.
**When NOT to.** For password storage use PBKDF2; for full confidentiality+integrity use an AEAD (ChaCha20-Poly1305 / AES-GCM).

**API.**

| Symbol | Signature | Returns |
|---|---|---|
| `HmacSha256{key}` | ctor from `std::span<const uint8_t>` or `std::string_view` | — |
| `.update(span)` / `.update(string_view)` | feed message | `void` |
| `.finalize() const` | tag (context reusable via `reset()`) | `Sha256Digest` (32 B) |
| `.compute(msg)` | `reset(); update(msg); finalize()` | `DigestType` |
| `.reset()` | re-key inner hash with cached ipad (no key reprocessing) | `void` |
| `.wipe()` | optimizer-proof zero of ipad/opad | `void` |
| `hmac_sha256(key, message)` | one-shot (span or string_view args) | `Sha256Digest` |
| `hmac_sha512(key, message)` | one-shot | `Sha512Digest` (64 B) |
| `verify_hmac_sha256(key, msg, expected&)` | constant-time compare | `bool` |
| `verify_hmac_sha512(key, msg, expected&)` | constant-time compare | `bool` |

```cpp
#include <qbuem/crypto/hmac.hpp>
using namespace qbuem::crypto;

std::string_view key = "my-secret-key";
std::string_view msg = "payload=hello&user=alice";

Sha256Digest tag = hmac_sha256(key, msg);              // one-shot

bool ok = verify_hmac_sha256(
    {reinterpret_cast<const uint8_t*>(key.data()), key.size()},
    {reinterpret_cast<const uint8_t*>(msg.data()), msg.size()},
    tag);                                              // constant-time

// Streaming + reuse for a verification loop
HmacSha256 h{key};
h.update(std::string_view("payload=hello"));
h.update(std::string_view("&user=alice"));
Sha256Digest streamed = h.finalize();                  // == tag
h.reset();                                             // re-key cheaply for the next message
```

**Gotchas.**
* **Always** verify tags with `verify_hmac_sha256` / `verify_hmac_sha512` (or the top-level `qbuem::constant_time_equal`), never `==` or `memcmp` — the provided verifiers fold the comparison through a `volatile` accumulator so they don't early-exit and leak timing.
* The `HashContext` concept (`kBlockSize`, `kDigestSize`, `DigestType`, `reset/update/finalize`) lets you build `HmacContext<YourWrapper>` over any conforming hash, but the two shipped wrappers (`Sha256Wrapper`, `Sha512Wrapper`) cover the common cases.
* Call `.wipe()` when discarding a long-lived keyed context that held a high-value key.

---

## Layer 1 — Key derivation

### PBKDF2 (RFC 2898) — `pbkdf2_hmac_sha256` / `pbkdf2_hmac_sha512`

**What it is.** Password-stretching KDF that iterates HMAC. Two PRFs: HMAC-SHA-256 and HMAC-SHA-512. Header: `<qbuem/crypto/pbkdf2.hpp>`.

**When to use it.** Deriving an encryption key or a verifier from a low-entropy human password. The iteration count is your security/latency dial.
**When NOT to.** For deriving sub-keys from an already-high-entropy secret (e.g. an ECDH output or a master key), use **HKDF** instead — it is far cheaper because it does not stretch. PBKDF2 is intentionally slow.

Two output styles for each PRF:
* Span output (zero-alloc): `void pbkdf2_hmac_sha256(password, salt, iterations, std::span<uint8_t> out)` — derives `out.size()` bytes.
* Template return: `std::array<uint8_t, N> pbkdf2_hmac_sha256<N>(password, salt, iterations)`.

Both accept `password`/`salt` as `std::span<const uint8_t>` or `std::string_view`. `iterations` is `uint32_t`.

```cpp
#include <qbuem/crypto/pbkdf2.hpp>
#include <qbuem/crypto/random.hpp>
using namespace qbuem::crypto;

auto salt_r = random_bytes<16>();             // 128-bit salt
if (!salt_r) return /* handle salt_r.error() */;

// Derive a 32-byte key. OWASP 2023 minimum for SHA-256 is 600,000 iterations.
std::array<uint8_t, 32> dk =
    pbkdf2_hmac_sha256<32>(std::string_view("correct horse battery staple"),
                           std::span<const uint8_t>{*salt_r},
                           600'000);
```

**Gotchas.**
* Recommended iterations: **600,000** for SHA-256, **210,000** for SHA-512 (SHA-512 is ~2× slower per iteration). The example uses 100,000 only for demo speed — do not ship that.
* Use a fresh random 16-byte salt per password and store it alongside the derived key.
* Cost scales linearly with `iterations`; on a 4 GHz CPU with SHA-NI, 600k iterations ≈ 30 ms/key. On scalar SHA-512 (this stack) PBKDF2-SHA-512 is correspondingly slower — benchmark on your target board.

### HKDF (RFC 5869) — `hkdf_extract_*`, `hkdf_expand_*`, `hkdf_sha256` / `hkdf_sha512`

**What it is.** Extract-then-Expand KDF for turning a high-entropy secret into one or more sub-keys. Header: `<qbuem/crypto/hkdf.hpp>`.

**When to use it.** Deriving distinct session keys (encryption key, auth key, IV seed) from a single shared secret — each with a different `info` label — or condensing a non-uniform secret into a uniform PRK. Cheap and fast (a handful of HMAC calls).
**When NOT to.** Stretching a password (use PBKDF2). HKDF provides no work factor.

| Function | Purpose | Returns / output |
|---|---|---|
| `hkdf_extract_sha256(salt, ikm)` | Extract → PRK | `Sha256Digest` |
| `hkdf_extract_sha512(salt, ikm)` | Extract → PRK | `Sha512Digest` |
| `hkdf_expand_sha256(prk, info, std::span<uint8_t> out)` | Expand into caller buffer | `void` |
| `hkdf_expand_sha512(prk, info, out)` | Expand | `void` |
| `hkdf_sha256(ikm, salt, info, out)` | Full extract+expand into buffer | `void` |
| `hkdf_sha256<N>(ikm, salt, info)` | Full, returns `std::array<uint8_t, N>` | array |
| `hkdf_sha512(...)` / `hkdf_sha512<N>(...)` | SHA-512 variants | as above |

Span overloads take `std::span<const uint8_t>`; convenience string-view overloads exist for `hkdf_sha256(string_view ikm, salt, info, out)` and the templated `hkdf_sha256<N>(string_view…)`.

```cpp
#include <qbuem/crypto/hkdf.hpp>
using namespace qbuem::crypto;

// shared_secret e.g. from an ECDH exchange (32 bytes), plus a salt nonce.
std::array<uint8_t,32> shared_secret = /* ... */;
std::array<uint8_t,32> salt          = /* ... */;

// Derive two independent 32-byte keys with different context labels.
auto enc_key  = hkdf_sha256<32>(std::span<const uint8_t>{shared_secret},
                                std::span<const uint8_t>{salt},
                                std::string_view("enc_key v1"));
auto auth_key = hkdf_sha256<32>(std::span<const uint8_t>{shared_secret},
                                std::span<const uint8_t>{salt},
                                std::string_view("auth_key v1"));
// enc_key != auth_key — distinct labels => distinct keys.

// Or extract once and expand multiple times:
Sha256Digest prk = hkdf_extract_sha256(salt, shared_secret);
std::array<uint8_t,32> k1{}, k2{};
hkdf_expand_sha256(prk, std::string_view("k1") , k1);
hkdf_expand_sha256(prk, std::string_view("k2"),  k2);
```

**Gotchas.**
* Output length is capped at `255 × HashLen` (RFC 5869) — 8160 B for SHA-256, 16320 B for SHA-512.
* An empty `salt` is replaced by a zero block of HashLen bytes (per spec). Prefer a real, fixed salt for domain separation.
* Always vary `info` between keys derived from the same PRK; that label is what makes the outputs independent.

---

## Layer 2 — Stream cipher + MAC (constant-time ARX)

These are constant-time **by design** — no S-box tables, no secret-dependent branches — so they are the safe choice on any CPU, including those without hardware AES.

### ChaCha20 (RFC 8439) — `ChaCha20`, `chacha20_xor`

**What it is.** 256-bit-key, 96-bit-nonce stream cipher. NEON-accelerated (4 blocks/call) on ARM, scalar on x86. Header: `<qbuem/crypto/chacha20.hpp>`.

**When to use it.** Raw stream encryption where you provide your own authentication, or as a keystream generator. **In almost all cases prefer the AEAD wrapper `chacha20_poly1305_*`** rather than raw ChaCha20 — unauthenticated encryption is malleable.

| Type | Definition |
|---|---|
| `ChaCha20Key` | `std::array<uint8_t, 32>` |
| `ChaCha20Nonce` | `std::array<uint8_t, 12>` |
| `ChaCha20Block` | `std::array<uint8_t, 64>` |

`ChaCha20` context: `ChaCha20(key, nonce, ctr=0)`, `.seek(block_counter)`, `.xor_into(std::span<uint8_t> data)` (in-place), `.xor_into(src, dst)`, `.keystream_block(block_counter) -> ChaCha20Block`, `.wipe()`.
Free functions: `chacha20_xor(key, nonce, counter, std::span<uint8_t> data)` (in-place) and `chacha20_xor(key, nonce, counter, src, dst)`.

```cpp
#include <qbuem/crypto/chacha20.hpp>
using namespace qbuem::crypto;

ChaCha20Key   key   = /* ... 32 bytes ... */;
ChaCha20Nonce nonce = {};                   // 12 bytes, unique per message
std::array<uint8_t, 11> data{ /* plaintext */ };

chacha20_xor(key, nonce, /*counter=*/0, std::span<uint8_t>{data});  // encrypt in place
chacha20_xor(key, nonce, /*counter=*/0, std::span<uint8_t>{data});  // XOR again => decrypt
```

**Gotchas.**
* **Never reuse a (key, nonce) pair.** Reuse leaks the XOR of plaintexts.
* The 32-bit block counter limits one (key,nonce) to 256 GiB of keystream.
* AEAD encryption starts at block counter **1** (block 0 is reserved for the Poly1305 one-time key). The raw `chacha20_xor` defaults to counter 0; only set 1 when hand-rolling AEAD.
* `.xor_into()` advances and buffers internal keystream state for true streaming; `chacha20_xor` is stateless per call.

### Poly1305 (RFC 8439) — `Poly1305`, `poly1305`, `poly1305_verify`

**What it is.** One-time-key 128-bit MAC over GF(2^130−5), computed with 5×26-bit limbs and 64-bit products (no `__uint128_t`). Constant-time. Header: `<qbuem/crypto/poly1305.hpp>`.

**When to use it.** As the authentication half of an AEAD, or to authenticate a single message with a freshly derived one-time key. You will normally consume it **through** `chacha20_poly1305_*`, not directly.
**When NOT to.** Do not reuse the 32-byte key across messages — Poly1305 is a *one-time* MAC; key reuse enables forgery.

| Type | Definition |
|---|---|
| `Poly1305Key` | `std::array<uint8_t, 32>` |
| `Poly1305Tag` | `std::array<uint8_t, 16>` |

API: `Poly1305{key}` (array or span), `.update(span)`, `.finalize() -> Poly1305Tag` (invalidates context), `.wipe()`. One-shot `poly1305(key, message)` and constant-time `poly1305_verify(key, message, expected&)`.

```cpp
#include <qbuem/crypto/poly1305.hpp>
using namespace qbuem::crypto;

Poly1305Key otk = /* first 32 bytes of ChaCha20 keystream block 0 */;
Poly1305 mac{otk};
mac.update(aad);
mac.update(ciphertext);
Poly1305Tag tag = mac.finalize();          // context now invalid
```

**Gotcha.** `finalize()` is non-const and consumes the context — construct a new `Poly1305` for the next message (with a new one-time key).

### ChaCha20-Poly1305 AEAD (RFC 8439) — `chacha20_poly1305_seal` / `chacha20_poly1305_open`

**What it is.** The recommended general-purpose AEAD: ChaCha20 confidentiality + Poly1305 integrity + authenticated-but-unencrypted associated data (AAD). IND-CCA2 secure when the nonce is unique. Header: `<qbuem/crypto/chacha20_poly1305.hpp>`.

**When to use it.** Default symmetric encryption for messages, sessions, on-disk blobs — **especially on ARM boards and any CPU lacking AES-NI**, where it is both fast (NEON) and constant-time. Use it as the fallback whenever `AesGcm::create` returns `function_not_supported`.

| Type | Definition |
|---|---|
| `AeadKey` | `ChaCha20Key` (32 B) |
| `AeadNonce` | `ChaCha20Nonce` (12 B) |
| `AeadTag` | `Poly1305Tag` (16 B) |

Seal (encrypt + authenticate):
* `void chacha20_poly1305_seal(key, nonce, aad, plaintext, std::span<uint8_t> ciphertext_out, AeadTag& tag)` — `ciphertext_out.size()` must equal `plaintext.size()`.
* `AeadTag chacha20_poly1305_seal(key, nonce, aad, std::span<uint8_t> data)` — in-place; returns the tag.

Open (verify + decrypt), both return `Result<void>` (`std::expected<void, std::error_code>`):
* `chacha20_poly1305_open(key, nonce, aad, ciphertext, const AeadTag& tag, std::span<uint8_t> plaintext_out)`
* `chacha20_poly1305_open(key, nonce, aad, std::span<uint8_t> data, const AeadTag& tag)` — in-place.

```cpp
#include <qbuem/crypto/chacha20_poly1305.hpp>
#include <qbuem/crypto/random.hpp>
using namespace qbuem::crypto;

auto key_r   = random_bytes<32>();
auto nonce_r = random_bytes<12>();
if (!key_r || !nonce_r) return; // handle .error()

AeadKey key{};  AeadNonce nonce{};
std::copy(key_r->begin(),   key_r->end(),   key.begin());
std::copy(nonce_r->begin(), nonce_r->end(), nonce.begin());

std::string_view pt  = "Transfer $1,000,000 to Alice";
std::string_view aad = "transaction-id:abc123";    // authenticated, not encrypted

std::vector<uint8_t> ct(pt.size());
AeadTag tag{};
chacha20_poly1305_seal(
    key, nonce,
    {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
    {reinterpret_cast<const uint8_t*>(pt.data()),  pt.size()},
    {ct.data(), ct.size()}, tag);

std::vector<uint8_t> recovered(pt.size());
Result<void> ok = chacha20_poly1305_open(
    key, nonce,
    {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
    {ct.data(), ct.size()}, tag,
    {recovered.data(), recovered.size()});
if (!ok) {
    // authentication failed — ok.error() == std::errc::bad_message; DO NOT use `recovered`
    return;
}
```

**Gotchas.**
* **`open` verifies the tag *before* decrypting** (authenticate-then-decrypt). On failure it returns `std::unexpected(std::make_error_code(std::errc::bad_message))` and writes **nothing** trustworthy to `plaintext` — never use the output buffer when `open` fails.
* The tag is transmitted/stored separately from the ciphertext (it is an out-param / return value, not appended). You must persist all of: ciphertext, 16-byte tag, 12-byte nonce, and AAD.
* Unique nonce per (key, message) is mandatory. Either 96-bit random, or 32-bit fixed prefix ‖ 64-bit counter.
* Output spans are caller-allocated and must be exactly the input size; the AEAD does no allocation itself.

---

## Layer 3 — Hardware AES-GCM — `AesGcm128` / `AesGcm256`

**What it is.** AES-128-GCM and AES-256-GCM AEAD using **hardware AES only** — AES-NI+PCLMUL on x86-64, ARMv8 AES (`AESE`/`AESMC`)+PMULL on aarch64. There is **no software AES fallback** by design: table-based AES is cache-timing vulnerable, so the header refuses to do it. Standard: NIST SP 800-38D (GCM), FIPS 197 (AES). Header: `<qbuem/crypto/aes_gcm.hpp>`.

**When to use it.** When you must interoperate with a peer/protocol that mandates AES-GCM (TLS records, JWE `A256GCM`, an existing AES-GCM datastore), or when AES-NI makes it the fastest option on your x86 fleet.
**When NOT to.** If you control both ends and don't need AES interop, prefer **ChaCha20-Poly1305** — it is constant-time on every CPU and needs no hardware. On a board without hardware AES, `AesGcm` is simply unavailable.

| Type | Definition |
|---|---|
| `AesGcm128` | `AesGcm<16>` (128-bit key, 10 rounds) |
| `AesGcm256` | `AesGcm<32>` (256-bit key, 14 rounds) |
| `AesGcmNonce` | `std::array<uint8_t, 12>` |
| `AesGcmTag` | `std::array<uint8_t, 16>` |
| `AesGcm<KeyBytes>::KeyArray` | `std::array<uint8_t, KeyBytes>` |

API:
* `static Result<AesGcm> AesGcm<KeyBytes>::create(const KeyArray& key)` — **factory**; returns `std::unexpected(function_not_supported)` if no hardware AES, otherwise a context with the key schedule pre-expanded.
* `bool has_aes_ni()` — free function; check at runtime before choosing a cipher.
* `void seal(nonce, aad, plaintext, std::span<uint8_t> ciphertext_out, AesGcmTag& tag) const`
* `Result<void> open(nonce, aad, ciphertext, const AesGcmTag& tag, std::span<uint8_t> plaintext_out) const`
* `void wipe()` — zero the key schedule and hash subkey (only present on hardware-AES builds).

```cpp
#include <qbuem/crypto/aes_gcm.hpp>
#include <qbuem/crypto/chacha20_poly1305.hpp>
using namespace qbuem::crypto;

std::array<uint8_t, 32> key = /* 256-bit key */;

auto ctx_r = AesGcm256::create(key);
if (!ctx_r) {
    // ctx_r.error() == std::errc::function_not_supported on a CPU without AES.
    // Clean, documented fallback path:
    //   use chacha20_poly1305_seal / _open instead.
    return;
}
auto& ctx = *ctx_r;

AesGcmNonce nonce = /* unique 12 bytes */;
std::string_view pt  = "hardware-accelerated confidentiality";
std::string_view aad = "request-id:xyz789";

std::vector<uint8_t> ct(pt.size());
AesGcmTag tag{};
ctx.seal(nonce,
         {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
         {reinterpret_cast<const uint8_t*>(pt.data()),  pt.size()},
         {ct.data(), ct.size()}, tag);

std::vector<uint8_t> recovered(pt.size());
Result<void> ok = ctx.open(nonce,
    {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
    {ct.data(), ct.size()}, tag,
    {recovered.data(), recovered.size()});
if (!ok) return;   // ok.error() == std::errc::bad_message on auth failure
```

**Gotchas.**
* **Always go through `create()` and check the `Result`.** Never assume AES is present. The canonical pattern is: try `AesGcm256::create`; on `function_not_supported` fall back to `chacha20_poly1305_*`. `has_aes_ni()` lets you decide up front.
* `open()` does **constant-time tag comparison before decrypting**; on mismatch it returns `bad_message` and writes no usable plaintext.
* AES *block/CTR* is hardware-accelerated, but **GHASH uses the portable scalar `gf_mul128`** to guarantee spec-correct, interoperable tags (a previous SIMD GHASH produced self-consistent-but-non-spec tags; the NIST known-answer test now guards this). Expect AES-GCM throughput to be CTR-bound-plus-scalar-GHASH, not a fully vectorized GHASH.
* The construction uses a 96-bit nonce with J0 = nonce ‖ 0x00000001 and increments the 32-bit counter; the same nonce-uniqueness rules as any GCM apply (nonce reuse is catastrophic — it can leak the authentication key H).
* `wipe()` only exists when compiled on a hardware-AES target (it is `#if`-guarded), since the key schedule members only exist there.

---

## Encoding — Base64 / Base64url (RFC 4648)

### `base64_encode` / `base64_decode` / `base64url_encode` / `base64url_decode`

**What it is.** RFC 4648 §4 (standard, `+`/`/`, padded) and §5 (URL-safe, `-`/`_`, padding optional). NEON-accelerated encode on ARM; scalar elsewhere and for all decoding. Header: `<qbuem/crypto/base64.hpp>`.

**When to use it.** Transporting binary in text contexts — JWT segments, JSON fields, URLs (use `base64url`), config files. For the hot path use the **span-output** overloads to avoid the `std::string` allocation.

**Compile-time sizing helpers** (use these to size buffers for the zero-alloc path):

| Function | Meaning |
|---|---|
| `base64_encoded_size(n)` | padded output chars for `n` input bytes |
| `base64_encoded_size_nopad(n)` | unpadded output chars |
| `base64_decoded_max(b64_len)` | upper bound on decoded bytes |

**Encode API.**
* `std::string base64_encode(std::span<const uint8_t>)` / `(std::string_view)` — padded, allocates.
* `size_t base64_encode(std::span<const uint8_t> data, std::span<char> out)` — **zero-alloc**, returns chars written; `out` must be ≥ `base64_encoded_size(data.size())`.
* `std::string base64url_encode(std::span<const uint8_t>, bool padding = false)` / `(std::string_view, bool=false)` — URL-safe; **no padding by default** (JWT/URL convention).

**Decode API** (invalid input → `std::errc::illegal_byte_sequence`):
* `Result<std::string> base64_decode(std::string_view)` / `base64url_decode(std::string_view)` — accepts padded or unpadded input.
* `Result<size_t> base64_decode(std::string_view, std::span<uint8_t> out)` / `base64url_decode(...)` — zero-alloc; `out` ≥ `base64_decoded_max(encoded.size())`.

```cpp
#include <qbuem/crypto/base64.hpp>
using namespace qbuem::crypto;

// Allocating round-trip
std::string enc = base64_encode(std::string_view("foobar"));   // "Zm9vYmFy"
Result<std::string> dec = base64_decode(enc);
if (!dec) { /* dec.error() == std::errc::illegal_byte_sequence */ }

// URL-safe, unpadded (for JWT/URL)
std::array<uint8_t,16> token{};
std::string url = base64url_encode({token.data(), token.size()});  // no '+','/','='

// Zero-allocation encode into a stack buffer
std::array<uint8_t,32> raw{};
std::array<char, /*compile-time*/ 44> buf{};   // base64_encoded_size(32) == 44
size_t written = base64_encode(std::span<const uint8_t>{raw}, std::span<char>{buf});
```

**Gotchas.**
* `base64url_encode` omits padding unless you pass `padding=true`; `base64url_decode` accepts both forms.
* On x86 the AVX2 encode path is a stub (`encode_avx2` returns 0) so encoding runs scalar; the genuinely vectorized path is NEON. Decode is scalar everywhere.
* A lone trailing encoded character (tail length 1) is invalid Base64 and yields `illegal_byte_sequence`.
* The allocating overloads are the only functions in the module that touch the heap; keep them out of per-message hot paths in favor of the span overloads.

---

## CSPRNG & hardware entropy — `random.hpp`

### `random_fill` / `random_bytes<N>` / `hw_random_fill` / `hw_seed_fill` and RDRAND/RDSEED

**What it is.** Cryptographically secure randomness from the best available source per platform. Header: `<qbuem/crypto/random.hpp>`. (Note: this is the **span/array, Result-returning** API in `qbuem::crypto`. There is a separate, higher-level convenience API in the top-level `<qbuem/crypto.hpp>` under namespace `qbuem` — `random_bytes(size_t)`, `csrf_token`, `constant_time_equal` — used by the `crypto_url` example; don't confuse the two.)

| Platform | Kernel source |
|---|---|
| Linux ≥ 3.17 | `getrandom(2)` syscall (no fd, EINTR-retried) |
| macOS / BSD | `arc4random_buf()` |
| x86-64 with RDRAND | RDRAND in 8-byte chunks, falling back to the kernel |

**API** (all return `Result<...>`):

| Function | Signature | Notes |
|---|---|---|
| `random_fill(std::span<uint8_t>)` | `Result<void>` | kernel CSPRNG |
| `random_bytes<N>()` | `Result<std::array<uint8_t, N>>` | stack array, zero heap |
| `hw_random_fill(std::span<uint8_t>)` | `Result<void>` | RDRAND → kernel fallback |
| `hw_random_bytes<N>()` | `Result<std::array<uint8_t, N>>` | RDRAND-preferred |
| `hw_seed_fill(std::span<uint8_t>)` | `Result<void>` | RDSEED → RDRAND → kernel |
| `has_rdrand()` / `has_rdseed()` | `bool` | cached CPUID check (x86 only; false on ARM) |
| `rdrand64(uint64_t&)` / `rdseed64(uint64_t&)` | `bool` | single 64-bit draw, 10 retries |

```cpp
#include <qbuem/crypto/random.hpp>
using namespace qbuem::crypto;

// Idiomatic key/nonce generation — checked Result.
auto key = random_bytes<32>();           // Result<std::array<uint8_t,32>>
if (!key) {
    // key.error() carries errno/system_category on getrandom failure
    return;
}
const std::array<uint8_t,32>& k = *key;

// Fill an existing buffer
std::array<uint8_t, 16> salt{};
if (auto r = random_fill(salt); !r) { /* r.error() */ }
```

**Gotchas.**
* `random_fill`/`random_bytes` only fail if the kernel entropy call itself fails (rare after boot) — but you must still check the `Result`; the functions are `[[nodiscard]]`.
* `RDRAND`/`RDSEED` are x86-only. On ARM64 (Jetson, Mac) `has_rdrand()` returns false and `hw_random_fill` transparently uses the kernel source — same security, no special-casing needed in your code.
* Use `random_fill`/`random_bytes` for keys, nonces, salts, and tokens. `hw_seed_fill`/RDSEED is for seeding *other* PRNGs (thermal-noise TRNG), not bulk randomness.

---

## Memory hygiene — `secure_zero`

### `secure_zero(void*, size_t)` / `secure_zero(T&)`

**What it is.** An optimizer-proof memory wipe. A plain `memset` before free/return is a dead store the compiler may delete; `secure_zero` writes through a `volatile` pointer so the wipe is an observable side effect and always executes. Zero-dependency (no `explicit_bzero`/`memset_s`). Header: `<qbuem/crypto/secure_zero.hpp>`.

**When to use it.** Clearing key material, derived keystreams, password buffers, and other secrets the moment they are no longer needed — shrinking the window a secret sits in freed memory or a core dump. It already backs the `.wipe()` methods on `HmacContext`, `ChaCha20`, `Poly1305`, and `AesGcm`.

```cpp
#include <qbuem/crypto/secure_zero.hpp>
using namespace qbuem::crypto;

std::array<uint8_t, 32> key = /* ... */;
// ... use key ...
secure_zero(key);                       // wipes sizeof(key) bytes
// or: secure_zero(key.data(), key.size());
```

**Gotcha.** `secure_zero` does not stop a secret from having been *copied* elsewhere (registers, spilled stack, prior buffers). It only clears the bytes you point it at. Prefer calling the contexts' `.wipe()` so all key-derived internal state is cleared too.

---

## SIMD JWT parsing — `qbuem::security`

### `SIMDJwtParser` / `JwtView` (`simd_jwt.hpp`)

**What it is.** A stateless, zero-allocation, zero-copy JWT (RFC 7519) **parser**. It splits `Base64url(header).Base64url(payload).signature` using SIMD dot-scanning (AVX2 / SSE4.2 / NEON, scalar fallback), validates Base64url character sets, and returns a `JwtView` of string-views into the original token. Header: `<qbuem/security/simd_jwt.hpp>`. Namespace `qbuem::security`.

> **It parses, it does NOT verify signatures.** Signature verification is the job of an `ITokenVerifier` (see below). Always verify after parsing.

`JwtView` members and methods:

| Member | Type / signature | Meaning |
|---|---|---|
| `header` / `payload` / `signature` | `std::string_view` | the three Base64url segments (views into the token) |
| `signing_input(full_token)` | `std::string_view` | `header.payload` slice, the bytes an HMAC/RSA signature covers |
| `claim(key)` | `std::optional<std::string_view>` | string claim (e.g. `"sub"`, `"iss"`, `"aud"`) |
| `claim_int(key)` | `std::optional<int64_t>` | numeric claim (`"exp"`, `"iat"`, `"nbf"`) |
| `is_expired(now_unix, leeway_sec=0)` | `bool` | true if `exp` present and exceeded |

`SIMDJwtParser`: default-constructible, stateless (safe to share across threads). `std::optional<JwtView> parse(std::string_view token) const`. Constant `kMaxTokenLen = 8192`.

```cpp
#include <qbuem/security/simd_jwt.hpp>
using namespace qbuem::security;

SIMDJwtParser parser;
auto view = parser.parse(token);             // std::optional<JwtView>
if (!view) {
    // structural error: missing dot, empty header/payload, bad base64url, > 8 KB
    return;
}

if (view->is_expired(/*now_unix=*/std::time(nullptr))) return;

std::optional<std::string_view> sub = view->claim("sub");
std::optional<int64_t>          exp = view->claim_int("exp");
// ... now hand the token to your ITokenVerifier for signature verification ...
```

**Gotchas.**
* **Lifetime:** `JwtView` and any `claim()` string-view alias the **original token buffer**; the token must outlive every view derived from it.
* `claim()` / `claim_int()` Base64url-decode the payload into a **`thread_local` 8 KB buffer** and run a *flat* JSON scan — nested objects/arrays are not supported, and the returned string-view points into that thread-local buffer (consume it before the next `claim()` call on the same thread overwrites it).
* `parse()` rejects tokens larger than 8192 bytes and tokens whose header or payload is empty; padding (`=`) is rejected in segments (JWT is unpadded Base64url).
* Parsing performs **no** signature check — a `JwtView` you got from `parse()` is *untrusted* until an `ITokenVerifier` validates the signature.

### `JwtAuthAction<Msg>` (`jwt_action.hpp`)

**What it is.** A pipeline-stage Action that wires `SIMDJwtParser` + a user-supplied `middleware::ITokenVerifier` together: it extracts the Bearer token, parses it, checks `exp`/required claims, delegates signature verification, optionally caches the result, and injects validated `JwtClaims` into the pipeline `Context`. Header: `<qbuem/security/jwt_action.hpp>`.

**When to use it.** Authenticating requests inside a `qbuem` pipeline. You implement `ITokenVerifier` once for your signing scheme (HS256 via this module's HMAC, RS256/ES256 via your own code) and reuse it across routes.
**When NOT to.** If you are not using the pipeline `Context`/`Action` model, call `SIMDJwtParser` + your verifier directly instead.

Supporting types:
* `JwtClaims` — `sub`/`iss`/`aud` (`std::string_view`), `exp`/`iat`/`nbf` (`int64_t`, −1 = absent), `is_valid_at(now, leeway=0)`.
* `JwtAuthConfig` — `leeway_sec`, `cache_size` (0 disables the LRU cache), `require_exp`, `require_sub`, `auth_header` (default `"authorization"`).
* `JwtAuthResult` — enum of outcome codes (`OK`, `NoToken`, `InvalidFormat`, `Expired`, `NotYetValid`, `SignatureInvalid`, `MissingClaim`, `CacheHit`).
* `ActionResult` — `{ should_continue, error }` with `ActionResult::next()` / `ActionResult::stop(ec)`.

The `ITokenVerifier` interface (from `<qbuem/middleware/token_auth.hpp>`):

```cpp
class ITokenVerifier {
public:
  virtual ~ITokenVerifier() = default;
  // Return claims on success, std::nullopt on ANY failure. MUST be noexcept, must not throw.
  virtual std::optional<qbuem::middleware::TokenClaims>
  verify(std::string_view token) noexcept = 0;
};
// TokenClaims { std::string subject, issuer, audience; long exp=-1, nbf=-1;
//               std::unordered_map<std::string,std::string> custom; };
```

```cpp
#include <qbuem/security/jwt_action.hpp>
#include <qbuem/crypto/hmac.hpp>
using namespace qbuem;

// An HS256 verifier built entirely on this module's zero-dependency HMAC.
class HS256Verifier : public middleware::ITokenVerifier {
public:
  explicit HS256Verifier(std::string secret) : secret_(std::move(secret)) {}

  std::optional<middleware::TokenClaims>
  verify(std::string_view token) noexcept override {
    security::SIMDJwtParser parser;
    auto v = parser.parse(token);
    if (!v) return std::nullopt;

    // Recompute HMAC over header.payload and compare to the decoded signature.
    std::string_view signing = v->signing_input(token);
    auto mac = crypto::hmac_sha256(secret_, signing);     // 32-byte tag
    auto sig = crypto::base64url_decode(v->signature);    // Result<std::string>
    if (!sig || sig->size() != mac.size()) return std::nullopt;
    if (!qbuem::constant_time_equal(
            {reinterpret_cast<const char*>(mac.data()), mac.size()}, *sig))
      return std::nullopt;

    middleware::TokenClaims c;
    if (auto sub = v->claim("sub")) c.subject = std::string{*sub};
    c.exp = v->claim_int("exp").value_or(-1);
    return c;
  }
private:
  std::string secret_;
};

// Plug it into a pipeline stage:
//   PipelineBuilder<HttpRequest>()
//       .add<security::JwtAuthAction<HttpRequest>>(verifier, security::JwtAuthConfig{.leeway_sec = 5})
//       .add<ApiHandler>()
//       .build();
```

**Gotchas.**
* **Order is enforced for you:** the action parses, checks `exp` (when `require_exp`), then calls `verifier_.verify(...)`. A bad signature returns `ActionResult::stop(permission_denied)`. Your `verify()` must itself do the cryptographic check — `JwtAuthAction` will not verify the signature for you.
* The optional LRU cache keys on an FNV-1a hash of the token and short-circuits re-validation; set `cache_size = 0` if you cannot tolerate caching validity decisions.
* `JwtClaims` string-views (and those from `SIMDJwtParser`) reference the **token buffer** (and, for `claim()`, the thread-local decode buffer) — they are only valid while the request/token is alive. Copy into owned strings (as `TokenClaims` does) if you need to retain them beyond the stage.
* `ITokenVerifier::verify` must be `noexcept` and thread-safe (reactor threads call it concurrently); return `std::nullopt` for every failure rather than throwing.

---

## Choosing the right primitive (quick reference)

| Goal | Use | Avoid |
|---|---|---|
| Hash content / integrity check | `sha256` (HW-accelerated) | raw hash for passwords or MAC |
| Authenticate a message with a shared key | `hmac_sha256` + `verify_hmac_sha256` | `==`/`memcmp` on tags |
| Store a password verifier | `pbkdf2_hmac_sha256<32>(…, 600'000)` | plain SHA / low iterations |
| Derive sub-keys from a strong secret | `hkdf_sha256<N>(secret, salt, info)` | PBKDF2 (needlessly slow) |
| Encrypt+authenticate (portable, any CPU) | `chacha20_poly1305_seal/open` | raw `chacha20_xor` alone |
| Encrypt+authenticate (AES interop / AES-NI x86) | `AesGcm256::create` → `seal/open`, fall back to ChaCha20-Poly1305 on `function_not_supported` | assuming AES-NI exists |
| Generate keys/nonces/salts | `random_bytes<N>()` (checked Result) | `rand()` / non-CSPRNG |
| Wipe a secret buffer | `secure_zero` / context `.wipe()` | plain `memset` (may be elided) |
| Parse a JWT (no verify) | `SIMDJwtParser::parse` | treating the result as trusted |
| Authenticate requests in a pipeline | `JwtAuthAction<Msg>` + your `ITokenVerifier` | rolling your own stage ordering |
