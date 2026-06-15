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
 * - **Key schedule**: AES-128 (10 rounds) / AES-256 (14 rounds). x86 uses
 *   `_mm_aeskeygenassist_si128`; ARM uses a software expansion (cold path).
 * - **CTR encryption**: hardware AES — `_mm_aesenc*` (x86) / `vaeseq_u8`+`vaesmcq_u8` (ARM).
 * - **GHASH**: portable, spec-correct GF(2^128) multiply (`detail::aes::gf_mul128`),
 *   shared by both backends and validated by a NIST known-answer test.
 *   (A SIMD PMULL/PCLMUL GHASH is a future optimization, guarded by that test.)
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

#include <qbuem/crypto/secure_zero.hpp>

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
        uint32_t eax = 1u, ebx = 0u, ecx = 0u, edx = 0u;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
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

template <size_t Rounds, typename KsType>
inline void aes_block(const KsType& ks, const uint8_t in[16], uint8_t out[16]) noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out),
                     aes_encrypt_block<Rounds>(
                         ks, _mm_loadu_si128(reinterpret_cast<const __m128i*>(in))));
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

#elif defined(QBUEM_AES_ARM)

// AES-128: 11 round keys (10 rounds); AES-256: 15 round keys (14 rounds).
using Ks128 = std::array<uint8x16_t, 11>;
using Ks256 = std::array<uint8x16_t, 15>;

// AES S-box (FIPS 197 Table 7). Used only by the cold-path key schedule; the
// data path uses constant-time AESE/AESMC instructions.
inline constexpr std::array<uint8_t, 256> kSbox = {{
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
}};

/**
 * @brief Software AES key expansion (FIPS 197). ARM has no key-gen-assist
 *        instruction, so the schedule is computed in software (once per key).
 * @param key  Raw key bytes (16 for AES-128, 32 for AES-256).
 * @param nk   Key length in 32-bit words (4 or 8).
 * @param nr   Number of rounds (10 or 14).
 * @param out  Output buffer of (nr+1)*16 bytes for the expanded round keys.
 */
inline void keyschedule_expand(const uint8_t* key, size_t nk, size_t nr,
                               uint8_t* out) noexcept {
    static constexpr uint8_t kRcon[] = {
        0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c};
    const size_t total_words = 4u * (nr + 1u);
    std::memcpy(out, key, nk * 4u);
    uint8_t t[4];
    for (size_t i = nk; i < total_words; ++i) {
        t[0] = out[(i - 1) * 4 + 0];
        t[1] = out[(i - 1) * 4 + 1];
        t[2] = out[(i - 1) * 4 + 2];
        t[3] = out[(i - 1) * 4 + 3];
        if (i % nk == 0) {
            const uint8_t tmp = t[0];  // RotWord
            t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = tmp;
            t[0] = kSbox[t[0]]; t[1] = kSbox[t[1]];  // SubWord
            t[2] = kSbox[t[2]]; t[3] = kSbox[t[3]];
            t[0] ^= kRcon[i / nk - 1];
        } else if (nk > 6 && i % nk == 4) {
            t[0] = kSbox[t[0]]; t[1] = kSbox[t[1]];
            t[2] = kSbox[t[2]]; t[3] = kSbox[t[3]];
        }
        out[i * 4 + 0] = out[(i - nk) * 4 + 0] ^ t[0];
        out[i * 4 + 1] = out[(i - nk) * 4 + 1] ^ t[1];
        out[i * 4 + 2] = out[(i - nk) * 4 + 2] ^ t[2];
        out[i * 4 + 3] = out[(i - nk) * 4 + 3] ^ t[3];
    }
}

inline void keyschedule_128(const uint8_t* key, Ks128& ks) noexcept {
    alignas(16) uint8_t buf[11 * 16];
    keyschedule_expand(key, 4, 10, buf);
    for (size_t r = 0; r < 11; ++r) ks[r] = vld1q_u8(buf + r * 16);
}

inline void keyschedule_256(const uint8_t* key, Ks256& ks) noexcept {
    alignas(16) uint8_t buf[15 * 16];
    keyschedule_expand(key, 8, 14, buf);
    for (size_t r = 0; r < 15; ++r) ks[r] = vld1q_u8(buf + r * 16);
}

// ─── AES block encryption (ARMv8 AESE/AESMC) ───────────────────────────────────

template <size_t Rounds, typename KsType>
[[nodiscard]] inline uint8x16_t aes_encrypt_block(const KsType& ks, uint8x16_t block) noexcept {
    // AESE performs AddRoundKey then SubBytes+ShiftRows; AESMC does MixColumns.
    for (size_t i = 0; i < Rounds - 1; ++i)
        block = vaesmcq_u8(vaeseq_u8(block, ks[i]));
    block = vaeseq_u8(block, ks[Rounds - 1]);
    return veorq_u8(block, ks[Rounds]);
}

template <size_t Rounds, typename KsType>
inline void aes_block(const KsType& ks, const uint8_t in[16], uint8_t out[16]) noexcept {
    vst1q_u8(out, aes_encrypt_block<Rounds>(ks, vld1q_u8(in)));
}

/** @brief Reverse all 16 bytes of a 128-bit value (big-endian ↔ little-endian). */
[[nodiscard]] inline uint8x16_t bswap128(uint8x16_t v) noexcept {
    const uint8x16_t r = vrev64q_u8(v);     // reverse bytes within each 64-bit lane
    return vextq_u8(r, r, 8);               // swap the two 64-bit lanes
}

/**
 * @brief GF(2^128) multiplication for GHASH using PMULL (vmull_p64).
 *
 * Faithfully mirrors the x86 PCLMULQDQ implementation (same Karatsuba split and
 * reduction by x^128 + x^7 + x^2 + x + 1); vmull_p64 and clmulepi64 compute the
 * identical 64×64 carryless product, so the byte-for-byte result matches.
 */
[[nodiscard]] inline uint8x16_t ghash_mul(uint8x16_t X, uint8x16_t H) noexcept {
    const uint64x2_t Xu = vreinterpretq_u64_u8(X);
    const uint64x2_t Hu = vreinterpretq_u64_u8(H);
    const auto x_lo = static_cast<poly64_t>(vgetq_lane_u64(Xu, 0));
    const auto x_hi = static_cast<poly64_t>(vgetq_lane_u64(Xu, 1));
    const auto h_lo = static_cast<poly64_t>(vgetq_lane_u64(Hu, 0));
    const auto h_hi = static_cast<poly64_t>(vgetq_lane_u64(Hu, 1));

    const uint8x16_t A  = vreinterpretq_u8_p128(vmull_p64(x_lo, h_lo));  // X.lo·H.lo
    const uint8x16_t B  = vreinterpretq_u8_p128(vmull_p64(x_hi, h_hi));  // X.hi·H.hi
    const uint8x16_t C  = vreinterpretq_u8_p128(vmull_p64(x_hi, h_lo));
    const uint8x16_t D  = vreinterpretq_u8_p128(vmull_p64(x_lo, h_hi));
    const uint8x16_t CD = veorq_u8(C, D);
    const uint8x16_t zero = vdupq_n_u8(0);

    const uint8x16_t lo = veorq_u8(A, vextq_u8(zero, CD, 8));  // A ^ (CD << 64)
    const uint8x16_t hi = veorq_u8(B, vextq_u8(CD, zero, 8));  // B ^ (CD >> 64)

    // Reduction mod the GCM polynomial (0x87 = x^7 + x^2 + x + 1).
    const auto p_lo = static_cast<poly64_t>(0x0000000100000087ULL);
    const auto hi_hi = static_cast<poly64_t>(vgetq_lane_u64(vreinterpretq_u64_u8(hi), 1));
    const uint8x16_t t1 = vreinterpretq_u8_p128(vmull_p64(hi_hi, p_lo));
    const uint8x16_t t2 = veorq_u8(lo, vextq_u8(zero, t1, 8));
    const uint8x16_t t3 = veorq_u8(hi, vextq_u8(t1, zero, 8));
    const auto t3_lo = static_cast<poly64_t>(vgetq_lane_u64(vreinterpretq_u64_u8(t3), 0));
    const uint8x16_t t4 = vreinterpretq_u8_p128(vmull_p64(t3_lo, p_lo));
    return veorq_u8(veorq_u8(t2, t4), t3);
}

// ─── CTR + GHASH core (ARM) ─────────────────────────────────────────────────────

template <size_t Rounds, typename KsType>
inline void gcm_crypt(const KsType&            ks,
                      uint8x16_t&              ghash_acc,
                      const uint8x16_t&        H,
                      const std::array<uint8_t, 16>& j0,
                      std::span<const uint8_t> src,
                      std::span<uint8_t>       dst,
                      bool                     is_encrypt) noexcept {
    uint8_t ctr_bytes[16];
    std::memcpy(ctr_bytes, j0.data(), 16);

    auto inc_ctr = [](uint8_t* c) noexcept {
        for (int i = 15; i >= 12; --i) { if (++c[i] != 0) break; }
    };
    inc_ctr(ctr_bytes);  // Start at J0 + 1

    const size_t blocks    = src.size() / 16;
    const size_t remainder = src.size() % 16;

    for (size_t b = 0; b < blocks; ++b) {
        const uint8x16_t ctr      = vld1q_u8(ctr_bytes);
        const uint8x16_t ks_block = aes_encrypt_block<Rounds>(ks, ctr);
        const uint8x16_t plain    = vld1q_u8(src.data() + b * 16);
        const uint8x16_t cipher   = veorq_u8(plain, ks_block);
        vst1q_u8(dst.data() + b * 16, cipher);

        const uint8x16_t ct_be = bswap128(is_encrypt ? cipher : plain);
        ghash_acc = ghash_mul(veorq_u8(ghash_acc, ct_be), H);
        inc_ctr(ctr_bytes);
    }

    if (remainder > 0) {
        const uint8x16_t ctr      = vld1q_u8(ctr_bytes);
        const uint8x16_t ks_block = aes_encrypt_block<Rounds>(ks, ctr);

        alignas(16) uint8_t buf[16] = {};
        std::memcpy(buf, src.data() + blocks * 16, remainder);
        const uint8x16_t plain  = vld1q_u8(buf);
        const uint8x16_t cipher = veorq_u8(plain, ks_block);

        alignas(16) uint8_t out_buf[16];
        vst1q_u8(out_buf, cipher);
        std::memcpy(dst.data() + blocks * 16, out_buf, remainder);

        alignas(16) uint8_t ct_buf[16] = {};
        std::memcpy(ct_buf, is_encrypt ? out_buf : buf, remainder);
        const uint8x16_t ct_be = bswap128(vld1q_u8(ct_buf));
        ghash_acc = ghash_mul(veorq_u8(ghash_acc, ct_be), H);
    }
}

#endif  // QBUEM_AES_NI / QBUEM_AES_ARM

#if defined(QBUEM_AES_NI) || defined(QBUEM_AES_ARM)
// ─── GHASH (portable, spec-correct; shared by all backends) ────────────────────

/**
 * @brief GF(2^128) multiply for GHASH (NIST SP 800-38D). Computes Z = Z·H in
 *        place over the standard big-endian / bit-reflected GCM field.
 *
 * Portable and constant-time w.r.t. the data bits (mask, not branch). This
 * replaces the previous arch-specific PCLMUL/PMULL path, which produced
 * self-consistent but NON-spec tags (round-trip passed, interop failed). A SIMD
 * GHASH may reintroduce peak speed later — the known-answer test now guards
 * correctness on both x86 and ARM.
 */
inline void gf_mul128(uint8_t Z[16], const uint8_t H[16]) noexcept {
    uint8_t v[16];
    std::memcpy(v, H, 16);
    uint8_t z[16] = {};
    for (int i = 0; i < 128; ++i) {
        const uint8_t bit  = (Z[i >> 3] >> (7 - (i & 7))) & 1u;
        const uint8_t mask = static_cast<uint8_t>(0u - bit);  // 0xFF if set else 0
        for (int j = 0; j < 16; ++j) z[j] ^= static_cast<uint8_t>(v[j] & mask);
        const uint8_t lsb = v[15] & 1u;
        for (int j = 15; j > 0; --j)
            v[j] = static_cast<uint8_t>((v[j] >> 1) | (v[j - 1] << 7));
        v[0] = static_cast<uint8_t>(v[0] >> 1);
        v[0] ^= static_cast<uint8_t>(0xe1u & (0u - lsb));  // reduce by R = 0xe1<<120
    }
    std::memcpy(Z, z, 16);
}

/** @brief Increment the big-endian 32-bit counter in the last 4 bytes of @p ctr. */
inline void inc_counter(uint8_t ctr[16]) noexcept {
    for (int i = 15; i >= 12; --i) { if (++ctr[i] != 0) break; }
}
#endif  // QBUEM_AES_NI || QBUEM_AES_ARM

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
#if defined(QBUEM_AES_NI) || defined(QBUEM_AES_ARM)
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
#if defined(QBUEM_AES_NI) || defined(QBUEM_AES_ARM)
        return open_impl(nonce, aad, ciphertext, tag, plaintext);
#else
        (void)nonce; (void)aad; (void)ciphertext; (void)tag; (void)plaintext;
        return std::unexpected(std::make_error_code(std::errc::function_not_supported));
#endif
    }

#if defined(QBUEM_AES_NI) || defined(QBUEM_AES_ARM)
    /**
     * @brief Securely zero the expanded key schedule and hash subkey.
     *
     * Call when the context is no longer needed to minimize how long key
     * material lingers in memory (optimizer-proof — see crypto/secure_zero.hpp).
     */
    void wipe() noexcept {
        secure_zero(&ks_, sizeof(ks_));
        secure_zero(H_.data(), H_.size());
    }
#endif

private:
    AesGcm() = default;

#if defined(QBUEM_AES_NI) || defined(QBUEM_AES_ARM)
    using KsType = std::conditional_t<KeyBytes == 16,
                                       detail::aes::Ks128,
                                       detail::aes::Ks256>;
    KsType   ks_{};
    std::array<uint8_t, 16> H_{};  // Hash subkey: E_K(0)

    void init(const KeyArray& key) noexcept {
        if constexpr (KeyBytes == 16)
            detail::aes::keyschedule_128(key.data(), ks_);
        else
            detail::aes::keyschedule_256(key.data(), ks_);

        // Hash subkey H = E_K(0^128)
        const uint8_t zero[16] = {};
        detail::aes::aes_block<kRounds>(ks_, zero, H_.data());
    }

    std::array<uint8_t, 16> compute_j0(const AesGcmNonce& nonce) const noexcept {
        // J0 = nonce || 0x00000001 (96-bit nonce, 32-bit counter = 1)
        std::array<uint8_t, 16> j0{};
        std::memcpy(j0.data(), nonce.data(), 12);
        j0[15] = 0x01u;
        return j0;
    }

    // GHASH a zero-padded byte sequence into the accumulator.
    void ghash_update(uint8_t acc[16], std::span<const uint8_t> data) const noexcept {
        const size_t blocks    = data.size() / 16;
        const size_t remainder = data.size() % 16;
        for (size_t b = 0; b < blocks; ++b) {
            for (size_t j = 0; j < 16; ++j) acc[j] ^= data[b * 16 + j];
            detail::aes::gf_mul128(acc, H_.data());
        }
        if (remainder > 0) {
            uint8_t blk[16] = {};
            std::memcpy(blk, data.data() + blocks * 16, remainder);
            for (size_t j = 0; j < 16; ++j) acc[j] ^= blk[j];
            detail::aes::gf_mul128(acc, H_.data());
        }
    }

    // AES-CTR (GCTR) keystream starting at J0 + 1.
    void gctr(const std::array<uint8_t, 16>& j0,
              std::span<const uint8_t>       src,
              std::span<uint8_t>             dst) const noexcept {
        uint8_t ctr[16];
        std::memcpy(ctr, j0.data(), 16);
        detail::aes::inc_counter(ctr);  // start at J0 + 1

        uint8_t ks_block[16];
        const size_t blocks    = src.size() / 16;
        const size_t remainder = src.size() % 16;
        for (size_t b = 0; b < blocks; ++b) {
            detail::aes::aes_block<kRounds>(ks_, ctr, ks_block);
            for (size_t j = 0; j < 16; ++j)
                dst[b * 16 + j] = static_cast<uint8_t>(src[b * 16 + j] ^ ks_block[j]);
            detail::aes::inc_counter(ctr);
        }
        if (remainder > 0) {
            detail::aes::aes_block<kRounds>(ks_, ctr, ks_block);
            for (size_t j = 0; j < remainder; ++j)
                dst[blocks * 16 + j] =
                    static_cast<uint8_t>(src[blocks * 16 + j] ^ ks_block[j]);
        }
    }

    AesGcmTag compute_tag(uint8_t          acc[16],
                          size_t           aad_len,
                          size_t           ct_len,
                          const std::array<uint8_t, 16>& j0) const noexcept {
        // Length block: [len(A) in bits || len(C) in bits], big-endian 64+64.
        const uint64_t aad_bits = static_cast<uint64_t>(aad_len) * 8u;
        const uint64_t ct_bits  = static_cast<uint64_t>(ct_len)  * 8u;
        uint8_t len_block[16];
        for (int i = 7; i >= 0; --i)
            len_block[    7 - i] = static_cast<uint8_t>(aad_bits >> (i * 8));
        for (int i = 7; i >= 0; --i)
            len_block[8 + 7 - i] = static_cast<uint8_t>(ct_bits  >> (i * 8));
        for (size_t j = 0; j < 16; ++j) acc[j] ^= len_block[j];
        detail::aes::gf_mul128(acc, H_.data());

        // Tag = E_K(J0) XOR GHASH
        uint8_t ej0[16];
        detail::aes::aes_block<kRounds>(ks_, j0.data(), ej0);
        AesGcmTag tag{};
        for (size_t j = 0; j < 16; ++j)
            tag[j] = static_cast<uint8_t>(ej0[j] ^ acc[j]);
        return tag;
    }

    void seal_impl(const AesGcmNonce&       nonce,
                   std::span<const uint8_t> aad,
                   std::span<const uint8_t> plaintext,
                   std::span<uint8_t>       ciphertext,
                   AesGcmTag&               tag) const noexcept {
        const auto j0 = compute_j0(nonce);
        gctr(j0, plaintext, ciphertext);
        uint8_t acc[16] = {};
        ghash_update(acc, aad);
        ghash_update(acc, {ciphertext.data(), ciphertext.size()});
        tag = compute_tag(acc, aad.size(), ciphertext.size(), j0);
    }

    [[nodiscard]] Result<void>
    open_impl(const AesGcmNonce&       nonce,
              std::span<const uint8_t> aad,
              std::span<const uint8_t> ciphertext,
              const AesGcmTag&         tag,
              std::span<uint8_t>       plaintext) const noexcept {
        const auto j0 = compute_j0(nonce);
        uint8_t acc[16] = {};
        ghash_update(acc, aad);
        ghash_update(acc, {ciphertext.data(), ciphertext.size()});
        const AesGcmTag expected = compute_tag(acc, aad.size(), ciphertext.size(), j0);

        // Constant-time tag comparison BEFORE releasing any plaintext.
        volatile uint8_t diff = 0;
        for (size_t i = 0; i < 16; ++i)
            diff |= static_cast<uint8_t>(expected[i] ^ tag[i]);
        if (diff != 0)
            return std::unexpected(std::make_error_code(std::errc::bad_message));

        gctr(j0, ciphertext, plaintext);  // decrypt only after authentication
        return {};
    }
#else
    void init(const KeyArray&) noexcept {}  // No-op when no hardware AES
#endif
};

// ─── Named aliases ────────────────────────────────────────────────────────────

/** @brief AES-128-GCM AEAD context (128-bit key, hardware AES-NI required). */
using AesGcm128 = AesGcm<16>;

/** @brief AES-256-GCM AEAD context (256-bit key, hardware AES-NI required). */
using AesGcm256 = AesGcm<32>;

}  // namespace qbuem::crypto
