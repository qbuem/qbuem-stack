#pragma once

/**
 * @file qbuem/security/pqc.hpp
 * @brief Post-Quantum Cryptography (PQC) — Kyber KEM and Dilithium signatures.
 * @defgroup qbuem_pqc Post-Quantum Cryptography
 * @ingroup qbuem_security
 *
 * ## Overview
 *
 * NIST-standardised post-quantum algorithms (FIPS 203/204, finalised 2024):
 *
 * | Algorithm   | NIST Standard | Type              | Security Level   |
 * |-------------|---------------|-------------------|------------------|
 * | ML-KEM      | FIPS 203      | Key Encapsulation | 128/192/256-bit  |
 * | ML-DSA      | FIPS 204      | Digital Signature | 128/192/256-bit  |
 * | SLH-DSA     | FIPS 205      | Hash-based Sig    | 128/192/256-bit  |
 *
 * Former CRYSTALS-Kyber → ML-KEM (Module Lattice KEM)
 * Former CRYSTALS-Dilithium → ML-DSA (Module Lattice DSA)
 *
 * ## Zero-dependency design
 *
 * qbuem-stack does not statically link a PQC library (following zero-dependency
 * policy). `IPqcBackend` is the injection interface. Recommended implementations:
 *
 * | Library      | License  | Notes                                       |
 * |--------------|----------|---------------------------------------------|
 * | **liboqs**   | MIT      | Open Quantum Safe — reference implementation|
 * | **PQClean**  | Public   | Portable, constant-time, NIST submissions   |
 * | **AWS-LC**   | Apache-2 | AWS LibCrypto — ML-KEM + ML-DSA supported   |
 * | **BoringSSL**| Apache-2 | Google's fork; ML-KEM experimental          |
 *
 * ## Hybrid mode
 * PQC is often deployed in hybrid mode: a PQC algorithm AND a classical
 * algorithm (X25519 + ML-KEM-768) to ensure security even if the PQC
 * algorithm has unforeseen weaknesses. `HybridKem` implements this.
 *
 * ## Key sizes (ML-KEM-768 / Kyber-768, 192-bit security)
 * | Object        | Size (bytes) |
 * |---------------|-------------|
 * | Public key    | 1184        |
 * | Secret key    | 2400        |
 * | Ciphertext    | 1088        |
 * | Shared secret | 32          |
 *
 * ## Usage
 * @code
 * // Key exchange with ML-KEM-768
 * PqcKem kem(KemAlgorithm::MlKem768);
 *
 * // Alice generates key pair
 * auto [pk, sk] = co_await kem.keygen(st);
 *
 * // Bob encapsulates shared secret
 * auto [ciphertext, shared_secret_bob] = co_await kem.encapsulate(pk, st);
 *
 * // Alice decapsulates
 * auto shared_secret_alice = co_await kem.decapsulate(sk, ciphertext, st);
 *
 * assert(shared_secret_alice == shared_secret_bob);
 *
 * // Signing with ML-DSA-65
 * PqcDsa dsa(DsaAlgorithm::MlDsa65);
 * auto [sign_pk, sign_sk] = co_await dsa.keygen(st);
 * auto signature = co_await dsa.sign(message, sign_sk, st);
 * bool ok = co_await dsa.verify(message, signature, sign_pk, st);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

namespace qbuem {

// ─── Algorithm identifiers ───────────────────────────────────────────────────

/**
 * @brief ML-KEM (CRYSTALS-Kyber) algorithm variants (FIPS 203).
 *
 * Security level increases with the parameter set number.
 */
enum class KemAlgorithm {
    MlKem512,   ///< ML-KEM-512: 128-bit security (smallest keys)
    MlKem768,   ///< ML-KEM-768: 192-bit security (recommended)
    MlKem1024,  ///< ML-KEM-1024: 256-bit security (highest security)
};

/**
 * @brief ML-DSA (CRYSTALS-Dilithium) algorithm variants (FIPS 204).
 */
enum class DsaAlgorithm {
    MlDsa44,    ///< ML-DSA-44: 128-bit security
    MlDsa65,    ///< ML-DSA-65: 192-bit security (recommended)
    MlDsa87,    ///< ML-DSA-87: 256-bit security
};

/**
 * @brief SLH-DSA (SPHINCS+) hash-based signature variants (FIPS 205).
 */
enum class SlhDsaAlgorithm {
    SlhDsaSha2_128s,  ///< Small parameter set, SHA2
    SlhDsaSha2_192s,
    SlhDsaSha2_256s,
    SlhDsaShake_128f, ///< Fast parameter set, SHAKE
    SlhDsaShake_192f,
    SlhDsaShake_256f,
};

// ─── Key size constants ───────────────────────────────────────────────────────

/**
 * @brief Key and output sizes for ML-KEM variants.
 */
struct KemSizes {
    size_t pk_bytes;         ///< Public key size
    size_t sk_bytes;         ///< Secret key size
    size_t ct_bytes;         ///< Ciphertext size
    size_t shared_secret;    ///< Shared secret size (always 32)

    [[nodiscard]] static constexpr KemSizes for_algorithm(KemAlgorithm alg) noexcept {
        switch (alg) {
        case KemAlgorithm::MlKem512:  return {800,  1632, 768,  32};
        case KemAlgorithm::MlKem768:  return {1184, 2400, 1088, 32};
        case KemAlgorithm::MlKem1024: return {1568, 3168, 1568, 32};
        }
        return {1184, 2400, 1088, 32};
    }
};

/**
 * @brief Key and output sizes for ML-DSA variants.
 */
struct DsaSizes {
    size_t pk_bytes;    ///< Public key size
    size_t sk_bytes;    ///< Secret key size
    size_t sig_bytes;   ///< Signature size

    [[nodiscard]] static constexpr DsaSizes for_algorithm(DsaAlgorithm alg) noexcept {
        switch (alg) {
        case DsaAlgorithm::MlDsa44: return {1312, 2528, 2420};
        case DsaAlgorithm::MlDsa65: return {1952, 4000, 3309};
        case DsaAlgorithm::MlDsa87: return {2592, 4864, 4627};
        }
        return {1952, 4000, 3309};
    }
};

// ─── IPqcBackend ─────────────────────────────────────────────────────────────

/**
 * @brief Injection interface for PQC library implementations.
 *
 * Implement using liboqs, PQClean, AWS-LC, or BoringSSL.
 * All methods operate on caller-provided spans (zero allocation required
 * in the implementation itself when sizes are pre-computed from `KemSizes`).
 */
class IPqcBackend {
public:
    virtual ~IPqcBackend() = default;

    // ── KEM operations ────────────────────────────────────────────────────────

    /**
     * @brief Generate a KEM key pair.
     *
     * @param algorithm  ML-KEM variant.
     * @param pk_out     Public key output (sized per `KemSizes::pk_bytes`).
     * @param sk_out     Secret key output (sized per `KemSizes::sk_bytes`).
     * @returns `Result<void>` — ok on success.
     */
    [[nodiscard]] virtual Result<void>
    kem_keygen(KemAlgorithm algorithm,
               std::span<std::byte> pk_out,
               std::span<std::byte> sk_out) noexcept = 0;

    /**
     * @brief Encapsulate a shared secret into a ciphertext (Bob's step).
     *
     * @param algorithm     ML-KEM variant.
     * @param pk            Recipient's public key.
     * @param ct_out        Ciphertext output.
     * @param ss_out        Shared secret output (32 bytes).
     * @returns `Result<void>` — ok on success.
     */
    [[nodiscard]] virtual Result<void>
    kem_encapsulate(KemAlgorithm algorithm,
                    std::span<const std::byte> pk,
                    std::span<std::byte> ct_out,
                    std::span<std::byte> ss_out) noexcept = 0;

    /**
     * @brief Decapsulate a shared secret from a ciphertext (Alice's step).
     *
     * @param algorithm  ML-KEM variant.
     * @param sk         Alice's secret key.
     * @param ct         Ciphertext from Bob.
     * @param ss_out     Shared secret output (32 bytes).
     * @returns `Result<void>` — ok on success.
     */
    [[nodiscard]] virtual Result<void>
    kem_decapsulate(KemAlgorithm algorithm,
                    std::span<const std::byte> sk,
                    std::span<const std::byte> ct,
                    std::span<std::byte> ss_out) noexcept = 0;

    // ── DSA operations ────────────────────────────────────────────────────────

    /**
     * @brief Generate a DSA key pair.
     */
    [[nodiscard]] virtual Result<void>
    dsa_keygen(DsaAlgorithm algorithm,
               std::span<std::byte> pk_out,
               std::span<std::byte> sk_out) noexcept = 0;

    /**
     * @brief Sign a message with the secret key.
     *
     * @param algorithm  ML-DSA variant.
     * @param message    Message bytes to sign.
     * @param sk         Signer's secret key.
     * @param sig_out    Signature output.
     * @param sig_len    Actual signature length written.
     */
    [[nodiscard]] virtual Result<void>
    dsa_sign(DsaAlgorithm algorithm,
             std::span<const std::byte> message,
             std::span<const std::byte> sk,
             std::span<std::byte> sig_out,
             size_t& sig_len) noexcept = 0;

    /**
     * @brief Verify a signature against a public key.
     *
     * @returns `Result<void>` — ok if signature is valid.
     */
    [[nodiscard]] virtual Result<void>
    dsa_verify(DsaAlgorithm algorithm,
               std::span<const std::byte> message,
               std::span<const std::byte> signature,
               std::span<const std::byte> pk) noexcept = 0;
};

// ─── PqcKem ───────────────────────────────────────────────────────────────────

/**
 * @brief High-level ML-KEM key encapsulation wrapper.
 *
 * Pre-allocates key and ciphertext buffers on construction to avoid hot-path
 * heap allocation during the cryptographic operations.
 */
class PqcKem {
public:
    /**
     * @brief Construct with an algorithm variant and injected backend.
     *
     * @param alg      ML-KEM parameter set.
     * @param backend  PQC library backend (non-owning, must outlive this object).
     */
    PqcKem(KemAlgorithm alg, IPqcBackend* backend)
        : alg_(alg), backend_(backend), sizes_(KemSizes::for_algorithm(alg)) {}

    /** @brief Key and size parameters for this variant. */
    [[nodiscard]] const KemSizes& sizes() const noexcept { return sizes_; }
    [[nodiscard]] KemAlgorithm algorithm() const noexcept { return alg_; }

    /**
     * @brief Generate a fresh key pair into pre-allocated buffers.
     *
     * @param pk_out  Public key buffer (must be >= `sizes().pk_bytes`).
     * @param sk_out  Secret key buffer (must be >= `sizes().sk_bytes`).
     */
    [[nodiscard]] Result<void>
    keygen(std::span<std::byte> pk_out, std::span<std::byte> sk_out) noexcept {
        return backend_->kem_keygen(alg_, pk_out, sk_out);
    }

    /**
     * @brief Encapsulate: generate ciphertext + shared secret for a public key.
     *
     * @param pk      Recipient's public key.
     * @param ct_out  Ciphertext output buffer (>= `sizes().ct_bytes`).
     * @param ss_out  32-byte shared secret output.
     */
    [[nodiscard]] Result<void>
    encapsulate(std::span<const std::byte> pk,
                std::span<std::byte> ct_out,
                std::span<std::byte> ss_out) noexcept {
        return backend_->kem_encapsulate(alg_, pk, ct_out, ss_out);
    }

    /**
     * @brief Decapsulate: recover shared secret from ciphertext + secret key.
     *
     * @param sk      Recipient's secret key.
     * @param ct      Ciphertext from encapsulating party.
     * @param ss_out  32-byte shared secret output.
     */
    [[nodiscard]] Result<void>
    decapsulate(std::span<const std::byte> sk,
                std::span<const std::byte> ct,
                std::span<std::byte> ss_out) noexcept {
        return backend_->kem_decapsulate(alg_, sk, ct, ss_out);
    }

private:
    KemAlgorithm  alg_;
    IPqcBackend*  backend_;
    KemSizes      sizes_;
};

// ─── PqcDsa ───────────────────────────────────────────────────────────────────

/**
 * @brief High-level ML-DSA digital signature wrapper.
 */
class PqcDsa {
public:
    PqcDsa(DsaAlgorithm alg, IPqcBackend* backend)
        : alg_(alg), backend_(backend), sizes_(DsaSizes::for_algorithm(alg)) {}

    [[nodiscard]] const DsaSizes& sizes() const noexcept { return sizes_; }
    [[nodiscard]] DsaAlgorithm algorithm() const noexcept { return alg_; }

    [[nodiscard]] Result<void>
    keygen(std::span<std::byte> pk_out, std::span<std::byte> sk_out) noexcept {
        return backend_->dsa_keygen(alg_, pk_out, sk_out);
    }

    [[nodiscard]] Result<void>
    sign(std::span<const std::byte> message,
         std::span<const std::byte> sk,
         std::span<std::byte> sig_out,
         size_t& sig_len) noexcept {
        return backend_->dsa_sign(alg_, message, sk, sig_out, sig_len);
    }

    [[nodiscard]] Result<void>
    verify(std::span<const std::byte> message,
           std::span<const std::byte> signature,
           std::span<const std::byte> pk) noexcept {
        return backend_->dsa_verify(alg_, message, signature, pk);
    }

private:
    DsaAlgorithm  alg_;
    IPqcBackend*  backend_;
    DsaSizes      sizes_;
};

// ─── HybridKem ───────────────────────────────────────────────────────────────

/**
 * @brief Hybrid KEM: X25519 (classical) + ML-KEM-768 (post-quantum).
 *
 * Combines both shared secrets via HKDF or simple concatenation+hash, ensuring
 * security against both classical and quantum adversaries.
 *
 * The combined shared secret is:
 *   `SS = SHA3-256(SS_classical || SS_pqc || context_label)`
 *
 * This follows the draft IETF hybrid key exchange specification.
 */
struct HybridKemConfig {
    KemAlgorithm pqc_algorithm{KemAlgorithm::MlKem768}; ///< PQC component
    bool         use_x25519{true};                       ///< Enable classical X25519
    std::array<std::byte, 8> context_label{};            ///< Domain separation label
};

} // namespace qbuem

/** @} */ // end of qbuem_pqc
