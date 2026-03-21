#pragma once

/**
 * @file qbuem/crypto/sha256.hpp
 * @brief SHA-256 and SHA-224 hash functions (FIPS 180-4 / RFC 6234).
 * @ingroup qbuem_crypto
 *
 * Provides a streaming context (`Sha256Context`) and one-shot helpers
 * (`sha256()`, `sha224()`).  Three hardware-accelerated paths are selected
 * at compile time:
 *
 * | Path          | Macro              | Throughput      |
 * |---------------|--------------------|-----------------|
 * | Intel SHA-NI  | `__SHA__`          | ~4 GB/s         |
 * | ARM SHA2      | `__ARM_FEATURE_SHA2`| ~3 GB/s         |
 * | Scalar (C++23)| fallback           | ~200–400 MB/s   |
 *
 * ### Streaming usage
 * ```cpp
 * qbuem::crypto::Sha256Context ctx;
 * ctx.update(header);
 * ctx.update(body);
 * auto digest = ctx.finalize();  // std::array<uint8_t, 32>
 * ```
 *
 * ### One-shot usage
 * ```cpp
 * auto digest = qbuem::crypto::sha256(data);
 * auto digest224 = qbuem::crypto::sha224(data);
 * ```
 *
 * ### Zero-allocation guarantee
 * All internal state fits in the `Sha256Context` struct (no heap).
 * `finalize()` returns `std::array<uint8_t, 32>` — no allocation.
 */

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

// ─── SIMD detection ───────────────────────────────────────────────────────────
#if defined(__SHA__) && (defined(__x86_64__) || defined(__i386__))
#  if __has_include(<immintrin.h>)
#    include <immintrin.h>
#    define QBUEM_SHA256_SHA_NI 1
#  endif
#endif

#if defined(__ARM_FEATURE_SHA2) && defined(__aarch64__)
#  include <arm_neon.h>
#  define QBUEM_SHA256_ARM_SHA2 1
#endif

namespace qbuem::crypto {

// ─── Digest types ─────────────────────────────────────────────────────────────

/** @brief 256-bit SHA-256 digest. */
using Sha256Digest = std::array<uint8_t, 32>;

/** @brief 224-bit SHA-224 digest (SHA-256 variant, truncated). */
using Sha224Digest = std::array<uint8_t, 28>;

// ─── SHA-256 constants ────────────────────────────────────────────────────────

namespace detail::sha256 {

// First 32 bits of fractional parts of cube roots of the first 64 primes.
inline constexpr std::array<uint32_t, 64> K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

// SHA-256 initial hash values (first 32 bits of fractional parts of sqrt of first 8 primes).
inline constexpr std::array<uint32_t, 8> H0_256 = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

// SHA-224 initial hash values (differ from SHA-256).
inline constexpr std::array<uint32_t, 8> H0_224 = {
    0xc1059ed8u, 0x367cd507u, 0x3070dd17u, 0xf70e5939u,
    0xffc00b31u, 0x68581511u, 0x64f98fa7u, 0xbefa4fa4u,
};

// ─── Scalar round operations ──────────────────────────────────────────────────

[[nodiscard]] inline constexpr uint32_t Ch(uint32_t e, uint32_t f, uint32_t g) noexcept {
    return (e & f) ^ (~e & g);
}
[[nodiscard]] inline constexpr uint32_t Maj(uint32_t a, uint32_t b, uint32_t c) noexcept {
    return (a & b) ^ (a & c) ^ (b & c);
}
[[nodiscard]] inline constexpr uint32_t Sigma0(uint32_t x) noexcept {
    return std::rotr(x, 2u) ^ std::rotr(x, 13u) ^ std::rotr(x, 22u);
}
[[nodiscard]] inline constexpr uint32_t Sigma1(uint32_t x) noexcept {
    return std::rotr(x, 6u) ^ std::rotr(x, 11u) ^ std::rotr(x, 25u);
}
[[nodiscard]] inline constexpr uint32_t sigma0(uint32_t x) noexcept {
    return std::rotr(x, 7u) ^ std::rotr(x, 18u) ^ (x >> 3u);
}
[[nodiscard]] inline constexpr uint32_t sigma1(uint32_t x) noexcept {
    return std::rotr(x, 17u) ^ std::rotr(x, 19u) ^ (x >> 10u);
}

// ─── Big-endian helpers ───────────────────────────────────────────────────────

[[nodiscard]] inline uint32_t load_be32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    return v;
}

inline void store_be32(uint8_t* p, uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 4);
}

// ─── Scalar block compression ─────────────────────────────────────────────────

inline void compress_scalar(std::array<uint32_t, 8>& state,
                             const uint8_t* block) noexcept {
    std::array<uint32_t, 64> W{};

    // Message schedule: first 16 words from block (big-endian)
    for (size_t i = 0; i < 16; ++i)
        W[i] = load_be32(block + i * 4);

    // Extend to 64 words
    for (size_t i = 16; i < 64; ++i)
        W[i] = sigma1(W[i - 2]) + W[i - 7] + sigma0(W[i - 15]) + W[i - 16];

    // Working variables
    auto [a, b, c, d, e, f, g, h] = state;

    // 64 rounds
    for (size_t i = 0; i < 64; ++i) {
        const uint32_t T1 = h + Sigma1(e) + Ch(e, f, g) + K[i] + W[i];
        const uint32_t T2 = Sigma0(a) + Maj(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

// ─── ARM SHA2 block compression ───────────────────────────────────────────────

#if defined(QBUEM_SHA256_ARM_SHA2)
inline void compress_arm_sha2(std::array<uint32_t, 8>& state,
                               const uint8_t* block) noexcept {
    // Load state into two 128-bit registers: {abcd} and {efgh}
    uint32x4_t abcd = vld1q_u32(state.data());
    uint32x4_t efgh = vld1q_u32(state.data() + 4);

    // Save original state for final addition
    const uint32x4_t abcd0 = abcd;
    const uint32x4_t efgh0 = efgh;

    // Load message block (16 × 32-bit words), big-endian to little-endian swap
    static const uint8x16_t shuf = {3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12};
    uint32x4_t w0 = vreinterpretq_u32_u8(vqtbl1q_u8(vld1q_u8(block +  0), shuf));
    uint32x4_t w1 = vreinterpretq_u32_u8(vqtbl1q_u8(vld1q_u8(block + 16), shuf));
    uint32x4_t w2 = vreinterpretq_u32_u8(vqtbl1q_u8(vld1q_u8(block + 32), shuf));
    uint32x4_t w3 = vreinterpretq_u32_u8(vqtbl1q_u8(vld1q_u8(block + 48), shuf));

    // Precompute message + round constant
    const uint32x4_t k0  = vld1q_u32(&K[ 0]);
    const uint32x4_t k1  = vld1q_u32(&K[ 4]);
    const uint32x4_t k2  = vld1q_u32(&K[ 8]);
    const uint32x4_t k3  = vld1q_u32(&K[12]);
    const uint32x4_t k4  = vld1q_u32(&K[16]);
    const uint32x4_t k5  = vld1q_u32(&K[20]);
    const uint32x4_t k6  = vld1q_u32(&K[24]);
    const uint32x4_t k7  = vld1q_u32(&K[28]);
    const uint32x4_t k8  = vld1q_u32(&K[32]);
    const uint32x4_t k9  = vld1q_u32(&K[36]);
    const uint32x4_t k10 = vld1q_u32(&K[40]);
    const uint32x4_t k11 = vld1q_u32(&K[44]);
    const uint32x4_t k12 = vld1q_u32(&K[48]);
    const uint32x4_t k13 = vld1q_u32(&K[52]);
    const uint32x4_t k14 = vld1q_u32(&K[56]);
    const uint32x4_t k15 = vld1q_u32(&K[60]);

    // Macro for 4 rounds: schedule expansion + hash rounds
#define SHA256_ARM_ROUNDS4(w_new, wa, wb, wc, wd, k) \
    do { \
        w_new = vsha256su1q_u32(vsha256su0q_u32(wa, wb), wc, wd); \
        uint32x4_t wk = vaddq_u32(w_new, k); \
        uint32x4_t tmp = vsha256hq_u32(abcd, efgh, wk); \
        efgh = vsha256h2q_u32(efgh, abcd, wk); \
        abcd = tmp; \
    } while (0)

    // Rounds 0–15: no message schedule expansion needed
    {
        uint32x4_t wk = vaddq_u32(w0, k0);
        uint32x4_t tmp = vsha256hq_u32(abcd, efgh, wk);
        efgh = vsha256h2q_u32(efgh, abcd, wk);
        abcd = tmp;
    }
    {
        uint32x4_t wk = vaddq_u32(w1, k1);
        uint32x4_t tmp = vsha256hq_u32(abcd, efgh, wk);
        efgh = vsha256h2q_u32(efgh, abcd, wk);
        abcd = tmp;
    }
    {
        uint32x4_t wk = vaddq_u32(w2, k2);
        uint32x4_t tmp = vsha256hq_u32(abcd, efgh, wk);
        efgh = vsha256h2q_u32(efgh, abcd, wk);
        abcd = tmp;
    }
    {
        uint32x4_t wk = vaddq_u32(w3, k3);
        uint32x4_t tmp = vsha256hq_u32(abcd, efgh, wk);
        efgh = vsha256h2q_u32(efgh, abcd, wk);
        abcd = tmp;
    }

    // Rounds 16–63: schedule expansion with vsha256su0q/su1q
    SHA256_ARM_ROUNDS4(w0, w0, w1, w2, w3, k4);
    SHA256_ARM_ROUNDS4(w1, w1, w2, w3, w0, k5);
    SHA256_ARM_ROUNDS4(w2, w2, w3, w0, w1, k6);
    SHA256_ARM_ROUNDS4(w3, w3, w0, w1, w2, k7);
    SHA256_ARM_ROUNDS4(w0, w0, w1, w2, w3, k8);
    SHA256_ARM_ROUNDS4(w1, w1, w2, w3, w0, k9);
    SHA256_ARM_ROUNDS4(w2, w2, w3, w0, w1, k10);
    SHA256_ARM_ROUNDS4(w3, w3, w0, w1, w2, k11);
    SHA256_ARM_ROUNDS4(w0, w0, w1, w2, w3, k12);
    SHA256_ARM_ROUNDS4(w1, w1, w2, w3, w0, k13);
    SHA256_ARM_ROUNDS4(w2, w2, w3, w0, w1, k14);
    SHA256_ARM_ROUNDS4(w3, w3, w0, w1, w2, k15);

#undef SHA256_ARM_ROUNDS4

    // Add back original state
    abcd = vaddq_u32(abcd, abcd0);
    efgh = vaddq_u32(efgh, efgh0);

    vst1q_u32(state.data(),     abcd);
    vst1q_u32(state.data() + 4, efgh);
}
#endif  // QBUEM_SHA256_ARM_SHA2

// ─── Intel SHA-NI block compression ──────────────────────────────────────────

#if defined(QBUEM_SHA256_SHA_NI)
inline void compress_sha_ni(std::array<uint32_t, 8>& state,
                             const uint8_t* block) noexcept {
    // SHA-NI state layout:
    //   state0 = [e, f, b, a]  (words packed into XMM, little-endian uint32)
    //   state1 = [g, h, d, c]
    static const __m128i SHUF_MASK =
        _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);

    // Load and arrange state: SHA-NI expects {a,b,e,f} and {c,d,g,h}
    __m128i abef = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data()));
    __m128i cdgh = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data() + 4));

    // Shuffle state: abcd → [d,c,b,a] and efgh → [h,g,f,e]
    __m128i tmp = _mm_shuffle_epi32(abef, 0xB1);  // [b,a,d,c] → ...
    cdgh        = _mm_shuffle_epi32(cdgh, 0x1B);  // [h,g,f,e]
    abef        = _mm_alignr_epi8(tmp, cdgh, 8);  // [b,a,h,g]
    cdgh        = _mm_blend_epi16(cdgh, tmp, 0xF0); // [d,c,f,e]

    const __m128i abef0 = abef;
    const __m128i cdgh0 = cdgh;

    // Load message, convert to big-endian
    __m128i msg0 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block +  0)), SHUF_MASK);
    __m128i msg1 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16)), SHUF_MASK);
    __m128i msg2 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 32)), SHUF_MASK);
    __m128i msg3 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 48)), SHUF_MASK);

    // Round macro: add K, perform 2 rounds, advance message schedule
#define SHA256_NI_ROUND(msg_cur, msg_a, msg_b, msg_c, ki) \
    do { \
        __m128i wk = _mm_add_epi32(msg_cur, _mm_load_si128(reinterpret_cast<const __m128i*>(&K[ki]))); \
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk); \
        abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wk, 0xEE)); \
        msg_cur = _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(msg_cur, msg_a), \
                      _mm_alignr_epi8(msg_b, msg_a, 4)), msg_c); \
    } while (0)

    // Rounds 0–15 (no schedule for rounds 0–3)
    {
        __m128i wk = _mm_add_epi32(msg0, _mm_load_si128(reinterpret_cast<const __m128i*>(&K[0])));
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk);
        abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wk, 0xEE));
        msg0 = _mm_sha256msg1_epu32(msg0, msg1);
    }
    {
        __m128i wk = _mm_add_epi32(msg1, _mm_load_si128(reinterpret_cast<const __m128i*>(&K[4])));
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk);
        abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wk, 0xEE));
        msg1 = _mm_sha256msg1_epu32(msg1, msg2);
    }
    {
        __m128i wk = _mm_add_epi32(msg2, _mm_load_si128(reinterpret_cast<const __m128i*>(&K[8])));
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk);
        abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wk, 0xEE));
        msg2 = _mm_sha256msg1_epu32(msg2, msg3);
    }
    {
        __m128i wk = _mm_add_epi32(msg3, _mm_load_si128(reinterpret_cast<const __m128i*>(&K[12])));
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk);
        abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wk, 0xEE));
        __m128i tmp2 = _mm_alignr_epi8(msg3, msg2, 4);
        msg0 = _mm_add_epi32(msg0, tmp2);
        msg0 = _mm_sha256msg2_epu32(msg0, msg3);
    }

    // Rounds 16–63
    SHA256_NI_ROUND(msg1, msg2, msg3, msg0, 16);
    SHA256_NI_ROUND(msg2, msg3, msg0, msg1, 20);
    SHA256_NI_ROUND(msg3, msg0, msg1, msg2, 24);
    SHA256_NI_ROUND(msg0, msg1, msg2, msg3, 28);
    SHA256_NI_ROUND(msg1, msg2, msg3, msg0, 32);
    SHA256_NI_ROUND(msg2, msg3, msg0, msg1, 36);
    SHA256_NI_ROUND(msg3, msg0, msg1, msg2, 40);
    SHA256_NI_ROUND(msg0, msg1, msg2, msg3, 44);
    SHA256_NI_ROUND(msg1, msg2, msg3, msg0, 48);
    SHA256_NI_ROUND(msg2, msg3, msg0, msg1, 52);
    SHA256_NI_ROUND(msg3, msg0, msg1, msg2, 56);
    SHA256_NI_ROUND(msg0, msg1, msg2, msg3, 60);

#undef SHA256_NI_ROUND

    // Add saved state
    abef = _mm_add_epi32(abef, abef0);
    cdgh = _mm_add_epi32(cdgh, cdgh0);

    // Unscramble back to natural order [a,b,c,d,e,f,g,h]
    tmp  = _mm_shuffle_epi32(abef, 0x1B);   // [a,b,e,f] → [f,e,b,a]
    abef = _mm_shuffle_epi32(cdgh, 0xB1);   // [c,d,g,h] → [h,g,d,c]
    cdgh = _mm_blend_epi16(tmp, abef, 0xF0);// [h,g,b,a]
    abef = _mm_alignr_epi8(abef, tmp, 8);   // [d,c,f,e]

    _mm_storeu_si128(reinterpret_cast<__m128i*>(state.data()),     cdgh);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state.data() + 4), abef);
}
#endif  // QBUEM_SHA256_SHA_NI

// ─── Unified block dispatch ───────────────────────────────────────────────────

inline void compress_block(std::array<uint32_t, 8>& state,
                            const uint8_t* block) noexcept {
#if defined(QBUEM_SHA256_SHA_NI)
    compress_sha_ni(state, block);
#elif defined(QBUEM_SHA256_ARM_SHA2)
    compress_arm_sha2(state, block);
#else
    compress_scalar(state, block);
#endif
}

}  // namespace detail::sha256

// ─── Sha256Context ────────────────────────────────────────────────────────────

/**
 * @brief Streaming SHA-256 hash context.
 *
 * Maintains internal state across multiple `update()` calls.
 * Call `finalize()` to retrieve the digest without consuming the context
 * (the original context is preserved; you can continue hashing after finalizing
 * a copy by using `Sha256Context copy = ctx; copy.finalize()`).
 *
 * The context is 128 bytes — fits entirely in two L1 cache lines.
 */
class alignas(64) Sha256Context {
public:
    /** @brief Construct and initialise for SHA-256. */
    constexpr Sha256Context() noexcept { reset_256(); }

    /** @brief Reset to the SHA-256 initial state. */
    void reset() noexcept { reset_256(); }

    /**
     * @brief Feed @p data into the hash.
     * May be called any number of times before `finalize()`.
     */
    void update(std::span<const uint8_t> data) noexcept {
        const uint8_t* src = data.data();
        size_t         len = data.size();

        // Fill partial block if one is buffered
        if (buf_len_ > 0) {
            const size_t space = 64u - buf_len_;
            const size_t take  = len < space ? len : space;
            std::memcpy(buf_.data() + buf_len_, src, take);
            buf_len_ += take;
            src += take;
            len -= take;
            if (buf_len_ == 64) {
                detail::sha256::compress_block(state_, buf_.data());
                total_bits_ += 512;
                buf_len_ = 0;
            }
        }

        // Process full blocks directly from input (zero-copy)
        while (len >= 64) {
            detail::sha256::compress_block(state_, src);
            total_bits_ += 512;
            src += 64;
            len -= 64;
        }

        // Buffer remainder
        if (len > 0) {
            std::memcpy(buf_.data(), src, len);
            buf_len_ = len;
        }
    }

    /** @brief Convenience overload for string data. */
    void update(std::string_view sv) noexcept {
        update({reinterpret_cast<const uint8_t*>(sv.data()), sv.size()});
    }

    /**
     * @brief Compute and return the final SHA-256 digest.
     *
     * This method **copies** the context state before padding so the
     * original context may still be used (call `reset()` to reuse).
     *
     * @returns 32-byte SHA-256 digest.
     */
    [[nodiscard]] Sha256Digest finalize() const noexcept {
        // Work on a copy so this context remains usable
        Sha256Context tmp = *this;
        tmp.pad_and_compress();
        Sha256Digest out{};
        for (size_t i = 0; i < 8; ++i)
            detail::sha256::store_be32(out.data() + i * 4, tmp.state_[i]);
        return out;
    }

    /**
     * @brief Compute the SHA-224 digest (truncated SHA-256).
     *
     * Only valid when the context was created with `Sha256Context(Variant::SHA224)`.
     * @returns 28-byte SHA-224 digest.
     */
    [[nodiscard]] Sha224Digest finalize_224() const noexcept {
        Sha256Context tmp = *this;
        tmp.pad_and_compress();
        Sha224Digest out{};
        for (size_t i = 0; i < 7; ++i)
            detail::sha256::store_be32(out.data() + i * 4, tmp.state_[i]);
        return out;
    }

    /** @brief Select the hash variant for construction. */
    enum class Variant { SHA256, SHA224 };

    /** @brief Construct with explicit variant selection. */
    explicit constexpr Sha256Context(Variant v) noexcept {
        if (v == Variant::SHA224) reset_224();
        else reset_256();
    }

private:
    std::array<uint32_t, 8>  state_{};
    std::array<uint8_t,  64> buf_{};
    uint64_t                 total_bits_ = 0;
    size_t                   buf_len_    = 0;

    constexpr void reset_256() noexcept {
        state_      = detail::sha256::H0_256;
        total_bits_ = 0;
        buf_len_    = 0;
        buf_.fill(0);
    }
    constexpr void reset_224() noexcept {
        state_      = detail::sha256::H0_224;
        total_bits_ = 0;
        buf_len_    = 0;
        buf_.fill(0);
    }

    void pad_and_compress() noexcept {
        // Account for buffered bytes not yet included in total_bits_
        const uint64_t msg_bits = total_bits_ + buf_len_ * 8u;

        // Append bit '1' (0x80 byte)
        buf_[buf_len_++] = 0x80u;

        // If not enough room for the 8-byte length field, flush
        if (buf_len_ > 56) {
            std::memset(buf_.data() + buf_len_, 0, 64u - buf_len_);
            detail::sha256::compress_block(state_, buf_.data());
            buf_len_ = 0;
        }

        // Zero-pad up to byte 56
        std::memset(buf_.data() + buf_len_, 0, 56u - buf_len_);

        // Append message length in bits as big-endian 64-bit integer
        for (int i = 7; i >= 0; --i)
            buf_[56 + static_cast<size_t>(7 - i)] = static_cast<uint8_t>(msg_bits >> (i * 8));

        detail::sha256::compress_block(state_, buf_.data());
    }
};

// ─── One-shot helpers ─────────────────────────────────────────────────────────

/**
 * @brief Compute SHA-256 of the given data in one call.
 * @param data  Input bytes.
 * @returns 32-byte digest.
 */
[[nodiscard]] inline Sha256Digest sha256(std::span<const uint8_t> data) noexcept {
    Sha256Context ctx;
    ctx.update(data);
    return ctx.finalize();
}

/** @brief Compute SHA-256 of a string. */
[[nodiscard]] inline Sha256Digest sha256(std::string_view sv) noexcept {
    return sha256({reinterpret_cast<const uint8_t*>(sv.data()), sv.size()});
}

/**
 * @brief Compute SHA-224 of the given data in one call.
 * @param data  Input bytes.
 * @returns 28-byte digest.
 */
[[nodiscard]] inline Sha224Digest sha224(std::span<const uint8_t> data) noexcept {
    Sha256Context ctx{Sha256Context::Variant::SHA224};
    ctx.update(data);
    return ctx.finalize_224();
}

/** @brief Compute SHA-224 of a string. */
[[nodiscard]] inline Sha224Digest sha224(std::string_view sv) noexcept {
    return sha224({reinterpret_cast<const uint8_t*>(sv.data()), sv.size()});
}

}  // namespace qbuem::crypto
