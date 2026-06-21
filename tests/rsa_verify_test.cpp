/**
 * @file tests/rsa_verify_test.cpp
 * @brief Behavioral tests for native RS256 (RSASSA-PKCS1-v1.5 + SHA-256) verify.
 *
 * The test vector was minted offline with the OpenSSL CLI (RSA-2048, then
 * `openssl dgst -sha256 -sign`) and self-verified with `openssl dgst -verify`.
 * The private key was discarded; the modulus, public exponent, message and
 * signature below are all public by definition. The test is pure C++ (no OpenSSL
 * at build/run time) — it validates qbuem's own zero-dependency verifier.
 */

#include <qbuem/crypto/rsa.hpp>

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<uint8_t> unhex(std::string_view h) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    std::vector<uint8_t> out;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<uint8_t>((nib(h[i]) << 4) | nib(h[i + 1])));
    return out;
}

std::vector<uint8_t> bytes(std::string_view s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// ── RSA-2048 RS256 vector (OpenSSL-minted, self-verified) ──────────────────────
constexpr std::string_view kModulusHex =
    "C3A9968442F9AB034171E6A1D58790C1BE58E054C63959FC74787E826603EF58"
    "11B1A1C2F8B3F4FABD08F48D62300140D41BE604F7B98CAB5551BA6FA0D1A24A"
    "B8588E7BC7711383A9A0DC8F57145454424711A7B7BC718F0E48D48341D2E18A"
    "2B40AE78615D40FE5604D32611140777A6D0858452F0F4832AC81360DC6FDC14"
    "60A072CC4FA6DABFFFBE65BAE465836566F98BF714F23A3E907E168BB618C2F7"
    "C72E484555A4CF81FC6B5CF09982A27E69F71DBFC9EF9736D7C2617A87126793"
    "66A4A73746D3CE51AAE0930A5587A74C9A412521D39D46633B104C6BB919E357"
    "1DDC6ED79565E637A3D2CE24C3382ABFDB00662FB4196B3102FE19DED1414D73";

constexpr std::string_view kSignatureHex =
    "93f8bb4565fddaef414c76941e99c2567a060d8e0606c50cd3c227bdaea08852"
    "7307e41d065d43a0516a83184d7910318ecb93b83a391e6a591e5fab81b32f75"
    "adabcab169e84c2546646a4518948138f4ef36b4062410ac7aa1b9a25d7a6964"
    "f5418ff2e84e3da2922b1c889464a93c8ea03c9bfb2a37dce140aab44a161972"
    "3467e62f4bbcb585d227dfd047b4946e24fbfbe53be428cb27299d6ec38ef63d"
    "afec2a26521e3588a4dac6e12bfc86e6295e8adb1430e1ecf9d716670dd80f36"
    "ea48aa96a93cab1891b1499be11cc273cc815bece71b94ee361db63c22ff941c"
    "d6c51c5dc11cbd0c35617079875fac959a25c52a50a695a8387906541216e464";

constexpr std::string_view kMessage = "qbuem-stack RS256 test vector";
const std::vector<uint8_t> kExponent = {0x01, 0x00, 0x01}; // 65537

using qbuem::crypto::rsa_pkcs1_v15_sha256_verify;

} // namespace

TEST(Rs256Verify, ValidSignatureVerifies) {
    auto n = unhex(kModulusHex), sig = unhex(kSignatureHex), msg = bytes(kMessage);
    EXPECT_EQ(n.size(), 256u);
    EXPECT_EQ(sig.size(), 256u);
    EXPECT_TRUE(rsa_pkcs1_v15_sha256_verify(n, kExponent, msg, sig));
}

TEST(Rs256Verify, TamperedSignatureRejected) {
    auto n = unhex(kModulusHex), sig = unhex(kSignatureHex), msg = bytes(kMessage);
    sig[10] ^= 0x01; // flip one bit
    EXPECT_FALSE(rsa_pkcs1_v15_sha256_verify(n, kExponent, msg, sig));
}

TEST(Rs256Verify, WrongMessageRejected) {
    auto n = unhex(kModulusHex), sig = unhex(kSignatureHex);
    auto msg = bytes("qbuem-stack RS256 test vectoR"); // last char differs
    EXPECT_FALSE(rsa_pkcs1_v15_sha256_verify(n, kExponent, msg, sig));
}

TEST(Rs256Verify, WrongExponentRejected) {
    auto n = unhex(kModulusHex), sig = unhex(kSignatureHex), msg = bytes(kMessage);
    const std::vector<uint8_t> e3 = {0x03};
    EXPECT_FALSE(rsa_pkcs1_v15_sha256_verify(n, e3, msg, sig));
}

TEST(Rs256Verify, WrongSignatureLengthRejected) {
    auto n = unhex(kModulusHex), sig = unhex(kSignatureHex), msg = bytes(kMessage);
    sig.pop_back(); // 255 bytes != modulus length
    EXPECT_FALSE(rsa_pkcs1_v15_sha256_verify(n, kExponent, msg, sig));
}

TEST(Rs256Verify, LeadingZeroModulusStillVerifies) {
    auto n = unhex(kModulusHex), sig = unhex(kSignatureHex), msg = bytes(kMessage);
    n.insert(n.begin(), 0x00); // a stray leading zero byte must be tolerated
    EXPECT_TRUE(rsa_pkcs1_v15_sha256_verify(n, kExponent, msg, sig));
}
