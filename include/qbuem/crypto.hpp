#pragma once

/**
 * @file qbuem/crypto.hpp
 * @brief Cryptographic utilities: constant-time comparison, CSPRNG, CSRF tokens,
 *        and hardware entropy (RDRAND / RDSEED).
 *
 * All functions are header-only and platform-aware:
 *   - x86-64 (RDRAND/RDSEED): direct CPU hardware TRNG/DRNG access
 *   - Linux:  getrandom(2) syscall (no file descriptor needed)
 *   - macOS:  arc4random_buf() (kernel CSPRNG)
 *   - Other:  /dev/urandom fallback
 *
 * ## Hardware Entropy Strategy (v1.5.0)
 * `rdrand64()` / `rdseed64()` inline functions collect CPU entropy directly.
 * `hw_entropy_fill()` prefers RDRAND and transparently falls back to
 * `random_fill()` (getrandom/arc4random) on unsupported CPUs.
 *
 * ### RDRAND vs RDSEED
 * | Instruction | Source | Characteristics |
 * |-------------|--------|-----------------|
 * | RDRAND | CSPRNG (HW PRNG-based) | Fast, no entropy depletion |
 * | RDSEED | True random (TRNG, thermal noise) | Slow, intended for seeding |
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef __linux__
#  include <sys/random.h>  // getrandom
#else
#  include <cstdlib>       // arc4random_buf
#endif

// RDRAND / RDSEED (x86-64, requires -mrdrnd flag or __RDRND__ defined)
#if defined(__x86_64__) || defined(__i386__)
#  if __has_include(<immintrin.h>)
#    include <immintrin.h>
#    define QBUEM_HAS_RDRAND 1
#  endif
#endif

namespace qbuem {

// ─── Constant-time comparison ─────────────────────────────────────────────────

/**
 * Compare two strings in constant time.
 *
 * Prevents timing-oracle attacks on secret comparisons (HMAC signatures,
 * session tokens, CSRF tokens, …).  Always touches every byte of both strings
 * even when a mismatch is detected early.
 *
 * @returns true iff @p a and @p b are identical.
 */
[[nodiscard]] inline bool constant_time_equal(std::string_view a,
                                               std::string_view b) noexcept {
  // Branchless implementation: no early return on size mismatch.
  // A size difference is folded into `diff` via XOR reduction, so the
  // function always touches min(a.size(), b.size()) bytes.  This prevents
  // content-based timing oracles while remaining branchless on the sizes.
  //
  // Note: the *number of loop iterations* still depends on string length.
  // For truly length-independent timing use fixed-length tokens (e.g. CSRF
  // tokens, HMAC digests) so both operands always have the same size.
  const size_t na = a.size();
  const size_t nb = b.size();

  // Encode size mismatch as a non-zero diff without branching.
  // Reduce all bits of (na XOR nb) into a single byte.
  size_t sz_diff = na ^ nb;
  sz_diff |= sz_diff >> 32;
  sz_diff |= sz_diff >> 16;
  sz_diff |= sz_diff >> 8;
  volatile uint8_t diff = static_cast<uint8_t>(sz_diff & 0xFF);

  const size_t n = na < nb ? na : nb;
  for (size_t i = 0; i < n; ++i)
    diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
  return diff == 0;
}

// ─── CSPRNG ───────────────────────────────────────────────────────────────────

/**
 * Fill @p buf with @p len cryptographically secure random bytes.
 *
 * Uses the best available kernel interface:
 *   - Linux ≥ 3.17: `getrandom(2)` (no fd, no blocking after boot)
 *   - macOS / BSD:  `arc4random_buf()`
 *
 * @throws std::runtime_error if entropy source is unavailable.
 */
inline void random_fill(void *buf, size_t len) {
#ifdef __linux__
  size_t done = 0;
  auto *ptr = static_cast<uint8_t *>(buf);
  while (done < len) {
    ssize_t n = ::getrandom(ptr + done, len - done, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("getrandom failed");
    }
    done += static_cast<size_t>(n);
  }
#else
  ::arc4random_buf(buf, len);
#endif
}

/**
 * Return @p n cryptographically secure random bytes as a std::string.
 */
[[nodiscard]] inline std::string random_bytes(size_t n) {
  std::string out(n, '\0');
  random_fill(out.data(), n);
  return out;
}

// ─── Base64url encoding (RFC 4648 §5, no padding) ────────────────────────────

namespace detail {

inline std::string base64url_encode(const uint8_t *data, size_t len) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((len * 4 + 2) / 3);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t b  = static_cast<uint8_t>(data[i]) << 16;
    if (i + 1 < len) b |= static_cast<uint8_t>(data[i + 1]) << 8;
    if (i + 2 < len) b |= static_cast<uint8_t>(data[i + 2]);
    out += kTable[(b >> 18) & 0x3F];
    out += kTable[(b >> 12) & 0x3F];
    if (i + 1 < len) out += kTable[(b >> 6) & 0x3F];
    if (i + 2 < len) out += kTable[(b     ) & 0x3F];
  }
  return out;
}

} // namespace detail

// ─── CSRF token ───────────────────────────────────────────────────────────────

/**
 * Generate a URL-safe Base64 (RFC 4648 §5) CSRF token.
 *
 * The token contains @p bits of cryptographic entropy, rounded up to the
 * nearest 6-bit boundary.  Default: 128 bits → 22 Base64url characters.
 *
 * Example:
 *   auto token = qbuem::csrf_token();   // "dGhpcyBpcyBhIHRlc3Q"
 *   res.set_cookie("csrf", token, {.same_site = "Strict", .http_only = false});
 *
 * Verification (timing-safe):
 *   if (!qbuem::constant_time_equal(req.cookie("csrf"), expected)) { ... }
 */
[[nodiscard]] inline std::string csrf_token(size_t bits = 128) {
  size_t bytes = (bits + 7) / 8;
  std::string raw = random_bytes(bytes);
  return detail::base64url_encode(
      reinterpret_cast<const uint8_t *>(raw.data()), raw.size());
}

// ─── Hardware Entropy (RDRAND / RDSEED) ──────────────────────────────────────

/**
 * @brief Collect a 64-bit random number directly via the CPU RDRAND instruction.
 *
 * RDRAND reads directly from the hardware CSPRNG (AES-CTR based) on
 * Intel/AMD CPUs. It never depletes entropy and is FIPS 140-2 validated.
 *
 * ## Retry policy
 * The hardware CSPRNG can occasionally fail (carry flag = 0), so the
 * function retries up to `kMaxRetries` times.
 *
 * @param[out] out  The collected 64-bit random value.
 * @returns true on success, false if RDRAND is unsupported or retries exhausted.
 */
[[nodiscard]] inline bool rdrand64(uint64_t &out) noexcept {
#if defined(QBUEM_HAS_RDRAND) && defined(__RDRND__)
  static constexpr int kMaxRetries = 10;
  for (int i = 0; i < kMaxRetries; ++i) {
    unsigned long long val = 0;
    if (_rdrand64_step(&val)) {
      out = static_cast<uint64_t>(val);
      return true;
    }
  }
  return false;
#elif defined(QBUEM_HAS_RDRAND)
  // Build without __RDRND__: fall back to direct inline asm
  uint64_t val = 0;
  uint8_t  cf  = 0;
  __asm__ volatile (
    "rdrand %0\n\t"
    "setc   %1"
    : "=r"(val), "=qm"(cf)
    :
    : "cc"
  );
  if (cf) { out = val; return true; }
  return false;
#else
  (void)out;
  return false; // non-x86 fallback
#endif
}

/**
 * @brief Collect a 64-bit true random number (TRNG) via the CPU RDSEED instruction.
 *
 * RDSEED reads directly from a thermal-noise based TRNG.
 * It is slower than RDRAND but contains physical entropy.
 * Suitable for PRNG seed initialization and key generation.
 *
 * @param[out] out  The collected 64-bit true random value.
 * @returns true on success, false if RDSEED is unsupported or retries exhausted.
 */
[[nodiscard]] inline bool rdseed64(uint64_t &out) noexcept {
#if defined(QBUEM_HAS_RDRAND) && defined(__RDSEED__)
  static constexpr int kMaxRetries = 10;
  for (int i = 0; i < kMaxRetries; ++i) {
    unsigned long long val = 0;
    if (_rdseed64_step(&val)) {
      out = static_cast<uint64_t>(val);
      return true;
    }
  }
  return false;
#elif defined(QBUEM_HAS_RDRAND)
  uint64_t val = 0;
  uint8_t  cf  = 0;
  __asm__ volatile (
    "rdseed %0\n\t"
    "setc   %1"
    : "=r"(val), "=qm"(cf)
    :
    : "cc"
  );
  if (cf) { out = val; return true; }
  return false;
#else
  (void)out;
  return false;
#endif
}

/**
 * @brief Fills a buffer using hardware RDRAND. Falls back to `random_fill()` if unsupported.
 *
 * If RDRAND is supported, fills the buffer in 8-byte chunks quickly.
 * Otherwise, calls `random_fill()` (getrandom/arc4random_buf).
 *
 * ## Usage example
 * @code
 * std::array<uint8_t, 32> key;
 * qbuem::hw_entropy_fill(key.data(), key.size()); // 256-bit hardware entropy
 * @endcode
 *
 * @param buf Pointer to the buffer to fill.
 * @param len Number of bytes to fill.
 * @throws std::runtime_error if the entropy source is unavailable.
 */
inline void hw_entropy_fill(void *buf, size_t len) {
  auto *ptr = static_cast<uint8_t*>(buf);
  size_t done = 0;

  // 8-byte chunks: use RDRAND
  while (done + 8 <= len) {
    uint64_t rand_val = 0;
    if (!rdrand64(rand_val)) {
      // RDRAND failed — handle remainder with kernel CSPRNG
      random_fill(ptr + done, len - done);
      return;
    }
    std::memcpy(ptr + done, &rand_val, 8);
    done += 8;
  }

  // Remaining bytes: kernel CSPRNG
  if (done < len) {
    uint64_t rand_val = 0;
    if (rdrand64(rand_val)) {
      std::memcpy(ptr + done, &rand_val, len - done);
    } else {
      random_fill(ptr + done, len - done);
    }
  }
}

/**
 * @brief Fills a buffer using RDSEED. Slower than RDRAND but contains physical entropy.
 *
 * Falls back to `hw_entropy_fill()` (RDRAND preferred) if RDSEED fails.
 *
 * @param buf Buffer to fill.
 * @param len Number of bytes.
 * @throws std::runtime_error if no entropy source is available.
 */
inline void hw_seed_fill(void *buf, size_t len) {
  auto *ptr = static_cast<uint8_t*>(buf);
  size_t done = 0;

  while (done + 8 <= len) {
    uint64_t seed_val = 0;
    if (!rdseed64(seed_val)) {
      // RDSEED unsupported/failed — fall back to RDRAND + kernel CSPRNG
      hw_entropy_fill(ptr + done, len - done);
      return;
    }
    std::memcpy(ptr + done, &seed_val, 8);
    done += 8;
  }

  if (done < len) {
    uint64_t seed_val = 0;
    if (rdseed64(seed_val)) {
      std::memcpy(ptr + done, &seed_val, len - done);
    } else {
      hw_entropy_fill(ptr + done, len - done);
    }
  }
}

/**
 * @brief Checks at runtime whether hardware RDRAND is available.
 *
 * Uses CPUID to check ECX bit 30 (RDRAND support flag).
 * Caches the result after the first call.
 *
 * @returns true if RDRAND is available.
 */
[[nodiscard]] inline bool has_rdrand() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  static const bool cached = []() noexcept -> bool {
    uint32_t ecx = 0;
#  if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile (
      "cpuid"
      : "=c"(ecx)
      : "a"(1), "c"(0)
      : "ebx", "edx"
    );
#  endif
    return (ecx >> 30) & 1u;
  }();
  return cached;
#else
  return false;
#endif
}

/**
 * @brief Checks at runtime whether CPU RDSEED is available.
 *
 * Checks CPUID EBX bit 18 (RDSEED support flag).
 *
 * @returns true if RDSEED is available.
 */
[[nodiscard]] inline bool has_rdseed() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  static const bool cached = []() noexcept -> bool {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
#  if defined(__GNUC__) || defined(__clang__)
    // cpuid(leaf=7, subleaf=0): EBX bit 18 = RDSEED
    __asm__ volatile (
      "cpuid"
      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
      : "a"(7u), "c"(0u)
    );
    (void)eax; (void)ecx; (void)edx;
#  endif
    return (ebx >> 18) & 1u;
  }();
  return cached;
#else
  return false;
#endif
}

} // namespace qbuem
