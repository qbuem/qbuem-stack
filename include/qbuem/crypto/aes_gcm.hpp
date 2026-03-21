#pragma once

/**
 * @file qbuem/crypto/aes_gcm.hpp
 * @brief AES-128-GCM and AES-256-GCM AEAD using hardware AES-NI + CLMUL.
 * @ingroup qbuem_crypto
 *
 * ## Design rationale
 *
 * Software AES (table-based implementations) is vulnerable to cache-timing
 * side-channel attacks.  This header exposes AES-GCM **only** when hardware
 * AES-NI is available, ensuring constant-time operation.
 *
 * On platforms without AES-NI, all functions return
 * `errc::function_not_supported` — no unsafe software fallback is provided.
 * Use ChaCha20-Poly1305 (see chacha20_poly1305.hpp) as the constant-time
 * alternative when AES hardware is unavailable.
 *
 * ### Hardware requirements
 * | Feature | x86-64 macro      | ARM macro                    |
 * |---------|-------------------|------------------------------|
 * | AES     | `__AES__`         | `__ARM_FEATURE_AES`          |
 * | CLMUL   | `__PCLMUL__`      | `__ARM_FEATURE_AES` (PMULL)  |
 *
 * ### Algorithm overview
 * - **Key schedule**: AES-128 (10 rounds) or AES-256 (14 rounds) using `_mm_aeskeygenassist_si128`
 * - **CTR encryption**: `_mm_aesenc_si128` × 9/13 + `_mm_aesenclast_si128`
 * - **GHASH**: 128-bit polynomial multiplication via `_mm_clmulepi64_si128`
 *
 * ### Usage
 * ```cpp
 * if (!qbuem::crypto::has_aes_ni()) {
 *     // Fall back to ChaCha20-Poly1305
 * }
 *
 * auto ctx = qbuem::crypto::AesGcm256::create(key);
 * if (!ctx) handle_error(ctx.error());
 *
 * AeadTag tag;
 * ctx->seal(nonce, aad, plaintext, ciphertext_out, tag);
 * auto ok = ctx->open(nonce, aad, ciphertext, tag, plaintext_out);
 * ```
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <system_error>

// AES-NI + CLMUL detection (x86-64)
#if (defined(__AES__) && defined(__PCLMUL__)) && \
    (defined(__x86_64__) || defined(__i386__)) && \
    __has_include(<wmmintrin.h>)
#  include <immintrin.h>
#  include <wmmintrin.h>
#  define QBUEM_AES_NI 1
#endif

// ARM AES + PMULL (AArch64)
#if defined(__ARM_FEATURE_AES) && defined(__aarch64__) && \
    __has_include(<arm_neon.h>)
#  include <arm_neon.h>
#  define QBUEM_AES_ARM 1
#endif

namespace qbuem::crypto {

template <typename T>
using Result = std::expected<T, std::error_code>;

using AesGcmNonce = std::array<uint8_t, 12>;
using AesGcmTag   = std::array<uint8_t, 16>;

// ─── Runtime capability detection ────────────────────────────────────────────

/**
 * @brief Returns true if hardware AES acceleration is available at runtime.
 *
 * Checks CPUID on x86 or compile-time macros on ARM.
 */
[[nodiscard]] inline bool has_aes_ni() noexcept {
#if defined(QBUEM_AES_NI)
    static const bool cached = []() noexcept -> bool {
        uint32_t ecx = 0;
        __asm__ volatile("cpuid" : "=c"(ecx) : "a"(1u), "c"(0u) : "ebx", "edx");
        // ECX bit 25 = AES-NI, bit 1 = PCLMULQDQ
        return ((ecx >> 25) & 1u) && ((ecx >> 1) & 1u);
    }();
    return cached;
#elif defined(QBUEM_AES_ARM)
    return true;  // Compile-time guaranteed
#else
    return false;
#endif
}

// ─── AES key schedule ─────────────────────────────────────────────────────────

namespace detail::aes {

#if defined(QBUEM_AES_NI)

// AES-128: 11 round keys (10 rounds)
using Ks128 = std::array<__m128i, 11>;
// AES-256: 15 round keys (14 rounds)
using Ks256 = std::array<__m128i, 15>;

/**
 * @brief AES-128 key schedule expansion (FIPS 197).
 */
inline void keyschedule_128(const uint8_t* key, Ks128& ks) noexcept {
    ks[0] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key));

#define AES128_KS_STEP(i, rcon) \
    do { \
        __m128i t = _mm_aeskeygenassist_si128(ks[i], rcon); \
        t = _mm_shuffle_epi32(t, 0xFF); \
        __m128i k = ks[i]; \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4)); \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 8)); \
        ks[i + 1] = _mm_xor_si128(k, t); \
    } while (0)

    AES128_KS_STEP(0,  0x01);
    AES128_KS_STEP(1,  0x02);
    AES128_KS_STEP(2,  0x04);
    AES128_KS_STEP(3,  0x08);
    AES128_KS_STEP(4,  0x10);
    AES128_KS_STEP(5,  0x20);
    AES128_KS_STEP(6,  0x40);
    AES128_KS_STEP(7,  0x80);
    AES128_KS_STEP(8,  0x1b);
    AES128_KS_STEP(9,  0x36);
#undef AES128_KS_STEP
}

/**
 * @brief AES-256 key schedule expansion.
 */
inline void keyschedule_256(const uint8_t* key, Ks256& ks) noexcept {
    ks[0] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key));
    ks[1] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key + 16));

    auto key_expand_256a = [](const __m128i& prev, __m128i& out, int rcon) noexcept {
        __m128i t = _mm_aeskeygenassist_si128(prev, rcon);
        t = _mm_shuffle_epi32(t, 0xFF);
        __m128i k = out;
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
        k = _mm_xor_si128(k, _mm_slli_si128(k, 8));
        out = _mm_xor_si128(k, t);
    };
    auto key_expand_256b = [](const __m128i& prev, __m128i& out) noexcept {
        __m128i t = _mm_aeskeygenassist_si128(prev, 0x00);
        t = _mm_shuffle_epi32(t, 0xAA);
        __m128i k = out;
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
        k = _mm_xor_si128(k, _mm_slli_si128(k, 8));
        out = _mm_xor_si128(k, t);
    };

    key_expand_256a(ks[1], ks[2], 0x01);
    key_expand_256b(ks[2], ks[3]);
    key_expand_256a(ks[3], ks[4], 0x02);
    key_expand_256b(ks[4], ks[5]);
    key_expand_256a(ks[5], ks[6], 0x04);
    key_expand_256b(ks[6], ks[7]);
    key_expand_256a(ks[7], ks[8], 0x08);
    key_expand_256b(ks[8], ks[9]);
    key_expand_256a(ks[9], ks[10], 0x10);
    key_expand_256b(ks[10], ks[11]);
    key_expand_256a(ks[11], ks[12], 0x20);
    key_expand_256b(ks[12], ks[13]);
    key_expand_256a(ks[13], ks[14], 0x40);
}

// ─── AES block encryption ─────────────────────────────────────────────────────

template <size_t Rounds, typename KsType>
[[nodiscard]] inline __m128i aes_encrypt_block(const KsType& ks, __m128i block) noexcept {
    block = _mm_xor_si128(block, ks[0]);
    for (size_t i = 1; i < Rounds; ++i)
        block = _mm_aesenc_si128(block, ks[i]);
    return _mm_aesenclast_si128(block, ks[Rounds]);
}

// ─── GHASH ────────────────────────────────────────────────────────────────────

/**
 * @brief GCM field multiplication in GF(2^128) using PCLMULQDQ.
 *
 * Implements the "reflected" polynomial multiplication as per NIST SP 800-38D.
 */
[[nodiscard]] inline __m128i ghash_mul(__m128i X, __m128i H) noexcept {
    const __m128i A  = _mm_clmulepi64_si128(X, H, 0x00);
    const __m128i B  = _mm_clmulepi64_si128(X, H, 0x11);
    const __m128i C  = _mm_clmulepi64_si128(X, H, 0x01);
    const __m128i D  = _mm_clmulepi64_si128(X, H, 0x10);
    const __m128i CD = _mm_xor_si128(C, D);

    // Combine into 256-bit product (split into lo/hi 128 bits)
    const __m128i lo = _mm_xor_si128(A, _mm_slli_si128(CD, 8));
    const __m128i hi = _mm_xor_si128(B, _mm_srli_si128(CD, 8));

    // Reduction mod x^128 + x^7 + x^2 + x + 1 (GCM polynomial)
    const __m128i p = _mm_set_epi32(0, 0, 1, 0x87);  // 0x87 = x^7+x^2+x+1
    const __m128i t1 = _mm_clmulepi64_si128(hi, p, 0x01);
    const __m128i t2 = _mm_xor_si128(lo, _mm_slli_si128(t1, 8));
    const __m128i t3 = _mm_xor_si128(hi, _mm_srli_si128(t1, 8));
    const __m128i t4 = _mm_clmulepi64_si128(t3, p, 0x00);
    return _mm_xor_si128(_mm_xor_si128(t2, t4), t3);
}

/**
 * @brief Byte-swap a 128-bit value (big-endian ↔ little-endian).
 */
[[nodiscard]] inline __m128i bswap128(__m128i v) noexcept {
    const __m128i shuf = _mm_set_epi8(
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    return _mm_shuffle_epi8(v, shuf);
}

// ─── CTR + GHASH core ─────────────────────────────────────────────────────────

/**
 * @brief AES-GCM encrypt/decrypt core.
 *
 * @tparam Rounds  Number of AES rounds (10 for 128-bit, 14 for 256-bit).
 * @tparam KsType  Key schedule array type.
 */
template <size_t Rounds, typename KsType>
inline void gcm_crypt(const KsType&            ks,
                       __m128i&                 ghash_acc,
                       const __m128i&           H,
                       const std::array<uint8_t, 16>& j0,
                       std::span<const uint8_t> src,
                       std::span<uint8_t>       dst,
                       bool                     is_encrypt) noexcept {
    // Initial counter = J0 + 1 (big-endian counter increment)
    uint8_t ctr_bytes[16];
    std::memcpy(ctr_bytes, j0.data(), 16);

    auto inc_ctr = [](uint8_t* c) noexcept {
        // Increment the 32-bit counter field (bytes 12–15, big-endian)
        for (int i = 15; i >= 12; --i) {
            if (++c[i] != 0) break;
        }
    };
    inc_ctr(ctr_bytes);  // Start at J0+1

    const size_t blocks    = src.size() / 16;
    const size_t remainder = src.size() % 16;

    for (size_t b = 0; b < blocks; ++b) {
        const __m128i ctr  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ctr_bytes));
        const __m128i ks_block = aes_encrypt_block<Rounds>(ks, ctr);

        const __m128i plain = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(src.data() + b * 16));
        const __m128i cipher = _mm_xor_si128(plain, ks_block);

        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst.data() + b * 16), cipher);

        // GHASH over ciphertext
        const __m128i ct_be = bswap128(is_encrypt ? cipher : plain);
        ghash_acc = ghash_mul(_mm_xor_si128(ghash_acc, ct_be), H);

        inc_ctr(ctr_bytes);
    }

    // Partial final block
    if (remainder > 0) {
        const __m128i ctr = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ctr_bytes));
        const __m128i ks_block = aes_encrypt_block<Rounds>(ks, ctr);

        alignas(16) uint8_t buf[16] = {};
        std::memcpy(buf, src.data() + blocks * 16, remainder);
        const __m128i plain = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf));
        const __m128i cipher = _mm_xor_si128(plain, ks_block);

        alignas(16) uint8_t out_buf[16];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out_buf), cipher);
        std::memcpy(dst.data() + blocks * 16, out_buf, remainder);

        // GHASH: pad partial ciphertext block with zeros
        alignas(16) uint8_t ct_buf[16] = {};
        std::memcpy(ct_buf, is_encrypt ? out_buf : buf, remainder);
        const __m128i ct_be = bswap128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ct_buf)));
        ghash_acc = ghash_mul(_mm_xor_si128(ghash_acc, ct_be), H);
    }
}

#endif  // QBUEM_AES_NI

}  // namespace detail::aes

// ─── AES-GCM context ─────────────────────────────────────────────────────────

/**
 * @brief AES-GCM AEAD context with a pre-expanded key schedule.
 *
 * @tparam KeyBytes  16 for AES-128-GCM, 32 for AES-256-GCM.
 *
 * Construct via the static `create()` factory, which returns an error if
 * hardware AES-NI is not available.
 */
template <size_t KeyBytes>
class AesGcm {
    static_assert(KeyBytes == 16 || KeyBytes == 32,
                  "AES key must be 128 or 256 bits (16 or 32 bytes)");
    static constexpr size_t kRounds = (KeyBytes == 16) ? 10u : 14u;

public:
    using KeyArray = std::array<uint8_t, KeyBytes>;

    /**
     * @brief Factory: create an AesGcm context.
     *
     * @returns the context, or `errc::function_not_supported` if AES-NI is unavailable.
     */
    [[nodiscard]] static Result<AesGcm> create(const KeyArray& key) noexcept {
        if (!has_aes_ni())
            return std::unexpected(
                std::make_error_code(std::errc::function_not_supported));
        AesGcm ctx;
        ctx.init(key);
        return ctx;
    }

    /**
     * @brief Encrypt and authenticate.
     *
     * @param nonce        12-byte nonce (must be unique per message).
     * @param aad          Additional authenticated data (may be empty).
     * @param plaintext    Input data.
     * @param ciphertext   Output: same size as @p plaintext.
     * @param tag          Output: 16-byte authentication tag.
     */
    void seal(const AesGcmNonce&       nonce,
              std::span<const uint8_t> aad,
              std::span<const uint8_t> plaintext,
              std::span<uint8_t>       ciphertext,
              AesGcmTag&               tag) const noexcept {
#if defined(QBUEM_AES_NI)
        seal_impl(nonce, aad, plaintext, ciphertext, tag);
#else
        (void)nonce; (void)aad; (void)plaintext; (void)ciphertext; (void)tag;
#endif
    }

    /**
     * @brief Verify and decrypt.
     *
     * Authentication is checked before decryption to prevent oracle attacks.
     *
     * @returns `{}` on success, `errc::bad_message` if the tag is invalid.
     */
    [[nodiscard]] Result<void>
    open(const AesGcmNonce&       nonce,
         std::span<const uint8_t> aad,
         std::span<const uint8_t> ciphertext,
         const AesGcmTag&         tag,
         std::span<uint8_t>       plaintext) const noexcept {
#if defined(QBUEM_AES_NI)
        return open_impl(nonce, aad, ciphertext, tag, plaintext);
#else
        (void)nonce; (void)aad; (void)ciphertext; (void)tag; (void)plaintext;
        return std::unexpected(std::make_error_code(std::errc::function_not_supported));
#endif
    }

private:
    AesGcm() = default;

#if defined(QBUEM_AES_NI)
    using KsType = std::conditional_t<KeyBytes == 16,
                                       detail::aes::Ks128,
                                       detail::aes::Ks256>;
    KsType   ks_{};
    __m128i  H_{};  // Hash subkey: AES(K, 0)

    void init(const KeyArray& key) noexcept {
        if constexpr (KeyBytes == 16)
            detail::aes::keyschedule_128(key.data(), ks_);
        else
            detail::aes::keyschedule_256(key.data(), ks_);

        // Hash subkey H = AES_K(0)
        H_ = detail::aes::bswap128(
            detail::aes::aes_encrypt_block<kRounds>(ks_, _mm_setzero_si128()));
    }

    std::array<uint8_t, 16> compute_j0(const AesGcmNonce& nonce) const noexcept {
        // J0 = nonce || 0x00000001 (96-bit nonce, 32-bit counter = 1)
        std::array<uint8_t, 16> j0{};
        std::memcpy(j0.data(), nonce.data(), 12);
        j0[15] = 0x01u;
        return j0;
    }

    __m128i ghash_aad(std::span<const uint8_t> aad) const noexcept {
        __m128i acc = _mm_setzero_si128();
        const size_t blocks    = aad.size() / 16;
        const size_t remainder = aad.size() % 16;

        for (size_t b = 0; b < blocks; ++b) {
            const __m128i v = detail::aes::bswap128(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(aad.data() + b * 16)));
            acc = detail::aes::ghash_mul(_mm_xor_si128(acc, v), H_);
        }
        if (remainder > 0) {
            alignas(16) uint8_t buf[16] = {};
            std::memcpy(buf, aad.data() + blocks * 16, remainder);
            const __m128i v = detail::aes::bswap128(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf)));
            acc = detail::aes::ghash_mul(_mm_xor_si128(acc, v), H_);
        }
        return acc;
    }

    AesGcmTag compute_tag(__m128i          ghash_acc,
                           size_t           aad_len,
                           size_t           ct_len,
                           const std::array<uint8_t, 16>& j0) const noexcept {
        // GHASH length block: [len(A) in bits || len(C) in bits] (big-endian 64+64)
        const uint64_t aad_bits = aad_len * 8u;
        const uint64_t ct_bits  = ct_len  * 8u;
        alignas(16) uint8_t len_block[16];
        // Big-endian storage
        for (int i = 7; i >= 0; --i)
            len_block[    7 - i] = static_cast<uint8_t>(aad_bits >> (i * 8));
        for (int i = 7; i >= 0; --i)
            len_block[8 + 7 - i] = static_cast<uint8_t>(ct_bits  >> (i * 8));

        const __m128i len_v = detail::aes::bswap128(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(len_block)));
        ghash_acc = detail::aes::ghash_mul(_mm_xor_si128(ghash_acc, len_v), H_);

        // S = GCTR(K, J0, GHASH output)
        const __m128i j0v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(j0.data()));
        const __m128i s = detail::aes::bswap128(
            _mm_xor_si128(
                detail::aes::aes_encrypt_block<kRounds>(ks_, j0v),
                detail::aes::bswap128(ghash_acc)));

        AesGcmTag tag{};
        _mm_storeu_si128(reinterpret_cast<__m128i*>(tag.data()), s);
        return tag;
    }

    void seal_impl(const AesGcmNonce&       nonce,
                   std::span<const uint8_t> aad,
                   std::span<const uint8_t> plaintext,
                   std::span<uint8_t>       ciphertext,
                   AesGcmTag&               tag) const noexcept {
        const auto j0 = compute_j0(nonce);
        __m128i acc = ghash_aad(aad);
        detail::aes::gcm_crypt<kRounds>(ks_, acc, H_, j0, plaintext, ciphertext, true);
        tag = compute_tag(acc, aad.size(), ciphertext.size(), j0);
    }

    [[nodiscard]] Result<void>
    open_impl(const AesGcmNonce&       nonce,
              std::span<const uint8_t> aad,
              std::span<const uint8_t> ciphertext,
              const AesGcmTag&         tag,
              std::span<uint8_t>       plaintext) const noexcept {
        const auto j0 = compute_j0(nonce);
        __m128i acc = ghash_aad(aad);
        detail::aes::gcm_crypt<kRounds>(ks_, acc, H_, j0, ciphertext, plaintext, false);
        const AesGcmTag expected = compute_tag(acc, aad.size(), ciphertext.size(), j0);

        // Constant-time tag comparison
        volatile uint8_t diff = 0;
        for (size_t i = 0; i < 16; ++i)
            diff |= expected[i] ^ tag[i];
        if (diff != 0)
            return std::unexpected(std::make_error_code(std::errc::bad_message));
        return {};
    }
#else
    void init(const KeyArray&) noexcept {}  // No-op without AES-NI
#endif
};

// ─── Named aliases ────────────────────────────────────────────────────────────

/** @brief AES-128-GCM AEAD context (128-bit key, hardware AES-NI required). */
using AesGcm128 = AesGcm<16>;

/** @brief AES-256-GCM AEAD context (256-bit key, hardware AES-NI required). */
using AesGcm256 = AesGcm<32>;

}  // namespace qbuem::crypto
