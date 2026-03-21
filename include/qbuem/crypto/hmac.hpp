#pragma once

/**
 * @file qbuem/crypto/hmac.hpp
 * @brief HMAC-SHA-256 and HMAC-SHA-512 (RFC 2104).
 * @ingroup qbuem_crypto
 *
 * Generic HMAC template over any SHA context that satisfies the
 * `HashContext` concept.  Pre-computed ipad/opad key blocks are stored in
 * the `HmacContext` struct so repeated calls with the same key reuse the
 * partially-hashed state — no per-call key processing overhead.
 *
 * ### Zero-allocation guarantee
 * All state lives in the `HmacContext` struct.  The `compute()` method
 * returns a fixed-size `std::array` — no heap allocation.
 *
 * ### One-shot convenience functions
 * ```cpp
 * auto tag = qbuem::crypto::hmac_sha256(key, message);
 * auto tag = qbuem::crypto::hmac_sha512(key, message);
 * ```
 *
 * ### Streaming / keyed use
 * ```cpp
 * qbuem::crypto::HmacSha256 h{key};
 * h.update(chunk1);
 * h.update(chunk2);
 * auto tag = h.finalize();
 * // Reset to same key and reuse:
 * h.reset();
 * ```
 *
 * ### Security notes
 * - Keys longer than the block size are hashed down to the digest size.
 * - Keys shorter than the block size are zero-padded.
 * - The output MAC should be compared with `qbuem::crypto::constant_time_equal()`
 *   to prevent timing-oracle attacks on MAC verification.
 */

#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <qbuem/crypto/sha256.hpp>
#include <qbuem/crypto/sha512.hpp>

namespace qbuem::crypto {

// ─── HashContext concept ──────────────────────────────────────────────────────

/**
 * @brief Concept describing a streaming hash context compatible with HMAC.
 *
 * A conforming type must provide:
 *   - `kBlockSize`  — compile-time block size in bytes
 *   - `kDigestSize` — compile-time output size in bytes
 *   - `DigestType`  — `std::array<uint8_t, kDigestSize>`
 *   - `reset()`
 *   - `update(std::span<const uint8_t>)`
 *   - `finalize() -> DigestType`
 */
template <typename H>
concept HashContext = requires(H h, std::span<const uint8_t> s) {
    { H::kBlockSize  } -> std::convertible_to<size_t>;
    { H::kDigestSize } -> std::convertible_to<size_t>;
    { h.reset()      } -> std::same_as<void>;
    { h.update(s)    } -> std::same_as<void>;
    { h.finalize()   };
};

// ─── Sha256Wrapper — wraps Sha256Context for HMAC ────────────────────────────

/**
 * @brief Sha256Context wrapper that exposes `kBlockSize`, `kDigestSize`,
 *        and `DigestType` for the `HashContext` concept.
 */
struct Sha256Wrapper {
    static constexpr size_t kBlockSize  = 64;
    static constexpr size_t kDigestSize = 32;
    using DigestType = Sha256Digest;

    void reset()                          noexcept { ctx_.reset(); }
    void update(std::span<const uint8_t> d) noexcept { ctx_.update(d); }
    [[nodiscard]] DigestType finalize() const noexcept { return ctx_.finalize(); }

    // Hash a full buffer (key derivation helper)
    [[nodiscard]] static DigestType hash(std::span<const uint8_t> d) noexcept {
        return sha256(d);
    }

private:
    Sha256Context ctx_;
};

/**
 * @brief Sha512Context wrapper for HMAC.
 */
struct Sha512Wrapper {
    static constexpr size_t kBlockSize  = 128;
    static constexpr size_t kDigestSize = 64;
    using DigestType = Sha512Digest;

    void reset()                            noexcept { ctx_.reset(); }
    void update(std::span<const uint8_t> d)  noexcept { ctx_.update(d); }
    [[nodiscard]] DigestType finalize() const noexcept { return ctx_.finalize(); }

    [[nodiscard]] static DigestType hash(std::span<const uint8_t> d) noexcept {
        return sha512(d);
    }

private:
    Sha512Context ctx_;
};

static_assert(HashContext<Sha256Wrapper>);
static_assert(HashContext<Sha512Wrapper>);

// ─── Generic HmacContext ─────────────────────────────────────────────────────

/**
 * @brief Generic HMAC context parameterised on a hash wrapper type.
 *
 * @tparam H  Hash wrapper type satisfying `HashContext`.
 *
 * ### Implementation
 * RFC 2104:
 *   HMAC(K, M) = H((K' ⊕ opad) || H((K' ⊕ ipad) || M))
 *
 * Where:
 *   - K' = H(K) if |K| > blockSize, else zero-padded K to blockSize
 *   - ipad = 0x36 repeated blockSize times
 *   - opad = 0x5C repeated blockSize times
 *
 * The inner hash `H((K' ⊕ ipad) || M)` is computed incrementally via
 * `update()`.  `finalize()` applies the outer hash.
 */
template <HashContext H>
class HmacContext {
public:
    static constexpr size_t kBlockSize  = H::kBlockSize;
    static constexpr size_t kDigestSize = H::kDigestSize;
    using DigestType = typename H::DigestType;

    /** @brief Construct and key the HMAC context. */
    explicit HmacContext(std::span<const uint8_t> key) noexcept {
        init(key);
    }

    /** @brief Construct from string_view key. */
    explicit HmacContext(std::string_view key) noexcept
        : HmacContext({reinterpret_cast<const uint8_t*>(key.data()), key.size()}) {}

    /**
     * @brief Reset to the same key state (re-initialize inner hash).
     * Avoids re-processing the key.
     */
    void reset() noexcept {
        inner_.reset();
        inner_.update({ipad_.data(), ipad_.size()});
    }

    /** @brief Feed data into the inner hash. */
    void update(std::span<const uint8_t> data) noexcept {
        inner_.update(data);
    }

    /** @brief Feed string data. */
    void update(std::string_view sv) noexcept {
        inner_.update({reinterpret_cast<const uint8_t*>(sv.data()), sv.size()});
    }

    /**
     * @brief Compute the final HMAC tag.
     *
     * The internal state is copied before finalizing — the context can be
     * reused by calling `reset()`.
     */
    [[nodiscard]] DigestType finalize() const noexcept {
        // Inner digest
        const DigestType inner_digest = inner_.finalize();

        // Outer hash: H(opad || inner_digest)
        H outer;
        outer.update({opad_.data(), opad_.size()});
        outer.update({inner_digest.data(), inner_digest.size()});
        return outer.finalize();
    }

    /**
     * @brief One-shot: compute HMAC over a single message with the current key.
     *
     * Equivalent to `reset(); update(msg); return finalize();`
     */
    [[nodiscard]] DigestType compute(std::span<const uint8_t> msg) noexcept {
        reset();
        update(msg);
        return finalize();
    }

    [[nodiscard]] DigestType compute(std::string_view msg) noexcept {
        return compute({reinterpret_cast<const uint8_t*>(msg.data()), msg.size()});
    }

private:
    H inner_;
    // Padded key XOR'd with ipad/opad — stored to avoid re-computation on reset()
    std::array<uint8_t, H::kBlockSize> ipad_{};
    std::array<uint8_t, H::kBlockSize> opad_{};

    void init(std::span<const uint8_t> key) noexcept {
        std::array<uint8_t, H::kBlockSize> kp{};  // k-prime (normalized key)

        if (key.size() > kBlockSize) {
            // Hash the key down to digest size
            const auto digest = H::hash(key);
            std::memcpy(kp.data(), digest.data(), digest.size());
        } else {
            std::memcpy(kp.data(), key.data(), key.size());
            // Remaining bytes already zero-initialized
        }

        // Compute ipad = kp ⊕ 0x36  and  opad = kp ⊕ 0x5C
        for (size_t i = 0; i < kBlockSize; ++i) {
            ipad_[i] = kp[i] ^ 0x36u;
            opad_[i] = kp[i] ^ 0x5Cu;
        }

        // Start inner hash with ipad prefix
        inner_.reset();
        inner_.update({ipad_.data(), ipad_.size()});
    }
};

// ─── Named aliases ────────────────────────────────────────────────────────────

/** @brief HMAC-SHA-256 context (RFC 2104 + SHA-256). */
using HmacSha256 = HmacContext<Sha256Wrapper>;

/** @brief HMAC-SHA-512 context (RFC 2104 + SHA-512). */
using HmacSha512 = HmacContext<Sha512Wrapper>;

// ─── One-shot convenience functions ───────────────────────────────────────────

/**
 * @brief Compute HMAC-SHA-256 in one call.
 *
 * @param key     HMAC key (any length).
 * @param message Input message.
 * @returns 32-byte HMAC tag.
 */
[[nodiscard]] inline Sha256Digest hmac_sha256(std::span<const uint8_t> key,
                                               std::span<const uint8_t> message) noexcept {
    HmacSha256 h{key};
    h.update(message);
    return h.finalize();
}

[[nodiscard]] inline Sha256Digest hmac_sha256(std::string_view key,
                                               std::string_view message) noexcept {
    HmacSha256 h{key};
    h.update(message);
    return h.finalize();
}

/**
 * @brief Compute HMAC-SHA-512 in one call.
 *
 * @param key     HMAC key (any length).
 * @param message Input message.
 * @returns 64-byte HMAC tag.
 */
[[nodiscard]] inline Sha512Digest hmac_sha512(std::span<const uint8_t> key,
                                               std::span<const uint8_t> message) noexcept {
    HmacSha512 h{key};
    h.update(message);
    return h.finalize();
}

[[nodiscard]] inline Sha512Digest hmac_sha512(std::string_view key,
                                               std::string_view message) noexcept {
    HmacSha512 h{key};
    h.update(message);
    return h.finalize();
}

// ─── Constant-time HMAC verification ─────────────────────────────────────────

/**
 * @brief Verify an HMAC-SHA-256 tag in constant time.
 *
 * @param key      HMAC key.
 * @param message  The message that was authenticated.
 * @param expected Expected 32-byte tag.
 * @returns true iff the computed tag equals @p expected.
 *
 * @note Uses volatile XOR accumulation to prevent early-exit optimisation.
 */
[[nodiscard]] inline bool verify_hmac_sha256(std::span<const uint8_t> key,
                                              std::span<const uint8_t> message,
                                              const Sha256Digest&      expected) noexcept {
    const Sha256Digest computed = hmac_sha256(key, message);
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < 32; ++i)
        diff |= computed[i] ^ expected[i];
    return diff == 0;
}

[[nodiscard]] inline bool verify_hmac_sha512(std::span<const uint8_t> key,
                                              std::span<const uint8_t> message,
                                              const Sha512Digest&      expected) noexcept {
    const Sha512Digest computed = hmac_sha512(key, message);
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < 64; ++i)
        diff |= computed[i] ^ expected[i];
    return diff == 0;
}

}  // namespace qbuem::crypto
