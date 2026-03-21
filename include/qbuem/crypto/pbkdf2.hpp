#pragma once

/**
 * @file qbuem/crypto/pbkdf2.hpp
 * @brief PBKDF2 key derivation function (RFC 2898 / PKCS#5 §5.2).
 * @ingroup qbuem_crypto
 *
 * PBKDF2 stretches a password into a key of arbitrary length by iterating
 * an HMAC-based PRF (PBKDF2-HMAC-SHA-256 or PBKDF2-HMAC-SHA-512).
 *
 * ### Zero-allocation design
 * The caller provides the output buffer via `std::span<uint8_t>`.
 * All intermediate state is stack-allocated (`std::array`).
 * No heap allocation occurs.
 *
 * ### Usage
 * ```cpp
 * // Derive a 32-byte key with 600 000 iterations (OWASP 2023 minimum for SHA-256)
 * std::array<uint8_t, 32> dk;
 * qbuem::crypto::pbkdf2_hmac_sha256(password, salt, 600'000, dk);
 *
 * // Derive variable-length output into a stack buffer
 * std::array<uint8_t, 64> dk64;
 * qbuem::crypto::pbkdf2_hmac_sha256(password, salt, 100'000, dk64);
 * ```
 *
 * ### Performance
 * Each iteration requires two HMAC-SHA-256 compressions (inner + outer).
 * For 600 000 iterations on a 4 GHz CPU with SHA-NI: ~30 ms per key.
 *
 * ### Security
 * - Minimum recommended iterations: 600 000 (OWASP 2023, SHA-256)
 * - Use a 16-byte (128-bit) randomly generated salt.
 * - Output key length ≥ 16 bytes for symmetric key derivation.
 */

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <qbuem/crypto/hmac.hpp>

namespace qbuem::crypto {

namespace detail::pbkdf2 {

/**
 * @brief Compute one PBKDF2 block: F(password, salt, c, i).
 *
 * F(P, S, c, i) = U1 ⊕ U2 ⊕ ... ⊕ Uc
 *   U1 = PRF(P, S || INT(i))
 *   Uj = PRF(P, U(j-1))
 *
 * @tparam H       Hash wrapper type (Sha256Wrapper or Sha512Wrapper).
 * @param hmac     Pre-keyed HMAC context.
 * @param salt     Salt bytes.
 * @param counter  Block index i (1-based, big-endian uint32).
 * @param iters    Iteration count c.
 * @param out      Output buffer of exactly kDigestSize bytes.
 */
template <HashContext H>
inline void compute_block(HmacContext<H>&          hmac,
                           std::span<const uint8_t> salt,
                           uint32_t                 counter,
                           uint32_t                 iters,
                           std::span<uint8_t>       out) noexcept {
    static constexpr size_t dlen = H::kDigestSize;

    // U1 = PRF(P, S || INT(counter))
    hmac.reset();
    hmac.update(salt);
    const uint8_t cnt[4] = {
        static_cast<uint8_t>(counter >> 24),
        static_cast<uint8_t>(counter >> 16),
        static_cast<uint8_t>(counter >>  8),
        static_cast<uint8_t>(counter),
    };
    hmac.update(cnt);
    typename H::DigestType u = hmac.finalize();

    // Accumulate XOR into out
    std::memcpy(out.data(), u.data(), dlen);

    // U2 … Uc
    for (uint32_t j = 1; j < iters; ++j) {
        hmac.reset();
        hmac.update({u.data(), dlen});
        u = hmac.finalize();
        for (size_t k = 0; k < dlen; ++k)
            out[k] ^= u[k];
    }
}

}  // namespace detail::pbkdf2

// ─── PBKDF2-HMAC-SHA-256 ─────────────────────────────────────────────────────

/**
 * @brief Derive a key using PBKDF2-HMAC-SHA-256 (RFC 2898).
 *
 * @param password   The password (any length).
 * @param salt       A randomly generated salt (≥ 16 bytes recommended).
 * @param iterations Iteration count (≥ 600 000 recommended per OWASP 2023).
 * @param out        Output buffer; receives `out.size()` derived key bytes.
 *                   May be any length from 1 to (2^32 - 1) × 32 bytes.
 */
inline void pbkdf2_hmac_sha256(std::span<const uint8_t> password,
                                std::span<const uint8_t> salt,
                                uint32_t                 iterations,
                                std::span<uint8_t>       out) noexcept {
    static constexpr size_t dlen = Sha256Wrapper::kDigestSize;  // 32

    HmacSha256 hmac{password};

    const size_t   full_blocks = out.size() / dlen;
    const size_t   remainder   = out.size() % dlen;
    uint32_t       counter     = 1;

    // Full blocks
    for (size_t b = 0; b < full_blocks; ++b, ++counter) {
        detail::pbkdf2::compute_block<Sha256Wrapper>(
            hmac, salt, counter, iterations,
            out.subspan(b * dlen, dlen));
    }

    // Partial last block
    if (remainder > 0) {
        std::array<uint8_t, dlen> block{};
        detail::pbkdf2::compute_block<Sha256Wrapper>(
            hmac, salt, counter, iterations, block);
        std::memcpy(out.data() + full_blocks * dlen, block.data(), remainder);
    }
}

/** @brief Overload accepting string_view password and salt. */
inline void pbkdf2_hmac_sha256(std::string_view   password,
                                std::string_view   salt,
                                uint32_t           iterations,
                                std::span<uint8_t> out) noexcept {
    pbkdf2_hmac_sha256(
        {reinterpret_cast<const uint8_t*>(password.data()), password.size()},
        {reinterpret_cast<const uint8_t*>(salt.data()),     salt.size()},
        iterations, out);
}

/**
 * @brief Derive a fixed-length key via PBKDF2-HMAC-SHA-256.
 *
 * Template overload that returns a compile-time-sized array.
 *
 * @tparam N  Desired output length in bytes.
 *
 * ```cpp
 * auto dk = qbuem::crypto::pbkdf2_hmac_sha256<32>(pw, salt, 600'000);
 * ```
 */
template <size_t N>
[[nodiscard]] inline std::array<uint8_t, N>
pbkdf2_hmac_sha256(std::span<const uint8_t> password,
                   std::span<const uint8_t> salt,
                   uint32_t                 iterations) noexcept {
    std::array<uint8_t, N> out{};
    pbkdf2_hmac_sha256(password, salt, iterations, out);
    return out;
}

template <size_t N>
[[nodiscard]] inline std::array<uint8_t, N>
pbkdf2_hmac_sha256(std::string_view password,
                   std::string_view salt,
                   uint32_t         iterations) noexcept {
    std::array<uint8_t, N> out{};
    pbkdf2_hmac_sha256(password, salt, iterations, out);
    return out;
}

// ─── PBKDF2-HMAC-SHA-512 ─────────────────────────────────────────────────────

/**
 * @brief Derive a key using PBKDF2-HMAC-SHA-512 (RFC 2898).
 *
 * Recommended iterations: 210 000 (OWASP 2023, SHA-512 is ~2× slower than SHA-256).
 */
inline void pbkdf2_hmac_sha512(std::span<const uint8_t> password,
                                std::span<const uint8_t> salt,
                                uint32_t                 iterations,
                                std::span<uint8_t>       out) noexcept {
    static constexpr size_t dlen = Sha512Wrapper::kDigestSize;  // 64

    HmacSha512 hmac{password};

    const size_t full_blocks = out.size() / dlen;
    const size_t remainder   = out.size() % dlen;
    uint32_t     counter     = 1;

    for (size_t b = 0; b < full_blocks; ++b, ++counter) {
        detail::pbkdf2::compute_block<Sha512Wrapper>(
            hmac, salt, counter, iterations,
            out.subspan(b * dlen, dlen));
    }

    if (remainder > 0) {
        std::array<uint8_t, dlen> block{};
        detail::pbkdf2::compute_block<Sha512Wrapper>(
            hmac, salt, counter, iterations, block);
        std::memcpy(out.data() + full_blocks * dlen, block.data(), remainder);
    }
}

inline void pbkdf2_hmac_sha512(std::string_view   password,
                                std::string_view   salt,
                                uint32_t           iterations,
                                std::span<uint8_t> out) noexcept {
    pbkdf2_hmac_sha512(
        {reinterpret_cast<const uint8_t*>(password.data()), password.size()},
        {reinterpret_cast<const uint8_t*>(salt.data()),     salt.size()},
        iterations, out);
}

template <size_t N>
[[nodiscard]] inline std::array<uint8_t, N>
pbkdf2_hmac_sha512(std::span<const uint8_t> password,
                   std::span<const uint8_t> salt,
                   uint32_t                 iterations) noexcept {
    std::array<uint8_t, N> out{};
    pbkdf2_hmac_sha512(password, salt, iterations, out);
    return out;
}

template <size_t N>
[[nodiscard]] inline std::array<uint8_t, N>
pbkdf2_hmac_sha512(std::string_view password,
                   std::string_view salt,
                   uint32_t         iterations) noexcept {
    std::array<uint8_t, N> out{};
    pbkdf2_hmac_sha512(password, salt, iterations, out);
    return out;
}

}  // namespace qbuem::crypto
