#pragma once
/**
 * @file qbuem/crypto/rsa.hpp
 * @brief RSASSA-PKCS1-v1.5 signature **verification** with SHA-256 (RS256).
 *
 * Zero-dependency, header-only. This is the cryptographic foundation for RS256
 * JWT / JWKS verification — the SaaS auth path where tokens are issued by an
 * external IdP (Auth0, Cognito, Firebase, Okta, …) and the server verifies them
 * with the IdP's RSA public key.
 *
 * Scope: **verification only** (the public-key operation `s^e mod n`). There is no
 * RSA key generation, signing, or private-key handling here — a verifying server
 * never needs them. The public exponent is tiny (typically 65537), so a compact
 * verify-only big-integer (no division, no Montgomery form) is sufficient and
 * keeps the implementation small and obviously correct.
 *
 * This is an auth-verify step, not a zero-allocation hot path: it uses small
 * heap buffers for the big-integer limbs (unavoidable for an arbitrary-size
 * modulus). It satisfies the Zero-Dependency pillar (no third-party headers); the
 * ports-and-adapters layout means an OpenSSL/mbedTLS adapter could replace it
 * behind the same `ITokenVerifier` port if hardware RSA is ever wanted.
 *
 * @code
 * bool ok = qbuem::crypto::rsa_pkcs1_v15_sha256_verify(modulus, exponent, msg, sig);
 * @endcode
 */

#include <qbuem/crypto/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace qbuem::crypto {

namespace detail::rsa {

// Big unsigned integer for RSA verification. Limbs are base 2^32, little-endian
// (limb[0] is least significant). Only the operations needed for `s^e mod n` are
// implemented: compare, subtract, shift-by-one, schoolbook multiply, and modular
// reduction by binary long division.
using BigUint = std::vector<uint32_t>;

inline void trim(BigUint& a) {
    while (a.size() > 1 && a.back() == 0) a.pop_back();
    if (a.empty()) a.push_back(0);
}

// Parse big-endian bytes into little-endian 32-bit limbs.
inline BigUint from_be_bytes(std::span<const uint8_t> b) {
    BigUint out;
    out.reserve(b.size() / 4 + 1);
    size_t i = b.size();
    while (i >= 4) {
        out.push_back((static_cast<uint32_t>(b[i - 4]) << 24) |
                      (static_cast<uint32_t>(b[i - 3]) << 16) |
                      (static_cast<uint32_t>(b[i - 2]) <<  8) |
                       static_cast<uint32_t>(b[i - 1]));
        i -= 4;
    }
    if (i > 0) {
        uint32_t limb = 0;
        for (size_t j = 0; j < i; ++j) limb = (limb << 8) | b[j];
        out.push_back(limb);
    }
    if (out.empty()) out.push_back(0);
    trim(out);
    return out;
}

// Serialize to big-endian, left-zero-padded to exactly `k` bytes.
inline std::vector<uint8_t> to_be_bytes(const BigUint& a, size_t k) {
    std::vector<uint8_t> out(k, 0);
    size_t pos = k;
    for (size_t i = 0; i < a.size() && pos > 0; ++i) {
        uint32_t limb = a[i];
        for (int b = 0; b < 4 && pos > 0; ++b) {
            out[--pos] = static_cast<uint8_t>(limb & 0xFF);
            limb >>= 8;
        }
    }
    return out;
}

// -1 if a<b, 0 if a==b, 1 if a>b.
inline int cmp(const BigUint& a, const BigUint& b) {
    size_t na = a.size(), nb = b.size();
    while (na > 1 && a[na - 1] == 0) --na;
    while (nb > 1 && b[nb - 1] == 0) --nb;
    if (na != nb) return na < nb ? -1 : 1;
    for (size_t i = na; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// a -= b, requires a >= b.
inline void sub_inplace(BigUint& a, const BigUint& b) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const uint64_t bi = (i < b.size()) ? b[i] : 0;
        const uint64_t need = bi + borrow;
        if (static_cast<uint64_t>(a[i]) >= need) {
            a[i] = static_cast<uint32_t>(static_cast<uint64_t>(a[i]) - need);
            borrow = 0;
        } else {
            a[i] = static_cast<uint32_t>(
                (static_cast<uint64_t>(a[i]) + 0x100000000ULL) - need);
            borrow = 1;
        }
    }
    trim(a);
}

// a = (a << 1) | (bit & 1).
inline void shl1(BigUint& a, uint32_t bit) {
    uint32_t carry = bit & 1;
    for (size_t i = 0; i < a.size(); ++i) {
        const uint32_t next = a[i] >> 31;
        a[i] = (a[i] << 1) | carry;
        carry = next;
    }
    if (carry != 0) a.push_back(1);
}

// Schoolbook multiply: returns a * b.
inline BigUint mul(const BigUint& a, const BigUint& b) {
    BigUint out(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t carry = 0;
        const uint64_t ai = a[i];
        for (size_t j = 0; j < b.size(); ++j) {
            const uint64_t cur =
                static_cast<uint64_t>(out[i + j]) + ai * b[j] + carry;
            out[i + j] = static_cast<uint32_t>(cur & 0xFFFFFFFF);
            carry = cur >> 32;
        }
        out[i + b.size()] += static_cast<uint32_t>(carry); // position is 0 here
    }
    trim(out);
    return out;
}

// x mod n by binary long division (process x bit-by-bit, keeping r < n).
inline BigUint mod(const BigUint& x, const BigUint& n) {
    BigUint r;
    r.push_back(0);
    for (size_t li = x.size(); li-- > 0;) {
        const uint32_t limb = x[li];
        for (int bit = 31; bit >= 0; --bit) {
            shl1(r, (limb >> bit) & 1);  // r < n  =>  r<<1|bit < 2n
            if (cmp(r, n) >= 0) sub_inplace(r, n);
        }
    }
    return r;
}

inline BigUint modmul(const BigUint& a, const BigUint& b, const BigUint& n) {
    return mod(mul(a, b), n);
}

// base^exp mod n, exp given as big-endian bytes. Left-to-right square-and-multiply.
inline BigUint modexp(BigUint base, std::span<const uint8_t> exp_be,
                      const BigUint& n) {
    base = mod(base, n);
    BigUint result;
    result.push_back(1);
    size_t i0 = 0;
    while (i0 < exp_be.size() && exp_be[i0] == 0) ++i0; // skip leading zero bytes
    for (size_t i = i0; i < exp_be.size(); ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            result = modmul(result, result, n);
            if (((exp_be[i] >> bit) & 1) != 0) result = modmul(result, base, n);
        }
    }
    return result; // exp == 0 → result stays 1
}

} // namespace detail::rsa

/**
 * @brief Verify an RSASSA-PKCS1-v1.5 signature over SHA-256 (RS256 / RSA-SHA256).
 *
 * @param modulus   RSA public modulus `n`, big-endian (e.g. 256 bytes for RSA-2048).
 * @param exponent  RSA public exponent `e`, big-endian (typically {0x01,0x00,0x01}).
 * @param message   The message whose signature is being verified.
 * @param signature The signature `s`, big-endian, exactly `len(modulus)` bytes.
 * @returns true iff `signature` is a valid RS256 signature over `message` under
 *          the given public key. Never throws; returns false on any malformed
 *          input (wrong length, out-of-range signature, bad padding).
 */
[[nodiscard]] inline bool rsa_pkcs1_v15_sha256_verify(
    std::span<const uint8_t> modulus,
    std::span<const uint8_t> exponent,
    std::span<const uint8_t> message,
    std::span<const uint8_t> signature) {
    using namespace detail::rsa;

    if (modulus.empty() || exponent.empty() || signature.empty()) return false;

    // k = octet length of the modulus (ignoring any leading zero bytes).
    size_t lead = 0;
    while (lead < modulus.size() && modulus[lead] == 0) ++lead;
    const size_t k = modulus.size() - lead;
    if (k == 0 || signature.size() != k) return false;

    const BigUint n = from_be_bytes(modulus);
    const BigUint s = from_be_bytes(signature);
    if (cmp(s, n) >= 0) return false; // signature representative must be < modulus

    const BigUint m = modexp(s, exponent, n);
    const std::vector<uint8_t> em = to_be_bytes(m, k); // EM = I2OSP(m, k)

    // Expected EM (EMSA-PKCS1-v1.5):
    //   0x00 0x01 || PS(0xFF…) || 0x00 || DigestInfo(SHA-256) || H
    static constexpr std::array<uint8_t, 19> kSha256DigestInfo = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

    const Sha256Digest h = sha256(message);
    const size_t tlen = kSha256DigestInfo.size() + h.size(); // 19 + 32 = 51
    if (k < tlen + 11) return false; // RFC 8017 §9.2: at least 8 octets of PS

    std::vector<uint8_t> expected(k, 0xFF);
    expected[0] = 0x00;
    expected[1] = 0x01;
    expected[k - tlen - 1] = 0x00;
    std::copy(kSha256DigestInfo.begin(), kSha256DigestInfo.end(),
              expected.begin() + static_cast<std::ptrdiff_t>(k - tlen));
    std::copy(h.begin(), h.end(),
              expected.begin() + static_cast<std::ptrdiff_t>(k - h.size()));

    // Constant-time comparison.
    uint8_t diff = 0;
    for (size_t i = 0; i < k; ++i)
        diff |= static_cast<uint8_t>(em[i] ^ expected[i]);
    return diff == 0;
}

} // namespace qbuem::crypto
