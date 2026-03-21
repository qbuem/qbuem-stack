#pragma once

/**
 * @file qbuem/crypto/poly1305.hpp
 * @brief Poly1305 one-time MAC (RFC 8439 / RFC 7539).
 * @ingroup qbuem_crypto
 *
 * Poly1305 authenticates a message using a 256-bit one-time key:
 * - 128-bit "r" (clamped) — the polynomial evaluation key
 * - 128-bit "s" — the final addition key
 *
 * The computation is over GF(2^130 - 5):
 *   acc = ((acc + msg_block) * r) mod (2^130 - 5)
 *   tag = (acc + s) mod 2^128
 *
 * ### Constant-time guarantee
 * All arithmetic uses 64-bit integer multiplications with carry propagation.
 * No secret-dependent branches or table lookups — safe against timing attacks.
 *
 * ### Zero-allocation design
 * All state fits in the `Poly1305Context` struct (stack only).
 *
 * ### Usage
 * ```cpp
 * std::array<uint8_t, 32> one_time_key = ...;  // from ChaCha20 block 0
 *
 * qbuem::crypto::Poly1305 mac{one_time_key};
 * mac.update(aad);
 * mac.update(ciphertext);
 * auto tag = mac.finalize();  // std::array<uint8_t, 16>
 *
 * // Or one-shot:
 * auto tag = qbuem::crypto::poly1305(one_time_key, message);
 * ```
 *
 * @note The one-time key MUST NOT be reused across messages.
 *       In ChaCha20-Poly1305 AEAD, it is derived fresh per message
 *       as the first 32 bytes of the ChaCha20 keystream block 0.
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace qbuem::crypto {

// ─── Poly1305 key and tag types ───────────────────────────────────────────────

using Poly1305Key = std::array<uint8_t, 32>;
using Poly1305Tag = std::array<uint8_t, 16>;

// ─── Implementation detail ────────────────────────────────────────────────────

namespace detail::poly {

/**
 * @brief Load a little-endian uint64_t from memory.
 */
[[nodiscard]] inline uint64_t load_le64(const uint8_t* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

inline void store_le64(uint8_t* p, uint64_t v) noexcept {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    std::memcpy(p, &v, 8);
}

[[nodiscard]] inline uint32_t load_le32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

// ─── 130-bit accumulator arithmetic using 5 × 26-bit limbs ──────────────────
//
// We represent numbers in the range [0, 2^130) as:
//   h[0] + h[1]*2^26 + h[2]*2^52 + h[3]*2^78 + h[4]*2^104
// where h[i] < 2^26 (after reduction).
//
// Multiplication uses 64-bit intermediate products with carry propagation.
// This avoids __uint128_t for maximum portability.

struct State {
    // Accumulator: 5 × 26-bit limbs (may temporarily hold > 26 bits between reductions)
    uint64_t h[5] = {};

    // r: clamped (128-bit) evaluation key, split into 5 × 26-bit limbs
    uint32_t r[5] = {};

    // s: 128-bit addition key
    uint32_t s[4] = {};
};

/**
 * @brief Clamp r according to RFC 8439 §2.5.1.
 *
 * r &= 0x0ffffffc0ffffffc0ffffffc0fffffff
 */
inline void clamp_r(uint8_t* r16) noexcept {
    r16[ 3] &= 15u;
    r16[ 7] &= 15u;
    r16[11] &= 15u;
    r16[15] &= 15u;
    r16[ 4] &= 252u;
    r16[ 8] &= 252u;
    r16[12] &= 252u;
}

/**
 * @brief Load key, clamp r, and split into limbs.
 */
inline void init_state(State& st, const uint8_t* key) noexcept {
    // Copy r (first 16 bytes) and apply clamp
    uint8_t r16[16];
    std::memcpy(r16, key, 16);
    clamp_r(r16);

    // Pack r into 5 × 26-bit limbs (little-endian)
    const uint64_t r0 = load_le32(r16 +  0);
    const uint64_t r1 = load_le32(r16 +  3) >> 2;
    const uint64_t r2 = load_le32(r16 +  6) >> 4;
    const uint64_t r3 = load_le32(r16 +  9) >> 6;
    const uint64_t r4 = load_le32(r16 + 12) >> 8;

    st.r[0] = static_cast<uint32_t>(r0 & 0x3FFFFFFu);
    st.r[1] = static_cast<uint32_t>(r1 & 0x3FFFFFFu);
    st.r[2] = static_cast<uint32_t>(r2 & 0x3FFFFFFu);
    st.r[3] = static_cast<uint32_t>(r3 & 0x3FFFFFFu);
    st.r[4] = static_cast<uint32_t>(r4 & 0x3FFFFFFu);

    // Load s (last 16 bytes of key)
    st.s[0] = load_le32(key + 16);
    st.s[1] = load_le32(key + 20);
    st.s[2] = load_le32(key + 24);
    st.s[3] = load_le32(key + 28);
}

/**
 * @brief Process one 16-byte (or shorter) message block.
 *
 * @param st      Poly1305 state.
 * @param block   Input block (16 bytes or less for final block).
 * @param len     Block length (1–16 bytes).
 * @param is_last True if this is the final (possibly partial) block.
 *                When true, adds a high bit to represent the message boundary.
 */
inline void process_block(State& st, const uint8_t* block,
                            size_t len, bool is_last) noexcept {
    // Load block into a 130-bit value:
    //   m = block_bytes || (is_last ? 0x01 : 0x00) at byte `len`
    uint64_t m[5] = {};
    {
        uint8_t buf[17] = {};
        std::memcpy(buf, block, len);
        buf[len] = 0x01u;  // append 1 bit (RFC 8439 §2.5.1 — always set for full blocks too)
        if (!is_last) {
            // For full 16-byte blocks the 1-bit is at position 128 (byte 16)
            // The buffer is already correct.
        }

        const uint64_t t0 = load_le32(buf +  0);
        const uint64_t t1 = load_le32(buf +  3) >> 2;
        const uint64_t t2 = load_le32(buf +  6) >> 4;
        const uint64_t t3 = load_le32(buf +  9) >> 6;
        const uint64_t t4 = load_le32(buf + 12) >> 8;

        m[0] = t0 & 0x3FFFFFFu;
        m[1] = t1 & 0x3FFFFFFu;
        m[2] = t2 & 0x3FFFFFFu;
        m[3] = t3 & 0x3FFFFFFu;
        m[4] = t4;  // may be > 26 bits (includes the appended 1)
    }

    // h += m
    uint64_t h0 = st.h[0] + m[0];
    uint64_t h1 = st.h[1] + m[1];
    uint64_t h2 = st.h[2] + m[2];
    uint64_t h3 = st.h[3] + m[3];
    uint64_t h4 = st.h[4] + m[4];

    // h *= r (mod 2^130 - 5)
    // Using the identity: 2^130 ≡ 5 (mod 2^130-5)
    // So h[i] * r[j] where i+j >= 5 contributes * 5 to the low part.
    const uint64_t r0 = st.r[0];
    const uint64_t r1 = st.r[1];
    const uint64_t r2 = st.r[2];
    const uint64_t r3 = st.r[3];
    const uint64_t r4 = st.r[4];

    // Precompute 5*r[i] for the reduction
    const uint64_t r1_5 = r1 * 5u;
    const uint64_t r2_5 = r2 * 5u;
    const uint64_t r3_5 = r3 * 5u;
    const uint64_t r4_5 = r4 * 5u;

    uint64_t d0 = h0*r0 + h1*r4_5 + h2*r3_5 + h3*r2_5 + h4*r1_5;
    uint64_t d1 = h0*r1 + h1*r0   + h2*r4_5 + h3*r3_5 + h4*r2_5;
    uint64_t d2 = h0*r2 + h1*r1   + h2*r0   + h3*r4_5 + h4*r3_5;
    uint64_t d3 = h0*r3 + h1*r2   + h2*r1   + h3*r0   + h4*r4_5;
    uint64_t d4 = h0*r4 + h1*r3   + h2*r2   + h3*r1   + h4*r0;

    // Propagate carries and reduce mod 2^130-5
    const uint64_t c0 = d0 >> 26; d0 &= 0x3FFFFFFu;
    d1 += c0;
    const uint64_t c1 = d1 >> 26; d1 &= 0x3FFFFFFu;
    d2 += c1;
    const uint64_t c2 = d2 >> 26; d2 &= 0x3FFFFFFu;
    d3 += c2;
    const uint64_t c3 = d3 >> 26; d3 &= 0x3FFFFFFu;
    d4 += c3;
    const uint64_t c4 = d4 >> 26; d4 &= 0x3FFFFFFu;
    d0 += c4 * 5u;
    const uint64_t c5 = d0 >> 26; d0 &= 0x3FFFFFFu;
    d1 += c5;

    st.h[0] = d0;
    st.h[1] = d1;
    st.h[2] = d2;
    st.h[3] = d3;
    st.h[4] = d4;
}

/**
 * @brief Finalize: serialize the tag.
 *
 * Computes (h + s) mod 2^128 and stores the 16-byte tag.
 */
inline Poly1305Tag finalize(const State& st) noexcept {
    // Carry propagation for final reduction
    uint64_t h0 = st.h[0];
    uint64_t h1 = st.h[1];
    uint64_t h2 = st.h[2];
    uint64_t h3 = st.h[3];
    uint64_t h4 = st.h[4];

    uint64_t c;
    c = h1 >> 26; h1 &= 0x3FFFFFFu; h2 += c;
    c = h2 >> 26; h2 &= 0x3FFFFFFu; h3 += c;
    c = h3 >> 26; h3 &= 0x3FFFFFFu; h4 += c;
    c = h4 >> 26; h4 &= 0x3FFFFFFu; h0 += c * 5u;
    c = h0 >> 26; h0 &= 0x3FFFFFFu; h1 += c;

    // Compute h + -p (where p = 2^130 - 5), check if h >= p
    uint64_t g0 = h0 + 5u;
    c = g0 >> 26; g0 &= 0x3FFFFFFu;
    uint64_t g1 = h1 + c;
    c = g1 >> 26; g1 &= 0x3FFFFFFu;
    uint64_t g2 = h2 + c;
    c = g2 >> 26; g2 &= 0x3FFFFFFu;
    uint64_t g3 = h3 + c;
    c = g3 >> 26; g3 &= 0x3FFFFFFu;
    uint64_t g4 = h4 + c - (1u << 26u);

    // If h >= p (g4 < 0 → high bit set), select g; else keep h
    const uint64_t mask = (~(g4 >> 63u)) & 0x3FFFFFFu;
    const uint64_t nmask = ~mask & 0x3FFFFFFu;
    h0 = (h0 & nmask) | (g0 & mask);
    h1 = (h1 & nmask) | (g1 & mask);
    h2 = (h2 & nmask) | (g2 & mask);
    h3 = (h3 & nmask) | (g3 & mask);
    h4 = (h4 & nmask) | (g4 & mask);

    // Pack 5 limbs into 128 bits
    uint64_t f0 = (h0      ) | (h1 << 26u);
    uint64_t f1 = (h1 >> 6u) | (h2 << 20u);
    uint64_t f2 = (h2 >> 12u)| (h3 << 14u);
    uint64_t f3 = (h3 >> 18u)| (h4 <<  8u);

    // Add s (128-bit little-endian)
    f0 += static_cast<uint64_t>(st.s[0]) | (static_cast<uint64_t>(st.s[1]) << 32u);
    const uint64_t carry0 = f0 >> 32u;
    f1 += (static_cast<uint64_t>(st.s[2]) | (static_cast<uint64_t>(st.s[3]) << 32u)) + carry0;

    Poly1305Tag tag{};
    store_le64(tag.data() + 0, f0);
    store_le64(tag.data() + 8, f1);
    (void)f2; (void)f3;
    return tag;
}

}  // namespace detail::poly

// ─── Poly1305 context ─────────────────────────────────────────────────────────

/**
 * @brief Poly1305 streaming MAC context.
 *
 * Feed message data with `update()`, then call `finalize()` for the 16-byte tag.
 */
class Poly1305 {
public:
    /** @brief Construct and key the MAC with a 32-byte one-time key. */
    explicit Poly1305(const Poly1305Key& key) noexcept { init(key); }
    explicit Poly1305(std::span<const uint8_t> key) noexcept {
        Poly1305Key k{};
        std::memcpy(k.data(), key.data(), std::min(key.size(), k.size()));
        init(k);
    }

    /** @brief Feed message bytes into the MAC. */
    void update(std::span<const uint8_t> data) noexcept {
        const uint8_t* src = data.data();
        size_t         len = data.size();

        // Fill partial buffer
        if (buf_len_ > 0) {
            const size_t space = 16u - buf_len_;
            const size_t take  = len < space ? len : space;
            std::memcpy(buf_.data() + buf_len_, src, take);
            buf_len_ += take;
            src += take;
            len -= take;
            if (buf_len_ == 16) {
                detail::poly::process_block(state_, buf_.data(), 16, false);
                buf_len_ = 0;
            }
        }

        // Process full 16-byte blocks
        while (len >= 16) {
            detail::poly::process_block(state_, src, 16, false);
            src += 16;
            len -= 16;
        }

        // Buffer remainder
        if (len > 0) {
            std::memcpy(buf_.data(), src, len);
            buf_len_ = len;
        }
    }

    /**
     * @brief Compute and return the 16-byte authentication tag.
     *
     * Finalizes any buffered partial block, then applies the addition key.
     * The context is invalidated after calling `finalize()`.
     */
    [[nodiscard]] Poly1305Tag finalize() noexcept {
        if (buf_len_ > 0) {
            detail::poly::process_block(state_, buf_.data(), buf_len_, true);
            buf_len_ = 0;
        }
        return detail::poly::finalize(state_);
    }

private:
    detail::poly::State        state_{};
    std::array<uint8_t, 16>    buf_{};
    size_t                     buf_len_ = 0;

    void init(const Poly1305Key& key) noexcept {
        state_ = {};
        buf_.fill(0);
        buf_len_ = 0;
        detail::poly::init_state(state_, key.data());
    }
};

// ─── One-shot helper ──────────────────────────────────────────────────────────

/**
 * @brief Compute a Poly1305 MAC in one call.
 *
 * @param key     32-byte one-time key.
 * @param message Input message.
 * @returns 16-byte authentication tag.
 */
[[nodiscard]] inline Poly1305Tag poly1305(const Poly1305Key&       key,
                                           std::span<const uint8_t> message) noexcept {
    Poly1305 mac{key};
    mac.update(message);
    return mac.finalize();
}

/**
 * @brief Verify a Poly1305 tag in constant time.
 *
 * @returns true iff the computed tag equals @p expected.
 */
[[nodiscard]] inline bool poly1305_verify(const Poly1305Key&       key,
                                           std::span<const uint8_t> message,
                                           const Poly1305Tag&       expected) noexcept {
    const Poly1305Tag computed = poly1305(key, message);
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i)
        diff |= computed[i] ^ expected[i];
    return diff == 0;
}

}  // namespace qbuem::crypto
