#pragma once

/**
 * @file qbuem/crypto/hkdf.hpp
 * @brief HKDF — HMAC-based Key Derivation Function (RFC 5869).
 * @ingroup qbuem_crypto
 *
 * HKDF is a two-phase KDF:
 *  1. **Extract**: condense potentially low-entropy input key material (IKM)
 *     together with a salt into a fixed-length pseudo-random key (PRK).
 *  2. **Expand**: expand the PRK into output keying material (OKM) of the
 *     desired length using an optional context label.
 *
 * Suitable for deriving sub-keys from a shared secret (e.g. ECDH output),
 * session keys, and deterministic nonces.
 *
 * ### Zero-allocation design
 * All intermediate state is stack-allocated.  The caller provides the
 * output span; no heap allocation occurs.
 *
 * ### Usage
 * ```cpp
 * // Full HKDF-SHA-256 (extract + expand)
 * std::array<uint8_t, 32> okm;
 * qbuem::crypto::hkdf_sha256(ikm, salt, info, okm);
 *
 * // Extract only (produce PRK)
 * auto prk = qbuem::crypto::hkdf_extract_sha256(salt, ikm);
 *
 * // Expand only (produce OKM from existing PRK)
 * std::array<uint8_t, 64> okm64;
 * qbuem::crypto::hkdf_expand_sha256(prk, info, okm64);
 * ```
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <qbuem/crypto/hmac.hpp>

namespace qbuem::crypto {

// ─── HKDF-Extract ────────────────────────────────────────────────────────────

/**
 * @brief HKDF-Extract (RFC 5869 §2.2).
 *
 * PRK = HMAC-Hash(salt, IKM)
 *
 * If @p salt is empty, a zero-filled block of HashLen bytes is used.
 *
 * @param salt  Optional salt (may be empty).
 * @param ikm   Input key material.
 * @returns PRK — a HashLen-byte pseudo-random key.
 */
[[nodiscard]] inline Sha256Digest
hkdf_extract_sha256(std::span<const uint8_t> salt,
                    std::span<const uint8_t> ikm) noexcept {
    if (salt.empty()) {
        // Zero-length salt → use a block of zeros as the salt
        static constexpr std::array<uint8_t, 32> kZeroSalt{};
        return hmac_sha256(kZeroSalt, ikm);
    }
    return hmac_sha256(salt, ikm);
}

[[nodiscard]] inline Sha512Digest
hkdf_extract_sha512(std::span<const uint8_t> salt,
                    std::span<const uint8_t> ikm) noexcept {
    if (salt.empty()) {
        static constexpr std::array<uint8_t, 64> kZeroSalt{};
        return hmac_sha512(kZeroSalt, ikm);
    }
    return hmac_sha512(salt, ikm);
}

// ─── HKDF-Expand ─────────────────────────────────────────────────────────────

/**
 * @brief HKDF-Expand (RFC 5869 §2.3).
 *
 * OKM = T(1) || T(2) || ... || T(N)
 * T(0) = ""
 * T(i) = HMAC-Hash(PRK, T(i-1) || info || i)
 *
 * @param prk   Pseudo-random key from HKDF-Extract (must be HashLen bytes).
 * @param info  Context and application-specific information (may be empty).
 * @param out   Output buffer; max length = 255 × HashLen.
 */
inline void hkdf_expand_sha256(const Sha256Digest&      prk,
                                std::span<const uint8_t> info,
                                std::span<uint8_t>       out) noexcept {
    static constexpr size_t dlen = 32;

    HmacSha256 hmac{prk};

    std::array<uint8_t, dlen> t{};  // T(i), starts as T(0) = ""
    bool has_prev = false;

    size_t   offset  = 0;
    uint8_t  counter = 1;

    while (offset < out.size()) {
        hmac.reset();
        if (has_prev) hmac.update({t.data(), dlen});
        hmac.update(info);
        hmac.update({&counter, 1});
        t = hmac.finalize();
        has_prev = true;

        const size_t copy = std::min(dlen, out.size() - offset);
        std::memcpy(out.data() + offset, t.data(), copy);
        offset += copy;
        ++counter;
    }
}

inline void hkdf_expand_sha512(const Sha512Digest&      prk,
                                std::span<const uint8_t> info,
                                std::span<uint8_t>       out) noexcept {
    static constexpr size_t dlen = 64;

    HmacSha512 hmac{prk};

    std::array<uint8_t, dlen> t{};
    bool has_prev = false;

    size_t  offset  = 0;
    uint8_t counter = 1;

    while (offset < out.size()) {
        hmac.reset();
        if (has_prev) hmac.update({t.data(), dlen});
        hmac.update(info);
        hmac.update({&counter, 1});
        t = hmac.finalize();
        has_prev = true;

        const size_t copy = std::min(dlen, out.size() - offset);
        std::memcpy(out.data() + offset, t.data(), copy);
        offset += copy;
        ++counter;
    }
}

// ─── HKDF (combined Extract + Expand) ────────────────────────────────────────

/**
 * @brief Full HKDF-SHA-256: extract IKM with salt, then expand with info.
 *
 * @param ikm   Input key material (e.g. ECDH shared secret).
 * @param salt  Optional salt; pass empty span to use zeros.
 * @param info  Context label (e.g. "handshake keys").
 * @param out   Output buffer for derived keying material.
 */
inline void hkdf_sha256(std::span<const uint8_t> ikm,
                         std::span<const uint8_t> salt,
                         std::span<const uint8_t> info,
                         std::span<uint8_t>       out) noexcept {
    const Sha256Digest prk = hkdf_extract_sha256(salt, ikm);
    hkdf_expand_sha256(prk, info, out);
}

inline void hkdf_sha256(std::string_view   ikm,
                         std::string_view   salt,
                         std::string_view   info,
                         std::span<uint8_t> out) noexcept {
    hkdf_sha256(
        {reinterpret_cast<const uint8_t*>(ikm.data()),  ikm.size()},
        {reinterpret_cast<const uint8_t*>(salt.data()), salt.size()},
        {reinterpret_cast<const uint8_t*>(info.data()), info.size()},
        out);
}

inline void hkdf_sha512(std::span<const uint8_t> ikm,
                         std::span<const uint8_t> salt,
                         std::span<const uint8_t> info,
                         std::span<uint8_t>       out) noexcept {
    const Sha512Digest prk = hkdf_extract_sha512(salt, ikm);
    hkdf_expand_sha512(prk, info, out);
}

/**
 * @brief HKDF-SHA-256 returning a compile-time-sized key array.
 *
 * ```cpp
 * auto key = qbuem::crypto::hkdf_sha256<32>(shared_secret, salt, "encryption key");
 * ```
 */
template <size_t N>
[[nodiscard]] inline std::array<uint8_t, N>
hkdf_sha256(std::span<const uint8_t> ikm,
            std::span<const uint8_t> salt,
            std::span<const uint8_t> info) noexcept {
    std::array<uint8_t, N> out{};
    hkdf_sha256(ikm, salt, info, out);
    return out;
}

template <size_t N>
[[nodiscard]] inline std::array<uint8_t, N>
hkdf_sha256(std::string_view ikm,
            std::string_view salt,
            std::string_view info) noexcept {
    std::array<uint8_t, N> out{};
    hkdf_sha256(ikm, salt, info, out);
    return out;
}

template <size_t N>
[[nodiscard]] inline std::array<uint8_t, N>
hkdf_sha512(std::span<const uint8_t> ikm,
            std::span<const uint8_t> salt,
            std::span<const uint8_t> info) noexcept {
    std::array<uint8_t, N> out{};
    hkdf_sha512(ikm, salt, info, out);
    return out;
}

}  // namespace qbuem::crypto
