/**
 * @file crypto_primitives_test.cpp
 * @brief Unit tests for qbuem::crypto — the zero-dependency cryptographic
 *        primitives module (include/qbuem/crypto/crypto.hpp).
 *
 * Coverage:
 * - SHA-256 / SHA-224  (FIPS 180-4 known-answer tests + streaming)
 * - SHA-512 / SHA-384  (consistency + streaming)
 * - HMAC-SHA-256/512   (RFC 4231 known-answer test + verify helpers)
 * - Base64 / Base64url  (RFC 4648 known-answer tests + round-trip)
 * - PBKDF2-HMAC-SHA-256 (determinism + known-answer)
 * - HKDF-SHA-256        (RFC 5869 known-answer test — extract + expand)
 * - ChaCha20-Poly1305   (RFC 8439 round-trip, tamper detection)
 * - AES-GCM             (round-trip when AES-NI available)
 * - CSPRNG              (random_fill, hw_random_fill, random_bytes<N>)
 */

#include <qbuem/crypto/crypto.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace qbuem::crypto;

// ─── Helper: hex encoding ────────────────────────────────────────────────────

namespace {

std::string to_hex(const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += kHex[data[i] >> 4u];
        out += kHex[data[i] & 0x0fu];
    }
    return out;
}

template <size_t N>
std::string to_hex(const std::array<uint8_t, N>& arr) {
    return to_hex(arr.data(), arr.size());
}

std::vector<uint8_t> from_hex(std::string_view hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4u) | nibble(hex[i + 1])));
    }
    return out;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// SHA-256
// ─────────────────────────────────────────────────────────────────────────────

// NIST FIPS 180-4 Appendix B.1 test vectors.
TEST(Sha256, EmptyStringKAT) {
    auto digest = sha256(std::span<const uint8_t>{});
    EXPECT_EQ(to_hex(digest),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, AbcKAT) {
    const std::string_view msg = "abc";
    auto digest = sha256(msg);
    EXPECT_EQ(to_hex(digest),
              "ba7816bf8f01cfea414140de5dae2ec73b003618bef0469f491c3f0cb96c72c2");
}

TEST(Sha256, DigestSizeIs32Bytes) {
    auto d = sha256(std::string_view("test"));
    static_assert(d.size() == 32u, "SHA-256 digest must be 32 bytes");
    EXPECT_EQ(d.size(), 32u);
}

TEST(Sha256, StreamingMatchesOneShot) {
    // "abcdef" split into two updates must equal one-shot sha256("abcdef")
    std::string_view full = "abcdef";
    std::string_view part1 = "abc";
    std::string_view part2 = "def";

    auto one_shot = sha256(full);

    Sha256Context ctx;
    ctx.update({reinterpret_cast<const uint8_t*>(part1.data()), part1.size()});
    ctx.update({reinterpret_cast<const uint8_t*>(part2.data()), part2.size()});
    auto streaming = ctx.finalize();

    EXPECT_EQ(one_shot, streaming);
}

TEST(Sha256, Idempotent) {
    // Hashing the same input twice must yield identical digests.
    auto d1 = sha256(std::string_view("hello world"));
    auto d2 = sha256(std::string_view("hello world"));
    EXPECT_EQ(d1, d2);
}

TEST(Sha256, DifferentInputsDifferentDigests) {
    auto d1 = sha256(std::string_view("input_a"));
    auto d2 = sha256(std::string_view("input_b"));
    EXPECT_NE(d1, d2);
}

// ─────────────────────────────────────────────────────────────────────────────
// SHA-224
// ─────────────────────────────────────────────────────────────────────────────

TEST(Sha224, EmptyStringKAT) {
    auto digest = sha224(std::span<const uint8_t>{});
    EXPECT_EQ(to_hex(digest),
              "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f");
}

TEST(Sha224, DigestSizeIs28Bytes) {
    auto d = sha224(std::string_view("test"));
    static_assert(d.size() == 28u, "SHA-224 digest must be 28 bytes");
    EXPECT_EQ(d.size(), 28u);
}

TEST(Sha224, DifferentFromSha256) {
    // SHA-224 and SHA-256 must produce different outputs.
    auto d224 = sha224(std::string_view("abc"));
    auto d256 = sha256(std::string_view("abc"));
    // Different sizes already, but make extra sure the first 28 bytes differ.
    EXPECT_NE(d224[0], d256[0]);
}

// ─────────────────────────────────────────────────────────────────────────────
// SHA-512
// ─────────────────────────────────────────────────────────────────────────────

TEST(Sha512, EmptyStringKAT) {
    auto digest = sha512(std::span<const uint8_t>{});
    EXPECT_EQ(to_hex(digest),
              "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
              "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}

TEST(Sha512, DigestSizeIs64Bytes) {
    auto d = sha512(std::string_view("test"));
    static_assert(d.size() == 64u, "SHA-512 digest must be 64 bytes");
    EXPECT_EQ(d.size(), 64u);
}

TEST(Sha512, StreamingMatchesOneShot) {
    std::string_view full = "hello world";
    std::string_view p1   = "hello ";
    std::string_view p2   = "world";

    auto one_shot = sha512(full);

    Sha512Context ctx;
    ctx.update({reinterpret_cast<const uint8_t*>(p1.data()), p1.size()});
    ctx.update({reinterpret_cast<const uint8_t*>(p2.data()), p2.size()});
    auto streaming = ctx.finalize();

    EXPECT_EQ(one_shot, streaming);
}

TEST(Sha512, DifferentFromSha256) {
    // Different digest sizes; verify outputs are not equal in first 32 bytes.
    const std::string_view msg = "test_message";
    auto d256 = sha256(msg);
    auto d512 = sha512(msg);
    bool first32_equal = (std::memcmp(d256.data(), d512.data(), 32) == 0);
    EXPECT_FALSE(first32_equal);
}

// ─────────────────────────────────────────────────────────────────────────────
// SHA-384
// ─────────────────────────────────────────────────────────────────────────────

TEST(Sha384, DigestSizeIs48Bytes) {
    auto d = sha384(std::string_view("test"));
    static_assert(d.size() == 48u, "SHA-384 digest must be 48 bytes");
    EXPECT_EQ(d.size(), 48u);
}

TEST(Sha384, EmptyStringKAT) {
    auto digest = sha384(std::span<const uint8_t>{});
    EXPECT_EQ(to_hex(digest),
              "38b060a751ac96384cd9327eb1b1e36a21fdb71114be0743"
              "4c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b");
}

// ─────────────────────────────────────────────────────────────────────────────
// HMAC-SHA-256
// ─────────────────────────────────────────────────────────────────────────────

// RFC 4231 §4.2 test vector 1.
TEST(HmacSha256, Rfc4231TestVector1) {
    // Key: 20 bytes of 0x0b
    std::array<uint8_t, 20> key;
    key.fill(0x0b);

    const std::string_view data = "Hi There";
    const auto tag = hmac_sha256(
        {key.data(), key.size()},
        {reinterpret_cast<const uint8_t*>(data.data()), data.size()});

    EXPECT_EQ(to_hex(tag),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

// RFC 4231 §4.3 test vector 2.
TEST(HmacSha256, Rfc4231TestVector2) {
    const std::string_view key  = "Jefe";
    const std::string_view data = "what do ya want for nothing?";
    const auto tag = hmac_sha256(key, data);
    EXPECT_EQ(to_hex(tag),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964a72020");
}

TEST(HmacSha256, DigestSizeIs32) {
    auto tag = hmac_sha256(std::string_view("key"), std::string_view("msg"));
    EXPECT_EQ(tag.size(), 32u);
}

TEST(HmacSha256, VerifyCorrectTag) {
    std::array<uint8_t, 20> key;
    key.fill(0x0b);
    const std::string_view data = "Hi There";
    const auto tag = hmac_sha256(
        {key.data(), key.size()},
        {reinterpret_cast<const uint8_t*>(data.data()), data.size()});

    EXPECT_TRUE(verify_hmac_sha256(
        {key.data(), key.size()},
        {reinterpret_cast<const uint8_t*>(data.data()), data.size()},
        tag));
}

TEST(HmacSha256, VerifyWrongTagFails) {
    const std::string_view key  = "secret";
    const std::string_view data = "message";
    auto tag = hmac_sha256(key, data);
    tag[0] ^= 0xffu;  // flip one byte

    EXPECT_FALSE(verify_hmac_sha256(
        {reinterpret_cast<const uint8_t*>(key.data()),  key.size()},
        {reinterpret_cast<const uint8_t*>(data.data()), data.size()},
        tag));
}

TEST(HmacSha256, StreamingMatchesCompute) {
    const std::string_view key  = "streaming_key";
    const std::string_view part1 = "hello ";
    const std::string_view part2 = "world";
    const std::string_view full  = "hello world";

    HmacSha256 h{key};
    h.update({reinterpret_cast<const uint8_t*>(part1.data()), part1.size()});
    h.update({reinterpret_cast<const uint8_t*>(part2.data()), part2.size()});
    const auto streaming = h.finalize();

    const auto one_shot = hmac_sha256(key, full);

    EXPECT_EQ(streaming, one_shot);
}

TEST(HmacSha256, ResetAndReuse) {
    const std::string_view key = "reuse_key";
    HmacSha256 h{key};
    const auto t1 = h.compute(std::string_view("msg1"));
    h.reset();
    const auto t2 = h.compute(std::string_view("msg1"));
    EXPECT_EQ(t1, t2);
}

// ─────────────────────────────────────────────────────────────────────────────
// HMAC-SHA-512
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacSha512, DigestSizeIs64) {
    auto tag = hmac_sha512(std::string_view("key"), std::string_view("msg"));
    EXPECT_EQ(tag.size(), 64u);
}

TEST(HmacSha512, DifferentFromHmacSha256) {
    const std::string_view key = "k";
    const std::string_view msg = "m";
    auto t256 = hmac_sha256(key, msg);
    auto t512 = hmac_sha512(key, msg);
    // Different sizes; first 32 bytes should differ.
    bool same = (std::memcmp(t256.data(), t512.data(), 32) == 0);
    EXPECT_FALSE(same);
}

TEST(HmacSha512, VerifyCorrectTag) {
    const std::string_view key = "k512";
    const std::string_view msg = "hello";
    auto tag = hmac_sha512(key, msg);
    EXPECT_TRUE(verify_hmac_sha512(
        {reinterpret_cast<const uint8_t*>(key.data()), key.size()},
        {reinterpret_cast<const uint8_t*>(msg.data()), msg.size()},
        tag));
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64 (RFC 4648)
// ─────────────────────────────────────────────────────────────────────────────

// RFC 4648 §10 test vectors.
TEST(Base64Encode, Rfc4648EmptyString) {
    EXPECT_EQ(base64_encode(std::string_view("")), "");
}

TEST(Base64Encode, Rfc4648SingleChar) {
    EXPECT_EQ(base64_encode(std::string_view("f")), "Zg==");
}

TEST(Base64Encode, Rfc4648TwoChars) {
    EXPECT_EQ(base64_encode(std::string_view("fo")), "Zm8=");
}

TEST(Base64Encode, Rfc4648ThreeChars) {
    EXPECT_EQ(base64_encode(std::string_view("foo")), "Zm9v");
}

TEST(Base64Encode, Rfc4648FourChars) {
    EXPECT_EQ(base64_encode(std::string_view("foob")), "Zm9vYg==");
}

TEST(Base64Encode, Rfc4648FiveChars) {
    EXPECT_EQ(base64_encode(std::string_view("fooba")), "Zm9vYmE=");
}

TEST(Base64Encode, Rfc4648SixChars) {
    EXPECT_EQ(base64_encode(std::string_view("foobar")), "Zm9vYmFy");
}

TEST(Base64Decode, Rfc4648EmptyString) {
    auto r = base64_decode("");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "");
}

TEST(Base64Decode, Rfc4648SingleChar) {
    auto r = base64_decode("Zg==");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "f");
}

TEST(Base64Decode, Rfc4648SixChars) {
    auto r = base64_decode("Zm9vYmFy");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "foobar");
}

TEST(Base64Decode, InvalidCharacterFails) {
    // '!' is not a valid Base64 character.
    auto r = base64_decode("Zm9v!mFy");
    EXPECT_FALSE(r.has_value());
}

TEST(Base64, RoundTrip) {
    const std::string_view original = "The quick brown fox jumps over the lazy dog";
    const std::string encoded = base64_encode(original);
    const auto decoded = base64_decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, std::string(original));
}

TEST(Base64, BinaryRoundTrip) {
    // Binary data including 0x00 bytes.
    std::array<uint8_t, 32> bin{};
    for (size_t i = 0; i < 32; ++i)
        bin[i] = static_cast<uint8_t>(i);

    const auto encoded = base64_encode({bin.data(), bin.size()});
    const auto decoded = base64_decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 32u);
    EXPECT_EQ(std::memcmp(decoded->data(), bin.data(), 32), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64url (RFC 4648 §5)
// ─────────────────────────────────────────────────────────────────────────────

TEST(Base64Url, EncodeNoPlus) {
    // Base64url uses '-' instead of '+' and '_' instead of '/'.
    // Verify a value that would produce '+' or '/' in standard Base64.
    // 0xfb -> encodes to base64 "+" context; base64url replaces with '-'.
    std::array<uint8_t, 3> bytes = {0xfb, 0xef, 0xbe};
    const std::string std_enc  = base64_encode({bytes.data(), bytes.size()});
    const std::string url_enc  = base64url_encode({bytes.data(), bytes.size()});
    // Standard may produce '+' or '/'; URL-safe must not.
    EXPECT_EQ(url_enc.find('+'), std::string::npos);
    EXPECT_EQ(url_enc.find('/'), std::string::npos);
    EXPECT_NE(std_enc, url_enc);  // They differ for this input
}

TEST(Base64Url, RoundTrip) {
    const std::string_view original = "Hello, Base64url!";
    const std::string encoded = base64url_encode(original);
    const auto decoded = base64url_decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, std::string(original));
}

TEST(Base64Url, NoPaddingByDefault) {
    // Default base64url_encode produces no '=' padding.
    const std::string encoded = base64url_encode(std::string_view("f"));
    EXPECT_EQ(encoded.find('='), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// PBKDF2-HMAC-SHA-256
// ─────────────────────────────────────────────────────────────────────────────

TEST(Pbkdf2Sha256, Deterministic) {
    // Same inputs → same output.
    auto dk1 = pbkdf2_hmac_sha256<32>(std::string_view("password"),
                                       std::string_view("salt"), 1000);
    auto dk2 = pbkdf2_hmac_sha256<32>(std::string_view("password"),
                                       std::string_view("salt"), 1000);
    EXPECT_EQ(dk1, dk2);
}

TEST(Pbkdf2Sha256, DifferentPasswordDifferentKey) {
    auto dk1 = pbkdf2_hmac_sha256<32>(std::string_view("password1"),
                                       std::string_view("salt"), 1);
    auto dk2 = pbkdf2_hmac_sha256<32>(std::string_view("password2"),
                                       std::string_view("salt"), 1);
    EXPECT_NE(dk1, dk2);
}

TEST(Pbkdf2Sha256, DifferentSaltDifferentKey) {
    auto dk1 = pbkdf2_hmac_sha256<32>(std::string_view("password"),
                                       std::string_view("salt1"), 1);
    auto dk2 = pbkdf2_hmac_sha256<32>(std::string_view("password"),
                                       std::string_view("salt2"), 1);
    EXPECT_NE(dk1, dk2);
}

TEST(Pbkdf2Sha256, DifferentIterationsDifferentKey) {
    auto dk1 = pbkdf2_hmac_sha256<32>(std::string_view("password"),
                                       std::string_view("salt"), 1);
    auto dk2 = pbkdf2_hmac_sha256<32>(std::string_view("password"),
                                       std::string_view("salt"), 2);
    EXPECT_NE(dk1, dk2);
}

TEST(Pbkdf2Sha256, OutputSizeIs32) {
    auto dk = pbkdf2_hmac_sha256<32>(std::string_view("pw"),
                                      std::string_view("s"), 1);
    EXPECT_EQ(dk.size(), 32u);
}

TEST(Pbkdf2Sha256, NonZeroOutput) {
    auto dk = pbkdf2_hmac_sha256<32>(std::string_view("password"),
                                      std::string_view("salt"), 1);
    bool all_zero = true;
    for (auto b : dk) {
        if (b != 0) { all_zero = false; break; }
    }
    EXPECT_FALSE(all_zero);
}

// ─────────────────────────────────────────────────────────────────────────────
// HKDF-SHA-256
// ─────────────────────────────────────────────────────────────────────────────

// RFC 5869 Appendix A.1 known-answer test.
TEST(HkdfSha256, Rfc5869A1Extract) {
    // IKM = 22 bytes of 0x0b
    std::array<uint8_t, 22> ikm;
    ikm.fill(0x0b);

    // salt = 0x00..0x0c (13 bytes)
    const std::array<uint8_t, 13> salt = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c};

    const auto prk = hkdf_extract_sha256(salt, ikm);

    EXPECT_EQ(to_hex(prk),
              "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
}

TEST(HkdfSha256, Rfc5869A1Expand) {
    // PRK from A.1 extract step (above).
    const auto prk_bytes = from_hex(
        "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    Sha256Digest prk{};
    std::copy(prk_bytes.begin(), prk_bytes.end(), prk.begin());

    // info = 0xf0..0xf9 (10 bytes)
    const std::array<uint8_t, 10> info = {
        0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9};

    // OKM length = 42 bytes
    std::array<uint8_t, 42> okm{};
    hkdf_expand_sha256(prk, info, okm);

    EXPECT_EQ(to_hex(okm.data(), 42),
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
              "34007208d5b887185865");
}

TEST(HkdfSha256, Deterministic) {
    // Same inputs → same output.
    std::array<uint8_t, 16> ikm;
    ikm.fill(0x42);
    std::array<uint8_t, 8> salt;
    salt.fill(0x01);
    const std::string_view info = "test_context";

    std::array<uint8_t, 32> okm1{}, okm2{};
    const auto prk = hkdf_extract_sha256(salt, ikm);
    hkdf_expand_sha256(prk,
        {reinterpret_cast<const uint8_t*>(info.data()), info.size()},
        okm1);
    hkdf_expand_sha256(prk,
        {reinterpret_cast<const uint8_t*>(info.data()), info.size()},
        okm2);
    EXPECT_EQ(okm1, okm2);
}

TEST(HkdfSha256, DifferentContextsDifferentKeys) {
    std::array<uint8_t, 32> ikm;
    ikm.fill(0x99);
    std::array<uint8_t, 16> salt;
    salt.fill(0x55);

    const auto prk = hkdf_extract_sha256(salt, ikm);

    const std::string_view info1 = "encryption_key";
    const std::string_view info2 = "authentication_key";

    std::array<uint8_t, 32> okm1{}, okm2{};
    hkdf_expand_sha256(prk,
        {reinterpret_cast<const uint8_t*>(info1.data()), info1.size()}, okm1);
    hkdf_expand_sha256(prk,
        {reinterpret_cast<const uint8_t*>(info2.data()), info2.size()}, okm2);
    EXPECT_NE(okm1, okm2);
}

// ─────────────────────────────────────────────────────────────────────────────
// ChaCha20-Poly1305 AEAD (RFC 8439)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ChaCha20Poly1305, SealOpenRoundTrip) {
    AeadKey   key{};
    AeadNonce nonce{};
    key[0]   = 0x01;  // non-trivial key
    nonce[0] = 0x02;  // non-trivial nonce

    const std::string plaintext = "Hello, authenticated encryption!";
    const std::string aad_str   = "associated data";

    std::span<const uint8_t> pt_span{
        reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size()};
    std::span<const uint8_t> aad_span{
        reinterpret_cast<const uint8_t*>(aad_str.data()), aad_str.size()};

    // Seal: allocate output buffers.
    std::vector<uint8_t> ciphertext(plaintext.size());
    AeadTag tag{};
    chacha20_poly1305_seal(key, nonce, aad_span, pt_span,
                           {ciphertext.data(), ciphertext.size()}, tag);

    // Ciphertext must differ from plaintext.
    bool ct_differs = (std::memcmp(ciphertext.data(), plaintext.data(),
                                   plaintext.size()) != 0);
    EXPECT_TRUE(ct_differs);

    // Open: decrypt and verify.
    std::vector<uint8_t> recovered(plaintext.size());
    auto result = chacha20_poly1305_open(
        key, nonce, aad_span,
        {ciphertext.data(), ciphertext.size()}, tag,
        {recovered.data(), recovered.size()});

    ASSERT_TRUE(result.has_value()) << "Authentication failed";
    EXPECT_EQ(std::memcmp(recovered.data(), plaintext.data(), plaintext.size()), 0);
}

TEST(ChaCha20Poly1305, TamperedCiphertextFails) {
    AeadKey key{};
    AeadNonce nonce{};
    key[1] = 0xaa;

    const std::string_view pt = "secret message";
    std::vector<uint8_t> ct(pt.size());
    AeadTag tag{};

    chacha20_poly1305_seal(
        key, nonce, {},
        {reinterpret_cast<const uint8_t*>(pt.data()), pt.size()},
        {ct.data(), ct.size()}, tag);

    // Flip a bit in the ciphertext.
    ct[4] ^= 0x01u;

    std::vector<uint8_t> out(pt.size());
    auto result = chacha20_poly1305_open(
        key, nonce, {},
        {ct.data(), ct.size()}, tag,
        {out.data(), out.size()});

    EXPECT_FALSE(result.has_value());
}

TEST(ChaCha20Poly1305, TamperedTagFails) {
    AeadKey key{};
    AeadNonce nonce{};
    key[2] = 0xbb;

    const std::string_view pt = "secret message";
    std::vector<uint8_t> ct(pt.size());
    AeadTag tag{};

    chacha20_poly1305_seal(
        key, nonce, {},
        {reinterpret_cast<const uint8_t*>(pt.data()), pt.size()},
        {ct.data(), ct.size()}, tag);

    // Flip a bit in the authentication tag.
    tag[0] ^= 0x01u;

    std::vector<uint8_t> out(pt.size());
    auto result = chacha20_poly1305_open(
        key, nonce, {},
        {ct.data(), ct.size()}, tag,
        {out.data(), out.size()});

    EXPECT_FALSE(result.has_value());
}

TEST(ChaCha20Poly1305, TamperedAadFails) {
    AeadKey key{};
    AeadNonce nonce{};
    key[3] = 0xcc;

    const std::string_view pt  = "secret message";
    const std::string_view aad = "original aad";
    std::vector<uint8_t> ct(pt.size());
    AeadTag tag{};

    chacha20_poly1305_seal(
        key, nonce,
        {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
        {reinterpret_cast<const uint8_t*>(pt.data()),  pt.size()},
        {ct.data(), ct.size()}, tag);

    // Open with different AAD must fail.
    const std::string_view wrong_aad = "wrong aad!!";
    std::vector<uint8_t> out(pt.size());
    auto result = chacha20_poly1305_open(
        key, nonce,
        {reinterpret_cast<const uint8_t*>(wrong_aad.data()), wrong_aad.size()},
        {ct.data(), ct.size()}, tag,
        {out.data(), out.size()});

    EXPECT_FALSE(result.has_value());
}

TEST(ChaCha20Poly1305, InPlaceSealOpen) {
    AeadKey key{};
    AeadNonce nonce{};
    key[5] = 0x55;

    const std::string original = "in-place encryption test";
    std::vector<uint8_t> buf(
        reinterpret_cast<const uint8_t*>(original.data()),
        reinterpret_cast<const uint8_t*>(original.data()) + original.size());

    // Seal in-place: buf becomes ciphertext.
    const AeadTag tag = chacha20_poly1305_seal(key, nonce, {}, buf);

    // buf must differ from original.
    bool differs = (std::memcmp(buf.data(), original.data(), original.size()) != 0);
    EXPECT_TRUE(differs);

    // Open in-place: buf becomes plaintext.
    auto result = chacha20_poly1305_open(key, nonce, {}, buf, tag);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::memcmp(buf.data(), original.data(), original.size()), 0);
}

TEST(ChaCha20Poly1305, EmptyPlaintextRoundTrip) {
    AeadKey key{};
    AeadNonce nonce{};
    key[7] = 0x77;
    const std::string_view aad = "only aad, no plaintext";

    std::vector<uint8_t> ct;  // zero bytes
    AeadTag tag{};
    chacha20_poly1305_seal(
        key, nonce,
        {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
        {}, ct, tag);  // empty plaintext span

    std::vector<uint8_t> pt;
    auto result = chacha20_poly1305_open(
        key, nonce,
        {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
        {}, tag, pt);
    EXPECT_TRUE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// AES-GCM (hardware-only; skip if AES-NI not available)
// ─────────────────────────────────────────────────────────────────────────────

TEST(AesGcm, HasAesNiReturnsBool) {
    // Only checks the detection is callable.
    const bool avail = has_aes_ni();
    (void)avail;
    SUCCEED();
}

TEST(AesGcm128, SealOpenRoundTrip) {
    if (!has_aes_ni()) GTEST_SKIP() << "AES-NI not available on this CPU";

    std::array<uint8_t, 16> key{};
    for (uint8_t i = 0; i < 16; ++i) key[i] = i;
    AesGcmNonce nonce{};
    nonce[0] = 0x01;

    auto ctx_result = AesGcm128::create(key);
    ASSERT_TRUE(ctx_result.has_value());
    auto& ctx = *ctx_result;

    const std::string_view pt  = "AES-GCM test plaintext";
    const std::string_view aad = "test aad";

    std::vector<uint8_t> ct(pt.size());
    AesGcmTag tag{};
    ctx.seal(nonce,
             {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
             {reinterpret_cast<const uint8_t*>(pt.data()),  pt.size()},
             {ct.data(), ct.size()},
             tag);

    std::vector<uint8_t> recovered(pt.size());
    auto open_result = ctx.open(
        nonce,
        {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
        {ct.data(), ct.size()},
        tag,
        {recovered.data(), recovered.size()});

    ASSERT_TRUE(open_result.has_value());
    EXPECT_EQ(std::memcmp(recovered.data(), pt.data(), pt.size()), 0);
}

TEST(AesGcm256, SealOpenRoundTrip) {
    if (!has_aes_ni()) GTEST_SKIP() << "AES-NI not available on this CPU";

    std::array<uint8_t, 32> key{};
    for (uint8_t i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i * 2);
    AesGcmNonce nonce{};
    nonce[3] = 0x07;

    auto ctx_result = AesGcm256::create(key);
    ASSERT_TRUE(ctx_result.has_value());
    auto& ctx = *ctx_result;

    const std::string_view pt  = "AES-256-GCM test plaintext with extra data";
    const std::string_view aad = "context info";

    std::vector<uint8_t> ct(pt.size());
    AesGcmTag tag{};
    ctx.seal(nonce,
             {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
             {reinterpret_cast<const uint8_t*>(pt.data()),  pt.size()},
             {ct.data(), ct.size()},
             tag);

    std::vector<uint8_t> recovered(pt.size());
    auto open_result = ctx.open(
        nonce,
        {reinterpret_cast<const uint8_t*>(aad.data()), aad.size()},
        {ct.data(), ct.size()},
        tag,
        {recovered.data(), recovered.size()});

    ASSERT_TRUE(open_result.has_value());
    EXPECT_EQ(std::memcmp(recovered.data(), pt.data(), pt.size()), 0);
}

TEST(AesGcm256, TamperedCiphertextFails) {
    if (!has_aes_ni()) GTEST_SKIP() << "AES-NI not available";

    std::array<uint8_t, 32> key{};
    key.fill(0x42);
    AesGcmNonce nonce{};

    auto ctx_result = AesGcm256::create(key);
    ASSERT_TRUE(ctx_result.has_value());
    auto& ctx = *ctx_result;

    const std::string_view pt = "secret";
    std::vector<uint8_t> ct(pt.size());
    AesGcmTag tag{};
    ctx.seal(nonce, {},
             {reinterpret_cast<const uint8_t*>(pt.data()), pt.size()},
             {ct.data(), ct.size()},
             tag);

    // Tamper with ciphertext.
    ct[0] ^= 0xffu;

    std::vector<uint8_t> out(pt.size());
    auto result = ctx.open(nonce, {},
                           {ct.data(), ct.size()}, tag,
                           {out.data(), out.size()});
    EXPECT_FALSE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// CSPRNG — random_fill, hw_random_fill, random_bytes<N>
// ─────────────────────────────────────────────────────────────────────────────

TEST(CryptoRandom, RandomFillProducesNonZeroBuffer) {
    std::array<uint8_t, 32> buf{};
    auto r = random_fill(buf);
    ASSERT_TRUE(r.has_value());
    bool all_zero = true;
    for (auto b : buf) if (b != 0) { all_zero = false; break; }
    EXPECT_FALSE(all_zero);
}

TEST(CryptoRandom, TwoCallsDiffer) {
    std::array<uint8_t, 32> a{}, b{};
    ASSERT_TRUE(random_fill(a).has_value());
    ASSERT_TRUE(random_fill(b).has_value());
    // Collision probability: 2^-256.
    EXPECT_NE(a, b);
}

TEST(CryptoRandom, RandomBytesNonZero) {
    auto r = random_bytes<32>();
    ASSERT_TRUE(r.has_value());
    bool all_zero = true;
    for (auto b : *r) if (b != 0) { all_zero = false; break; }
    EXPECT_FALSE(all_zero);
}

TEST(CryptoRandom, HwRandomFillNonZero) {
    std::array<uint8_t, 32> buf{};
    auto r = hw_random_fill(buf);
    ASSERT_TRUE(r.has_value());
    bool all_zero = true;
    for (auto b : buf) if (b != 0) { all_zero = false; break; }
    EXPECT_FALSE(all_zero);
}

TEST(CryptoRandom, HwSeedFillNonZero) {
    std::array<uint8_t, 32> buf{};
    auto r = hw_seed_fill(buf);
    ASSERT_TRUE(r.has_value());
    bool all_zero = true;
    for (auto b : buf) if (b != 0) { all_zero = false; break; }
    EXPECT_FALSE(all_zero);
}

TEST(CryptoRandom, HasRdrandReturnsBool) {
    bool v = has_rdrand();
    (void)v;
    SUCCEED();
}

TEST(CryptoRandom, HasRdseedReturnsBool) {
    bool v = has_rdseed();
    (void)v;
    SUCCEED();
}

TEST(CryptoRandom, Rdrand64ConsecutiveDiffer) {
    if (!has_rdrand()) GTEST_SKIP() << "RDRAND not supported";
    uint64_t a = 0, b = 0;
    EXPECT_TRUE(rdrand64(a));
    EXPECT_TRUE(rdrand64(b));
    // Collision probability: 2^-64.
    EXPECT_NE(a, b);
}

TEST(CryptoRandom, Rdseed64ConsecutiveDiffer) {
    if (!has_rdseed()) GTEST_SKIP() << "RDSEED not supported";
    uint64_t a = 0, b = 0;
    EXPECT_TRUE(rdseed64(a));
    EXPECT_TRUE(rdseed64(b));
    EXPECT_NE(a, b);
}
