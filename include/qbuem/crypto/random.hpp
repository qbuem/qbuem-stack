#pragma once

/**
 * @file qbuem/crypto/random.hpp
 * @brief Cryptographically secure random number generator (CSPRNG).
 * @ingroup qbuem_crypto
 *
 * Provides zero-dependency, high-performance cryptographically secure
 * random byte generation using the best available kernel interface:
 *
 * | Platform        | Source                                         |
 * |-----------------|------------------------------------------------|
 * | Linux ≥ 3.17    | `getrandom(2)` syscall (no fd, no blocking)    |
 * | macOS / BSD     | `arc4random_buf()` (kernel CSPRNG)             |
 * | x86-64 (hw)     | RDRAND → falls back to kernel CSPRNG on fail   |
 *
 * ### RDRAND vs RDSEED
 * | Instruction | Source                    | Use case              |
 * |-------------|---------------------------|-----------------------|
 * | RDRAND      | HW CSPRNG (AES-CTR based) | Fast bulk entropy     |
 * | RDSEED      | TRNG (thermal noise)      | Seeding other PRNGs   |
 *
 * ### Zero-allocation design
 * All functions write into caller-provided buffers (`std::span<uint8_t>`)
 * or return fixed-size `std::array<uint8_t, N>` types.
 * No heap allocation occurs in any code path.
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <system_error>

#ifdef __linux__
#  include <cerrno>
#  include <sys/syscall.h>
#  include <unistd.h>
#else
#  include <cstdlib>  // arc4random_buf
#endif

#if defined(__x86_64__) || defined(__i386__)
#  if __has_include(<immintrin.h>)
#    include <immintrin.h>
#    define QBUEM_CRYPTO_HAS_RDRAND 1
#  endif
#endif

namespace qbuem::crypto {

// ─── Result type ──────────────────────────────────────────────────────────────

template <typename T>
using Result = std::expected<T, std::error_code>;

// ─── CPUID capability detection ───────────────────────────────────────────────

/**
 * @brief Returns true if the CPU supports the RDRAND instruction.
 *
 * Checks CPUID leaf 1, ECX bit 30. Result is cached after the first call.
 */
[[nodiscard]] inline bool has_rdrand() noexcept {
#if defined(QBUEM_CRYPTO_HAS_RDRAND)
    static const bool cached = []() noexcept -> bool {
        uint32_t eax = 1u, ebx = 0u, ecx = 0u, edx = 0u;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
        return ((ecx >> 30) & 1u) != 0u;
    }();
    return cached;
#else
    return false;
#endif
}

/**
 * @brief Returns true if the CPU supports the RDSEED instruction.
 *
 * Checks CPUID leaf 7, subleaf 0, EBX bit 18. Result is cached.
 */
[[nodiscard]] inline bool has_rdseed() noexcept {
#if defined(QBUEM_CRYPTO_HAS_RDRAND)
    static const bool cached = []() noexcept -> bool {
        uint32_t eax = 7u, ebx = 0u, ecx = 0u, edx = 0u;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
        return ((ebx >> 18) & 1u) != 0u;
    }();
    return cached;
#else
    return false;
#endif
}

// ─── Hardware entropy primitives ──────────────────────────────────────────────

/**
 * @brief Read one 64-bit value from the CPU hardware CSPRNG (RDRAND).
 *
 * Retries up to 10 times as per Intel's recommendation.
 *
 * @param[out] out  Receives the random value on success.
 * @returns true on success, false if RDRAND is unavailable or retries exhausted.
 */
[[nodiscard]] inline bool rdrand64(uint64_t& out) noexcept {
#if defined(QBUEM_CRYPTO_HAS_RDRAND) && defined(__RDRND__)
    static constexpr int kMaxRetries = 10;
    for (int i = 0; i < kMaxRetries; ++i) {
        unsigned long long val = 0;
        if (_rdrand64_step(&val) != 0) {
            out = static_cast<uint64_t>(val);
            return true;
        }
    }
    return false;
#elif defined(QBUEM_CRYPTO_HAS_RDRAND)
    uint64_t val = 0;
    uint8_t  cf  = 0;
    __asm__ volatile("rdrand %0\n\t setc %1" : "=r"(val), "=qm"(cf) : : "cc");
    if (cf != 0u) { out = val; return true; }
    return false;
#else
    (void)out;
    return false;
#endif
}

/**
 * @brief Read one 64-bit value from the CPU true random number generator (RDSEED).
 *
 * RDSEED is seeded by a thermal-noise TRNG and is suitable for seeding
 * other PRNGs. Retries up to 10 times.
 *
 * @param[out] out  Receives the true random value on success.
 * @returns true on success, false if RDSEED is unavailable or retries exhausted.
 */
[[nodiscard]] inline bool rdseed64(uint64_t& out) noexcept {
#if defined(QBUEM_CRYPTO_HAS_RDRAND) && defined(__RDSEED__)
    static constexpr int kMaxRetries = 10;
    for (int i = 0; i < kMaxRetries; ++i) {
        unsigned long long val = 0;
        if (_rdseed64_step(&val) != 0) {
            out = static_cast<uint64_t>(val);
            return true;
        }
    }
    return false;
#elif defined(QBUEM_CRYPTO_HAS_RDRAND)
    uint64_t val = 0;
    uint8_t  cf  = 0;
    __asm__ volatile("rdseed %0\n\t setc %1" : "=r"(val), "=qm"(cf) : : "cc");
    if (cf != 0u) { out = val; return true; }
    return false;
#else
    (void)out;
    return false;
#endif
}

// ─── Kernel CSPRNG ────────────────────────────────────────────────────────────

/**
 * @brief Fill @p buf with cryptographically secure random bytes.
 *
 * Uses the kernel's CSPRNG directly:
 * - Linux:    `getrandom(2)` (no file descriptor, no blocking after boot)
 * - macOS/BSD: `arc4random_buf()`
 *
 * @param buf  Destination buffer.
 * @returns `{}` on success, or an error_code if the kernel call fails.
 */
[[nodiscard]] inline Result<void> random_fill(std::span<uint8_t> buf) noexcept {
#ifdef __linux__
    size_t done = 0;
    uint8_t* ptr = buf.data();
    const size_t len = buf.size();
    while (done < len) {
        const ssize_t n = static_cast<ssize_t>(
            ::syscall(SYS_getrandom, ptr + done, len - done, 0));
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(
                std::error_code{errno, std::system_category()});
        }
        done += static_cast<size_t>(n);
    }
    return {};
#else
    ::arc4random_buf(buf.data(), buf.size());
    return {};
#endif
}

// ─── Hardware-accelerated fill ────────────────────────────────────────────────

/**
 * @brief Fill @p buf using RDRAND (hw CSPRNG), falling back to kernel CSPRNG.
 *
 * Fills in 8-byte chunks using RDRAND when available. Any remainder
 * or RDRAND failure is handled by `random_fill()`.
 *
 * @param buf  Destination buffer.
 * @returns `{}` on success, error on kernel CSPRNG failure.
 */
[[nodiscard]] inline Result<void> hw_random_fill(std::span<uint8_t> buf) noexcept {
    uint8_t* ptr  = buf.data();
    size_t   len  = buf.size();
    size_t   done = 0;

    if (has_rdrand()) {
        // Process 8 bytes at a time with RDRAND
        for (; done + 8 <= len; done += 8) {
            uint64_t val = 0;
            if (!rdrand64(val)) break;  // fall through to kernel
            std::memcpy(ptr + done, &val, 8);
        }
        // Handle tail (< 8 bytes remaining) or RDRAND failure
        if (done < len) {
            uint64_t val = 0;
            if (rdrand64(val)) {
                std::memcpy(ptr + done, &val, len - done);
                done = len;
            }
        }
    }

    if (done < len) {
        return random_fill({ptr + done, len - done});
    }
    return {};
}

/**
 * @brief Fill @p buf using RDSEED (thermal TRNG), falling back to RDRAND/kernel.
 *
 * Suitable for seeding other PRNGs. RDSEED is slower than RDRAND.
 *
 * @param buf  Destination buffer.
 * @returns `{}` on success, error on failure.
 */
[[nodiscard]] inline Result<void> hw_seed_fill(std::span<uint8_t> buf) noexcept {
    uint8_t* ptr  = buf.data();
    size_t   len  = buf.size();
    size_t   done = 0;

    if (has_rdseed()) {
        for (; done + 8 <= len; done += 8) {
            uint64_t val = 0;
            if (!rdseed64(val)) break;
            std::memcpy(ptr + done, &val, 8);
        }
        if (done < len) {
            uint64_t val = 0;
            if (rdseed64(val)) {
                std::memcpy(ptr + done, &val, len - done);
                done = len;
            }
        }
    }

    if (done < len) {
        return hw_random_fill({ptr + done, len - done});
    }
    return {};
}

// ─── Fixed-size convenience helpers ───────────────────────────────────────────

/**
 * @brief Generate N cryptographically secure random bytes.
 *
 * Returns a fixed-size array — zero heap allocation.
 *
 * @tparam N  Number of bytes to generate.
 * @returns `Result<std::array<uint8_t, N>>` with the random bytes.
 *
 * @code
 * auto key = qbuem::crypto::random_bytes<32>();  // 256-bit key
 * if (!key) handle_error(key.error());
 * @endcode
 */
template <size_t N>
[[nodiscard]] inline Result<std::array<uint8_t, N>> random_bytes() noexcept {
    std::array<uint8_t, N> buf{};
    if (auto r = random_fill(buf); !r) return std::unexpected(r.error());
    return buf;
}

/**
 * @brief Generate N random bytes using hardware RDRAND + kernel fallback.
 *
 * @tparam N  Number of bytes.
 */
template <size_t N>
[[nodiscard]] inline Result<std::array<uint8_t, N>> hw_random_bytes() noexcept {
    std::array<uint8_t, N> buf{};
    if (auto r = hw_random_fill(buf); !r) return std::unexpected(r.error());
    return buf;
}

}  // namespace qbuem::crypto
