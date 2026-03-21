/**
 * @file crypto_primitives_example.cpp
 * @brief Comprehensive demonstration of the qbuem::crypto module.
 *
 * ## Coverage
 *
 * Layer 1 — Hash & Key Derivation
 * - SHA-256 / SHA-512     one-shot and streaming digest
 * - HMAC-SHA-256/512      message authentication + constant-time verify
 * - PBKDF2-HMAC-SHA-256   password hashing (OWASP-compliant iteration count)
 * - HKDF-SHA-256          extract + expand for session key derivation
 *
 * Layer 2 — Encoding
 * - Base64 / Base64url    encode and decode with RFC 4648 compliance
 *
 * Layer 3 — Authenticated Encryption (AEAD)
 * - ChaCha20-Poly1305     encrypt + authenticate, with AAD
 * - AES-256-GCM           hardware-accelerated AEAD (when AES-NI available)
 *
 * All operations use zero heap allocation on the hot path (stack-only buffers).
 */

#include <qbuem/crypto/crypto.hpp>
#include <qbuem/compat/print.hpp>

#include <array>
#include <cstring>
#include <format>
#include <string>
#include <vector>

using namespace qbuem::crypto;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string to_hex(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
        out += std::format("{:02x}", static_cast<unsigned>(data[i]));
    return out;
}

template <size_t N>
static std::string to_hex(const std::array<uint8_t, N>& arr) {
    return to_hex(arr.data(), arr.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// §1  SHA-256 and SHA-512
// ─────────────────────────────────────────────────────────────────────────────

static void demo_sha() {
    std::println("-- §1  SHA-256 / SHA-512 --");

    // One-shot SHA-256
    const auto d256 = sha256(std::string_view("hello world"));
    std::println("  SHA-256(\"hello world\") = {}", to_hex(d256));

    // One-shot SHA-512
    const auto d512 = sha512(std::string_view("hello world"));
    std::println("  SHA-512(\"hello world\") = {}...", to_hex(d512.data(), 32));

    // Streaming SHA-256: split input across multiple update() calls
    Sha256Context ctx;
    ctx.update({reinterpret_cast<const uint8_t*>("hello"), 5});
    ctx.update({reinterpret_cast<const uint8_t*>(" world"), 6});
    const auto streaming = ctx.finalize();
    const bool matches = (streaming == d256);
    std::println("  streaming == one-shot: {}", matches ? "yes" : "no");

    // SHA-224 (truncated SHA-256)
    const auto d224 = sha224(std::string_view("abc"));
    std::println("  SHA-224(\"abc\") = {}", to_hex(d224));

    // SHA-384 (truncated SHA-512)
    const auto d384 = sha384(std::string_view("abc"));
    std::println("  SHA-384(\"abc\") = {}...\n", to_hex(d384.data(), 24));
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  HMAC-SHA-256 and constant-time verify
// ─────────────────────────────────────────────────────────────────────────────

static void demo_hmac() {
    std::println("-- §2  HMAC-SHA-256 / HMAC-SHA-512 --");

    const std::string_view key  = "my-secret-key";
    const std::string_view msg  = "payload=hello&user=alice";

    // Compute tag (one-shot)
    const auto tag256 = hmac_sha256(key, msg);
    std::println("  HMAC-SHA-256 tag = {}", to_hex(tag256));

    // Constant-time verification (correct key/message)
    const bool valid = verify_hmac_sha256(
        {reinterpret_cast<const uint8_t*>(key.data()), key.size()},
        {reinterpret_cast<const uint8_t*>(msg.data()), msg.size()},
        tag256);
    std::println("  verify (correct): {}", valid ? "OK" : "FAIL");

    // Tampered message → verification must fail
    const std::string_view tampered = "payload=hello&user=mallory";
    const bool invalid = verify_hmac_sha256(
        {reinterpret_cast<const uint8_t*>(key.data()), key.size()},
        {reinterpret_cast<const uint8_t*>(tampered.data()), tampered.size()},
        tag256);
    std::println("  verify (tampered): {}", invalid ? "FAIL (unexpected)" : "correctly rejected");

    // Streaming HMAC: build tag across multiple updates
    HmacSha256 h{key};
    h.update({reinterpret_cast<const uint8_t*>("payload=hello"), 13});
    h.update({reinterpret_cast<const uint8_t*>("&user=alice"), 11});
    const auto streaming_tag = h.finalize();
    std::println("  streaming == one-shot: {}\n",
                 (streaming_tag == tag256) ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  PBKDF2-HMAC-SHA-256 (password hashing)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_pbkdf2() {
    std::println("-- §3  PBKDF2-HMAC-SHA-256 (password hashing) --");

    const std::string_view password = "correct-horse-battery-staple";

    // Generate a random salt (16 bytes / 128 bits)
    const auto salt_result = random_bytes<16>();
    if (!salt_result) {
        std::println("  ERROR: random_bytes failed");
        return;
    }
    const auto& salt = *salt_result;

    // Derive a 32-byte key.
    // Use 100 000 iterations here for demo speed; production should use 600 000+.
    const auto dk = pbkdf2_hmac_sha256<32>(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(password.data()), password.size()},
        std::span<const uint8_t>{salt},
        100'000);
    std::println("  salt     = {}", to_hex(salt));
    std::println("  dk[32B]  = {}", to_hex(dk));

    // Determinism check: same inputs → same output
    const auto dk2 = pbkdf2_hmac_sha256<32>(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(password.data()), password.size()},
        std::span<const uint8_t>{salt},
        100'000);
    std::println("  deterministic: {}\n", (dk == dk2) ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  HKDF-SHA-256 (session key derivation)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_hkdf() {
    std::println("-- §4  HKDF-SHA-256 (session key derivation) --");

    // Shared secret (e.g., from ECDH — simulated here as 32 random bytes)
    const auto ikm_result = random_bytes<32>();
    if (!ikm_result) {
        std::println("  ERROR: random_bytes failed");
        return;
    }
    const auto& ikm = *ikm_result;

    // Random salt (could be the server nonce in a TLS-like protocol)
    const auto salt_result = random_bytes<32>();
    if (!salt_result) {
        std::println("  ERROR: random_bytes failed");
        return;
    }
    const auto& salt = *salt_result;

    // Extract: condense IKM + salt into a pseudo-random key (PRK)
    const auto prk = hkdf_extract_sha256(salt, ikm);
    std::println("  PRK      = {}", to_hex(prk));

    // Expand: derive multiple keys from the same PRK with different labels
    const std::string_view enc_label  = "enc_key v1";
    const std::string_view auth_label = "auth_key v1";

    std::array<uint8_t, 32> enc_key{}, auth_key{};
    hkdf_expand_sha256(prk,
        {reinterpret_cast<const uint8_t*>(enc_label.data()), enc_label.size()},
        enc_key);
    hkdf_expand_sha256(prk,
        {reinterpret_cast<const uint8_t*>(auth_label.data()), auth_label.size()},
        auth_key);

    std::println("  enc_key  = {}", to_hex(enc_key));
    std::println("  auth_key = {}", to_hex(auth_key));
    std::println("  keys differ: {}\n", (enc_key != auth_key) ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  Base64 / Base64url
// ─────────────────────────────────────────────────────────────────────────────

static void demo_base64() {
    std::println("-- §5  Base64 / Base64url encoding --");

    // Standard Base64 (RFC 4648 §4)
    struct Case { const char* raw; const char* b64; };
    constexpr Case kCases[] = {
        {"f",      "Zg=="},
        {"fo",     "Zm8="},
        {"foo",    "Zm9v"},
        {"foobar", "Zm9vYmFy"},
    };
    for (const auto& c : kCases) {
        const auto enc = base64_encode(std::string_view(c.raw));
        const bool ok  = (enc == c.b64);
        std::println("  base64(\"{}\") = {} {}", c.raw, enc, ok ? "OK" : "FAIL");
    }

    // Round-trip
    const std::string_view data = "binary\x00\x01\x02\xFF data";
    const auto enc  = base64_encode(
        {reinterpret_cast<const uint8_t*>(data.data()), data.size()});
    const auto dec  = base64_decode(enc);
    const bool rt   = dec.has_value() && (dec->size() == data.size()) &&
                      (std::memcmp(dec->data(), data.data(), data.size()) == 0);
    std::println("  binary round-trip: {}", rt ? "yes" : "no");

    // Base64url (no '+', '/', '=' — safe for URLs and JWT)
    std::array<uint8_t, 16> token_bytes{};
    (void)random_fill(token_bytes);  // NOLINT(bugprone-unused-return-value)
    const auto b64url = base64url_encode({token_bytes.data(), token_bytes.size()});
    std::println("  base64url(random 16B) = {} (no +/= chars: {})\n",
                 b64url,
                 (b64url.find_first_of("+/=") == std::string::npos) ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §6  ChaCha20-Poly1305 AEAD (RFC 8439)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_chacha20_poly1305() {
    std::println("-- §6  ChaCha20-Poly1305 AEAD --");

    // Generate key and nonce from CSPRNG
    const auto key_result   = random_bytes<32>();
    const auto nonce_result = random_bytes<12>();
    if (!key_result || !nonce_result) {
        std::println("  ERROR: random_bytes failed");
        return;
    }

    AeadKey   key{};
    AeadNonce nonce{};
    std::copy(key_result->begin(),   key_result->end(),   key.begin());
    std::copy(nonce_result->begin(), nonce_result->end(), nonce.begin());

    const std::string_view plaintext  = "Confidential: Transfer $1,000,000 to Alice";
    const std::string_view aad        = "transaction-id:abc123";  // authenticated, not encrypted

    std::println("  plaintext  = \"{}\"", plaintext);
    std::println("  aad        = \"{}\"", aad);

    // Encrypt + authenticate
    std::vector<uint8_t> ciphertext(plaintext.size());
    AeadTag tag{};
    chacha20_poly1305_seal(
        key, nonce,
        {reinterpret_cast<const uint8_t*>(aad.data()),       aad.size()},
        {reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size()},
        {ciphertext.data(), ciphertext.size()}, tag);

    std::println("  ciphertext = {}...", to_hex(ciphertext.data(), 16));
    std::println("  poly1305   = {}", to_hex(tag));

    // Decrypt + verify
    std::vector<uint8_t> recovered(plaintext.size());
    const auto open_result = chacha20_poly1305_open(
        key, nonce,
        {reinterpret_cast<const uint8_t*>(aad.data()),   aad.size()},
        {ciphertext.data(), ciphertext.size()}, tag,
        {recovered.data(), recovered.size()});

    if (open_result.has_value()) {
        std::string_view recovered_sv{
            reinterpret_cast<const char*>(recovered.data()), recovered.size()};
        std::println("  decrypted  = \"{}\"", recovered_sv);
        std::println("  round-trip: yes");
    } else {
        std::println("  ERROR: authentication failed");
    }

    // Demonstrate tamper detection
    auto tampered_ct = ciphertext;
    tampered_ct[0] ^= 0x01u;
    std::vector<uint8_t> out(plaintext.size());
    const auto tamper_result = chacha20_poly1305_open(
        key, nonce,
        {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
        {tampered_ct.data(), tampered_ct.size()}, tag,
        {out.data(), out.size()});
    std::println("  tamper detection: {}\n",
                 tamper_result.has_value() ? "FAIL (not detected!)" : "OK (forgery rejected)");
}

// ─────────────────────────────────────────────────────────────────────────────
// §7  AES-256-GCM (hardware-only; requires AES-NI)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_aes_gcm() {
    std::println("-- §7  AES-256-GCM (hardware AEAD) --");

    if (!has_aes_ni()) {
        std::println("  AES-NI not available — use ChaCha20-Poly1305 as fallback\n");
        return;
    }

    // Generate key, nonce from CSPRNG
    const auto key_result   = random_bytes<32>();
    const auto nonce_result = random_bytes<12>();
    if (!key_result || !nonce_result) {
        std::println("  ERROR: random_bytes failed");
        return;
    }

    std::array<uint8_t, 32> key{};
    AesGcmNonce nonce{};
    std::copy(key_result->begin(),   key_result->end(),   key.begin());
    std::copy(nonce_result->begin(), nonce_result->end(), nonce.begin());

    auto ctx_result = AesGcm256::create(key);
    if (!ctx_result) {
        std::println("  AES-256-GCM unavailable: {}\n", ctx_result.error().message());
        return;
    }
    auto& ctx = *ctx_result;

    const std::string_view plaintext = "AES-256-GCM: hardware-accelerated confidentiality";
    const std::string_view aad       = "request-id:xyz789";

    std::vector<uint8_t> ciphertext(plaintext.size());
    AesGcmTag tag{};
    ctx.seal(nonce,
             {reinterpret_cast<const uint8_t*>(aad.data()),       aad.size()},
             {reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size()},
             {ciphertext.data(), ciphertext.size()},
             tag);

    std::println("  ciphertext = {}...", to_hex(ciphertext.data(), 16));
    std::println("  GCM tag    = {}", to_hex(tag));

    std::vector<uint8_t> recovered(plaintext.size());
    const auto open_result = ctx.open(
        nonce,
        {reinterpret_cast<const uint8_t*>(aad.data()),   aad.size()},
        {ciphertext.data(), ciphertext.size()}, tag,
        {recovered.data(), recovered.size()});

    if (open_result.has_value()) {
        std::string_view sv{reinterpret_cast<const char*>(recovered.data()), recovered.size()};
        std::println("  decrypted  = \"{}\"", sv);
        std::println("  round-trip: yes\n");
    } else {
        std::println("  ERROR: decryption/authentication failed\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §8  CSPRNG — random_bytes<N>, hw_random_fill, hw_seed_fill
// ─────────────────────────────────────────────────────────────────────────────

static void demo_random() {
    std::println("-- §8  CSPRNG & Hardware Entropy --");

    std::println("  RDRAND available: {}", has_rdrand() ? "yes" : "no (kernel fallback)");
    std::println("  RDSEED available: {}", has_rdseed() ? "yes" : "no (kernel fallback)");

    // random_bytes<N>: stack-allocated, CSPRNG-backed
    const auto key = random_bytes<32>();
    if (key)
        std::println("  random_bytes<32>  = {}...", to_hex(key->data(), 16));

    // hw_random_fill: prefers RDRAND over getrandom
    std::array<uint8_t, 32> hw_buf{};
    if (hw_random_fill(hw_buf).has_value())
        std::println("  hw_random<32>     = {}...", to_hex(hw_buf.data(), 16));

    // hw_seed_fill: prefers RDSEED (for seeding other PRNGs)
    std::array<uint8_t, 32> seed_buf{};
    if (hw_seed_fill(seed_buf).has_value())
        std::println("  hw_seed<32>       = {}...", to_hex(seed_buf.data(), 16));

    // Base64url token (suitable as session ID or CSRF token)
    std::array<uint8_t, 32> token_raw{};
    (void)random_fill(token_raw);  // NOLINT(bugprone-unused-return-value)
    const std::string token = base64url_encode({token_raw.data(), token_raw.size()});
    std::println("  session token     = {} ({} chars)\n", token, token.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {  // NOLINT(bugprone-exception-escape)
    std::println("=== qbuem Cryptographic Primitives Example ===\n");

    demo_sha();
    demo_hmac();
    demo_pbkdf2();
    demo_hkdf();
    demo_base64();
    demo_chacha20_poly1305();
    demo_aes_gcm();
    demo_random();

    std::println("=== Done ===");
    return 0;
}
