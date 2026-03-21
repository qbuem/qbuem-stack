#pragma once

/**
 * @file qbuem/crypto/chacha20_poly1305.hpp
 * @brief ChaCha20-Poly1305 AEAD (RFC 8439 / RFC 7539).
 * @ingroup qbuem_crypto
 *
 * An Authenticated Encryption with Associated Data (AEAD) construction
 * combining ChaCha20 stream encryption with Poly1305 MAC authentication.
 *
 * ### Security properties
 * - **Confidentiality**: ChaCha20 stream cipher (256-bit key)
 * - **Integrity**: Poly1305 MAC (128-bit tag)
 * - **Authenticated associated data** (AAD): authenticated but not encrypted
 * - **IND-CCA2 secure** when nonce is not reused
 *
 * ### Nonce requirements
 * A 96-bit (12-byte) nonce must be **unique** per (key, message) pair.
 * Recommended strategy: 96-bit random nonce or 32-bit fixed || 64-bit counter.
 * Reuse of a (key, nonce) pair allows a trivial forgery attack.
 *
 * ### Key construction (RFC 8439 §2.6)
 * The Poly1305 one-time key is derived from the first 32 bytes of the
 * ChaCha20 keystream at block counter 0.  Encryption starts at block 1.
 *
 * ### Usage
 * ```cpp
 * std::array<uint8_t, 32> key   = ...;
 * std::array<uint8_t, 12> nonce = ...;
 *
 * // Encrypt + authenticate
 * std::vector<uint8_t> ct(plaintext.size() + 16);
 * qbuem::crypto::chacha20_poly1305_seal(key, nonce, aad, plaintext, ct);
 *
 * // Decrypt + verify
 * std::vector<uint8_t> pt(ciphertext_without_tag.size());
 * auto ok = qbuem::crypto::chacha20_poly1305_open(key, nonce, aad, ciphertext, pt);
 * if (!ok) // authentication failed
 * ```
 *
 * ### Zero-allocation API (for hot paths)
 * ```cpp
 * // Output spans must be pre-allocated by the caller:
 * std::array<uint8_t, 16> tag;
 * qbuem::crypto::chacha20_poly1305_seal(key, nonce, aad, plaintext, ciphertext_out, tag);
 * ```
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <system_error>

#include <qbuem/crypto/chacha20.hpp>
#include <qbuem/crypto/poly1305.hpp>

namespace qbuem::crypto {

template <typename T>
using Result = std::expected<T, std::error_code>;

// ─── Key types ────────────────────────────────────────────────────────────────

using AeadKey   = ChaCha20Key;    // 32 bytes
using AeadNonce = ChaCha20Nonce;  // 12 bytes
using AeadTag   = Poly1305Tag;    // 16 bytes

// ─── Internal helpers ─────────────────────────────────────────────────────────

namespace detail::aead {

/**
 * @brief Derive the Poly1305 one-time key (RFC 8439 §2.6).
 *
 * Generates the first 32 bytes of ChaCha20 at block counter 0.
 */
[[nodiscard]] inline Poly1305Key poly_otk(const AeadKey&   key,
                                           const AeadNonce& nonce) noexcept {
    // ChaCha20 block 0 keystream
    const ChaCha20Block block0 = ChaCha20{key, nonce, 0}.keystream_block(0);
    Poly1305Key otk{};
    std::memcpy(otk.data(), block0.data(), 32);
    return otk;
}

/**
 * @brief Build the Poly1305 auth tag per RFC 8439 §2.8.
 *
 * MAC input layout (each segment padded to 16-byte boundary with zeros):
 *   AAD  || pad(AAD)  || ciphertext || pad(CT) || len(AAD) || len(CT)
 *
 * Both lengths are 64-bit little-endian.
 */
[[nodiscard]] inline AeadTag build_tag(const Poly1305Key&       otk,
                                        std::span<const uint8_t> aad,
                                        std::span<const uint8_t> ciphertext) noexcept {
    Poly1305 mac{otk};

    // Padding helper: feed up to 15 zero bytes to align to 16
    static constexpr uint8_t kZeros[16] = {};
    auto pad_to_16 = [&](size_t len) {
        const size_t rem = len % 16;
        if (rem != 0) mac.update({kZeros, 16 - rem});
    };

    mac.update(aad);
    pad_to_16(aad.size());

    mac.update(ciphertext);
    pad_to_16(ciphertext.size());

    // 64-bit LE lengths
    const uint64_t aad_len = aad.size();
    const uint64_t ct_len  = ciphertext.size();
    uint8_t lens[16];
    std::memcpy(lens + 0, &aad_len, 8);
    std::memcpy(lens + 8, &ct_len,  8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // Swap bytes for LE on big-endian host
    for (int i = 0; i < 8; ++i) { std::swap(lens[i], lens[7-i]); }
    for (int i = 8; i < 16; ++i) { std::swap(lens[i], lens[8+(15-i)]); }
#endif
    mac.update(lens);

    return mac.finalize();
}

}  // namespace detail::aead

// ─── AEAD seal (encrypt + authenticate) ──────────────────────────────────────

/**
 * @brief ChaCha20-Poly1305 encrypt and authenticate (RFC 8439 §2.8).
 *
 * @param key           32-byte secret key.
 * @param nonce         12-byte unique nonce.
 * @param aad           Additional authenticated data (not encrypted; may be empty).
 * @param plaintext     Data to encrypt.
 * @param ciphertext    Output: must be exactly `plaintext.size()` bytes.
 * @param tag           Output: 16-byte authentication tag.
 */
inline void chacha20_poly1305_seal(const AeadKey&           key,
                                    const AeadNonce&         nonce,
                                    std::span<const uint8_t> aad,
                                    std::span<const uint8_t> plaintext,
                                    std::span<uint8_t>       ciphertext,
                                    AeadTag&                 tag) noexcept {
    // Step 1: derive one-time Poly1305 key from block 0
    const Poly1305Key otk = detail::aead::poly_otk(key, nonce);

    // Step 2: encrypt plaintext with ChaCha20 starting at block counter 1
    detail::chacha::xor_stream(
        key.data(), nonce.data(), 1,
        plaintext.data(), ciphertext.data(), plaintext.size());

    // Step 3: compute authentication tag over (AAD, ciphertext)
    tag = detail::aead::build_tag(otk, aad, ciphertext);
}

/**
 * @brief Variant that returns the tag by value and writes ciphertext in-place.
 *
 * @p data is both input (plaintext) and output (ciphertext) — in-place XOR.
 */
[[nodiscard]] inline AeadTag
chacha20_poly1305_seal(const AeadKey&           key,
                        const AeadNonce&         nonce,
                        std::span<const uint8_t> aad,
                        std::span<uint8_t>       data) noexcept {
    const Poly1305Key otk = detail::aead::poly_otk(key, nonce);
    detail::chacha::xor_stream(
        key.data(), nonce.data(), 1,
        data.data(), data.data(), data.size());
    return detail::aead::build_tag(otk, aad, data);
}

// ─── AEAD open (verify + decrypt) ────────────────────────────────────────────

/**
 * @brief ChaCha20-Poly1305 verify and decrypt (RFC 8439 §2.8).
 *
 * Authentication is verified **before** decryption to prevent padding-oracle
 * and error-side-channel attacks.
 *
 * @param key           32-byte secret key.
 * @param nonce         12-byte nonce (must match the value used during seal).
 * @param aad           Additional authenticated data.
 * @param ciphertext    Encrypted data (does NOT include the tag).
 * @param tag           16-byte authentication tag from seal.
 * @param plaintext     Output: must be exactly `ciphertext.size()` bytes.
 * @returns `{}` on success, or `errc::bad_message` if authentication fails.
 */
[[nodiscard]] inline Result<void>
chacha20_poly1305_open(const AeadKey&           key,
                        const AeadNonce&         nonce,
                        std::span<const uint8_t> aad,
                        std::span<const uint8_t> ciphertext,
                        const AeadTag&           tag,
                        std::span<uint8_t>       plaintext) noexcept {
    // Step 1: derive one-time key
    const Poly1305Key otk = detail::aead::poly_otk(key, nonce);

    // Step 2: verify tag BEFORE decrypting (authenticate-then-decrypt)
    const AeadTag expected = detail::aead::build_tag(otk, aad, ciphertext);
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i)
        diff |= expected[i] ^ tag[i];
    if (diff != 0)
        return std::unexpected(std::make_error_code(std::errc::bad_message));

    // Step 3: decrypt (same as encrypt — XOR is symmetric)
    detail::chacha::xor_stream(
        key.data(), nonce.data(), 1,
        ciphertext.data(), plaintext.data(), ciphertext.size());

    return {};
}

/**
 * @brief In-place open: decrypt ciphertext in @p data, verify tag.
 *
 * @p data is both input (ciphertext) and output (plaintext).
 */
[[nodiscard]] inline Result<void>
chacha20_poly1305_open(const AeadKey&           key,
                        const AeadNonce&         nonce,
                        std::span<const uint8_t> aad,
                        std::span<uint8_t>       data,
                        const AeadTag&           tag) noexcept {
    const Poly1305Key otk = detail::aead::poly_otk(key, nonce);
    const AeadTag expected = detail::aead::build_tag(otk, aad, data);
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i)
        diff |= expected[i] ^ tag[i];
    if (diff != 0)
        return std::unexpected(std::make_error_code(std::errc::bad_message));

    detail::chacha::xor_stream(
        key.data(), nonce.data(), 1,
        data.data(), data.data(), data.size());
    return {};
}

}  // namespace qbuem::crypto
