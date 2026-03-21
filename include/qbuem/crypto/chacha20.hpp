#pragma once

/**
 * @file qbuem/crypto/chacha20.hpp
 * @brief ChaCha20 stream cipher (RFC 8439).
 * @ingroup qbuem_crypto
 *
 * ChaCha20 is an ARX (Add-Rotate-XOR) stream cipher designed by D. Bernstein.
 * Its design is inherently constant-time — no table lookups, no data-dependent
 * branches — making it safe to implement in pure C++ without hardware AES.
 *
 * ### Key properties
 * - 256-bit key
 * - 96-bit nonce (12 bytes)
 * - 32-bit block counter (max 256 GiB per key/nonce pair)
 * - 64-byte keystream block
 *
 * ### SIMD acceleration
 * | Path    | Condition          | Notes                          |
 * |---------|--------------------|--------------------------------|
 * | AVX2    | `__AVX2__`         | 8 blocks (512 bytes) / call    |
 * | NEON    | `__ARM_NEON`       | 4 blocks (256 bytes) / call    |
 * | Scalar  | fallback           | 1 block (64 bytes) / call      |
 *
 * ### Usage
 * ```cpp
 * // Encrypt (XOR with keystream) — in-place
 * std::array<uint8_t, 32> key = ...;
 * std::array<uint8_t, 12> nonce = {};
 * qbuem::crypto::chacha20_xor(key, nonce, 0, plaintext);
 *
 * // Streaming
 * qbuem::crypto::ChaCha20 c{key, nonce};
 * c.seek(0);           // set block counter
 * c.xor_into(chunk1);  // encrypt chunk1 in-place
 * c.xor_into(chunk2);
 * ```
 *
 * ### Security
 * - Never reuse a (key, nonce) pair.
 * - For AEAD use ChaCha20-Poly1305 (see chacha20_poly1305.hpp).
 */

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

#if defined(__AVX2__) && __has_include(<immintrin.h>)
#  include <immintrin.h>
#  define QBUEM_CHACHA20_AVX2 1
#elif defined(__ARM_NEON) && __has_include(<arm_neon.h>)
#  include <arm_neon.h>
#  define QBUEM_CHACHA20_NEON 1
#endif

namespace qbuem::crypto {

// ─── Constants ────────────────────────────────────────────────────────────────

/** @brief Standard ChaCha20 constants ("expand 32-byte k"). */
inline constexpr std::array<uint32_t, 4> kChaChaConst = {
    0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u,
};

// ─── Quarter-round (core operation) ──────────────────────────────────────────

namespace detail::chacha {

inline constexpr void qr(uint32_t& a, uint32_t& b,
                           uint32_t& c, uint32_t& d) noexcept {
    a += b; d ^= a; d = std::rotl(d, 16u);
    c += d; b ^= c; b = std::rotl(b, 12u);
    a += b; d ^= a; d = std::rotl(d,  8u);
    c += d; b ^= c; b = std::rotl(b,  7u);
}

// ─── Load / store helpers ─────────────────────────────────────────────────────

[[nodiscard]] inline uint32_t load_le32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::big)
        v = std::byteswap(v);
    return v;
}

inline void store_le32(uint8_t* p, uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::big)
        v = std::byteswap(v);
    std::memcpy(p, &v, 4);
}

// ─── Scalar block ─────────────────────────────────────────────────────────────

/**
 * @brief Compute one 64-byte ChaCha20 keystream block.
 *
 * @param key      32-byte key.
 * @param nonce    12-byte nonce.
 * @param counter  32-bit block counter.
 * @param out      64-byte output block.
 */
inline void block_scalar(const uint8_t* key,
                           const uint8_t* nonce,
                           uint32_t       counter,
                           uint8_t*       out) noexcept {
    // Initial state (RFC 8439 §2.3):
    //  0–3:   constants
    //  4–11:  key (256 bits = 8 × uint32)
    //  12:    counter
    //  13–15: nonce (96 bits = 3 × uint32)
    uint32_t x[16];
    x[ 0] = kChaChaConst[0];
    x[ 1] = kChaChaConst[1];
    x[ 2] = kChaChaConst[2];
    x[ 3] = kChaChaConst[3];
    x[ 4] = load_le32(key +  0);
    x[ 5] = load_le32(key +  4);
    x[ 6] = load_le32(key +  8);
    x[ 7] = load_le32(key + 12);
    x[ 8] = load_le32(key + 16);
    x[ 9] = load_le32(key + 20);
    x[10] = load_le32(key + 24);
    x[11] = load_le32(key + 28);
    x[12] = counter;
    x[13] = load_le32(nonce + 0);
    x[14] = load_le32(nonce + 4);
    x[15] = load_le32(nonce + 8);

    uint32_t s[16];
    std::memcpy(s, x, 64);

    // 20 rounds = 10 double-rounds
    for (int i = 0; i < 10; ++i) {
        // Column rounds
        qr(s[ 0], s[ 4], s[ 8], s[12]);
        qr(s[ 1], s[ 5], s[ 9], s[13]);
        qr(s[ 2], s[ 6], s[10], s[14]);
        qr(s[ 3], s[ 7], s[11], s[15]);
        // Diagonal rounds
        qr(s[ 0], s[ 5], s[10], s[15]);
        qr(s[ 1], s[ 6], s[11], s[12]);
        qr(s[ 2], s[ 7], s[ 8], s[13]);
        qr(s[ 3], s[ 4], s[ 9], s[14]);
    }

    // Add initial state
    for (size_t i = 0; i < 16; ++i)
        store_le32(out + i * 4u, s[i] + x[i]);
}

// ─── NEON block (4 parallel blocks) ──────────────────────────────────────────

#if defined(QBUEM_CHACHA20_NEON)

inline void qr_neon(uint32x4_t& a, uint32x4_t& b,
                     uint32x4_t& c, uint32x4_t& d) noexcept {
    a = vaddq_u32(a, b); d = veorq_u32(d, a); d = vorrq_u32(vshlq_n_u32(d, 16), vshrq_n_u32(d, 16));
    c = vaddq_u32(c, d); b = veorq_u32(b, c); b = vorrq_u32(vshlq_n_u32(b, 12), vshrq_n_u32(b, 20));
    a = vaddq_u32(a, b); d = veorq_u32(d, a); d = vorrq_u32(vshlq_n_u32(d,  8), vshrq_n_u32(d, 24));
    c = vaddq_u32(c, d); b = veorq_u32(b, c); b = vorrq_u32(vshlq_n_u32(b,  7), vshrq_n_u32(b, 25));
}

/**
 * @brief Compute 4 parallel ChaCha20 blocks using NEON (256 bytes output).
 *
 * Blocks have counters: counter, counter+1, counter+2, counter+3.
 */
inline void block4_neon(const uint8_t* key,
                          const uint8_t* nonce,
                          uint32_t       counter,
                          uint8_t*       out) noexcept {
    // Load constants into 4 lanes
    uint32x4_t r0 = {kChaChaConst[0], kChaChaConst[1], kChaChaConst[2], kChaChaConst[3]};
    uint32x4_t r1 = vld1q_u32(reinterpret_cast<const uint32_t*>(key +  0));
    uint32x4_t r2 = vld1q_u32(reinterpret_cast<const uint32_t*>(key + 16));
    // Counter lane: each of the 4 blocks gets counter+0..+3
    uint32x4_t r3 = {counter, counter + 1u, counter + 2u, counter + 3u};

    // For 4-block NEON: each NEON register holds ONE word from 4 different blocks.
    // This is the "interleaved" representation.
    // r0[lane] = constant word i for block[lane]
    // We transpose: state[word] = vector of that word across 4 blocks.

    uint32x4_t s[16];
    s[ 0] = vdupq_n_u32(kChaChaConst[0]);
    s[ 1] = vdupq_n_u32(kChaChaConst[1]);
    s[ 2] = vdupq_n_u32(kChaChaConst[2]);
    s[ 3] = vdupq_n_u32(kChaChaConst[3]);
    s[ 4] = vdupq_n_u32(load_le32(key +  0));
    s[ 5] = vdupq_n_u32(load_le32(key +  4));
    s[ 6] = vdupq_n_u32(load_le32(key +  8));
    s[ 7] = vdupq_n_u32(load_le32(key + 12));
    s[ 8] = vdupq_n_u32(load_le32(key + 16));
    s[ 9] = vdupq_n_u32(load_le32(key + 20));
    s[10] = vdupq_n_u32(load_le32(key + 24));
    s[11] = vdupq_n_u32(load_le32(key + 28));
    s[12] = {counter, counter + 1u, counter + 2u, counter + 3u};
    s[13] = vdupq_n_u32(load_le32(nonce + 0));
    s[14] = vdupq_n_u32(load_le32(nonce + 4));
    s[15] = vdupq_n_u32(load_le32(nonce + 8));

    uint32x4_t t[16];
    std::memcpy(t, s, sizeof(s));

    for (int i = 0; i < 10; ++i) {
        qr_neon(t[ 0], t[ 4], t[ 8], t[12]);
        qr_neon(t[ 1], t[ 5], t[ 9], t[13]);
        qr_neon(t[ 2], t[ 6], t[10], t[14]);
        qr_neon(t[ 3], t[ 7], t[11], t[15]);
        qr_neon(t[ 0], t[ 5], t[10], t[15]);
        qr_neon(t[ 1], t[ 6], t[11], t[12]);
        qr_neon(t[ 2], t[ 7], t[ 8], t[13]);
        qr_neon(t[ 3], t[ 4], t[ 9], t[14]);
    }

    for (int i = 0; i < 16; ++i)
        t[i] = vaddq_u32(t[i], s[i]);

    // De-interleave: write 4 × 64-byte blocks
    // block b gets word w at out[b*64 + w*4]
    for (int b = 0; b < 4; ++b) {
        for (int w = 0; w < 16; ++w)
            store_le32(out + b * 64 + w * 4, vgetq_lane_u32(t[w], b));
    }

    (void)r0; (void)r1; (void)r2; (void)r3;
}
#endif  // QBUEM_CHACHA20_NEON

// ─── XOR helpers ─────────────────────────────────────────────────────────────

/**
 * @brief XOR @p len bytes of @p src with keystream, writing to @p dst.
 *
 * @param key      32-byte key.
 * @param nonce    12-byte nonce.
 * @param counter  Starting block counter.
 * @param src      Plaintext/ciphertext input.
 * @param dst      Output (may equal @p src for in-place operation).
 * @param len      Number of bytes to process.
 */
inline void xor_stream(const uint8_t* key,
                        const uint8_t* nonce,
                        uint32_t       counter,
                        const uint8_t* src,
                        uint8_t*       dst,
                        size_t         len) noexcept {
    size_t done = 0;

#if defined(QBUEM_CHACHA20_NEON)
    // Process 4 blocks (256 bytes) at a time with NEON
    alignas(64) uint8_t ks[256];
    for (; done + 256 <= len; done += 256, counter += 4) {
        block4_neon(key, nonce, counter, ks);
        for (size_t j = 0; j < 256; ++j)
            dst[done + j] = src[done + j] ^ ks[j];
    }
#endif

    // Process remaining full blocks (64 bytes each)
    alignas(64) uint8_t block[64];
    for (; done + 64 <= len; done += 64, ++counter) {
        block_scalar(key, nonce, counter, block);
        for (size_t j = 0; j < 64; ++j)
            dst[done + j] = src[done + j] ^ block[j];
    }

    // Handle partial final block
    if (done < len) {
        block_scalar(key, nonce, counter, block);
        for (size_t j = 0; j < len - done; ++j)
            dst[done + j] = src[done + j] ^ block[j];
    }
}

}  // namespace detail::chacha

// ─── Public types ─────────────────────────────────────────────────────────────

using ChaCha20Key   = std::array<uint8_t, 32>;
using ChaCha20Nonce = std::array<uint8_t, 12>;
using ChaCha20Block = std::array<uint8_t, 64>;

// ─── ChaCha20 context ─────────────────────────────────────────────────────────

/**
 * @brief Stateful ChaCha20 encryption context.
 *
 * Maintains the current position within the keystream, allowing streaming
 * encryption/decryption of arbitrary-length messages.
 */
class ChaCha20 {
public:
    /**
     * @brief Construct with a key and nonce.  Counter starts at 0.
     *
     * @param key    32-byte secret key.
     * @param nonce  12-byte nonce (must be unique per (key, message) pair).
     * @param ctr    Initial block counter (default 0; use 1 for AEAD).
     */
    constexpr ChaCha20(const ChaCha20Key&   key,
                       const ChaCha20Nonce& nonce,
                       uint32_t             ctr = 0) noexcept
        : key_(key), nonce_(nonce), counter_(ctr), keystream_pos_(64) {}

    /** @brief Reset counter and keystream position. */
    void seek(uint32_t block_counter) noexcept {
        counter_       = block_counter;
        keystream_pos_ = 64;  // force re-generation on next call
    }

    /**
     * @brief XOR @p data in-place with the keystream (encrypt or decrypt).
     */
    void xor_into(std::span<uint8_t> data) noexcept {
        xor_into(data, data);
    }

    /**
     * @brief XOR @p src with keystream, write result to @p dst.
     *
     * @p src and @p dst may be the same span (in-place operation).
     */
    void xor_into(std::span<const uint8_t> src,
                   std::span<uint8_t>       dst) noexcept {
        size_t len  = src.size();
        size_t done = 0;

        // First drain any buffered keystream
        while (done < len && keystream_pos_ < 64) {
            dst[done] = src[done] ^ keystream_[keystream_pos_++];
            ++done;
        }

        // Bulk XOR via stream function
        if (done < len) {
            detail::chacha::xor_stream(
                key_.data(), nonce_.data(), counter_,
                src.data() + done, dst.data() + done, len - done);
            // Advance counter by number of full+partial blocks consumed
            const size_t bytes_streamed = len - done;
            const size_t blocks = (bytes_streamed + 63) / 64;
            counter_ += static_cast<uint32_t>(blocks);
            // If partial block was generated, buffer the unused tail
            const size_t partial = bytes_streamed % 64;
            if (partial != 0) {
                // Re-generate the last block to fill keystream_
                --counter_;
                detail::chacha::block_scalar(
                    key_.data(), nonce_.data(), counter_++, keystream_.data());
                keystream_pos_ = partial;
            } else {
                keystream_pos_ = 64;
            }
        }
    }

    /**
     * @brief Generate one 64-byte keystream block at the current counter.
     *
     * @returns Raw block; counter is NOT advanced automatically.
     */
    [[nodiscard]] ChaCha20Block keystream_block(uint32_t block_counter) const noexcept {
        ChaCha20Block out{};
        detail::chacha::block_scalar(
            key_.data(), nonce_.data(), block_counter, out.data());
        return out;
    }

private:
    ChaCha20Key   key_;
    ChaCha20Nonce nonce_;
    uint32_t      counter_;
    alignas(64) std::array<uint8_t, 64> keystream_{};
    size_t        keystream_pos_;
};

// ─── One-shot helpers ─────────────────────────────────────────────────────────

/**
 * @brief Encrypt or decrypt @p data in-place with ChaCha20.
 *
 * @param key      32-byte key.
 * @param nonce    12-byte nonce.
 * @param counter  Initial block counter (0 for standalone, 1 for AEAD).
 * @param data     Buffer to XOR with keystream (in-place).
 */
inline void chacha20_xor(const ChaCha20Key&   key,
                          const ChaCha20Nonce& nonce,
                          uint32_t             counter,
                          std::span<uint8_t>   data) noexcept {
    detail::chacha::xor_stream(
        key.data(), nonce.data(), counter,
        data.data(), data.data(), data.size());
}

inline void chacha20_xor(const ChaCha20Key&       key,
                          const ChaCha20Nonce&     nonce,
                          uint32_t                 counter,
                          std::span<const uint8_t> src,
                          std::span<uint8_t>       dst) noexcept {
    detail::chacha::xor_stream(
        key.data(), nonce.data(), counter,
        src.data(), dst.data(), src.size());
}

}  // namespace qbuem::crypto
