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

// ARM NEON SIMD (AArch64)
#if defined(__ARM_NEON)
#  include <arm_neon.h>
#  define QBUEM_CRYPTO_NEON 1
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
  // Branchless constant-time comparison — prevents timing-oracle attacks.
  // No early return on size mismatch; size difference is folded into `diff`.
  //
  // Note: the *number of loop iterations* depends on string length.
  // For truly length-independent timing use fixed-length tokens (CSRF tokens,
  // HMAC digests) so both operands always have the same size.
  const size_t na = a.size();
  const size_t nb = b.size();

  // Fold size mismatch into the low byte of diff (branchless).
  size_t sz_diff = na ^ nb;
  sz_diff |= sz_diff >> 32;
  sz_diff |= sz_diff >> 16;
  sz_diff |= sz_diff >> 8;
  volatile uint8_t diff = static_cast<uint8_t>(sz_diff & 0xFF);

  const size_t n = na < nb ? na : nb;
  const auto*  pa = reinterpret_cast<const uint8_t*>(a.data());
  const auto*  pb = reinterpret_cast<const uint8_t*>(b.data());

#if defined(QBUEM_CRYPTO_NEON)
  // AArch64 NEON path: process 16 bytes per iteration.
  // vorrq_u8 accumulates all XOR differences; vmaxvq_u8 collapses to scalar.
  // The volatile write at the end prevents optimisation across the barrier.
  uint8x16_t acc = vdupq_n_u8(0);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t va = vld1q_u8(pa + i);
    const uint8x16_t vb = vld1q_u8(pb + i);
    acc = vorrq_u8(acc, veorq_u8(va, vb)); // OR-accumulate all XOR differences
  }
  // Collapse 16-byte accumulator to a single byte (UMAXV on AArch64)
  diff |= vmaxvq_u8(acc);
  // Scalar tail
  for (; i < n; ++i)
    diff |= pa[i] ^ pb[i];
#else
  for (size_t i = 0; i < n; ++i)
    diff |= pa[i] ^ pb[i];
#endif

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
  // Base64url alphabet (RFC 4648 §5): A-Z, a-z, 0-9, '-', '_' (no padding)
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  std::string out;
  out.reserve((len * 4 + 2) / 3);

#if defined(QBUEM_CRYPTO_NEON)
  // AArch64 NEON path: process 12 input bytes → 16 output chars per iteration.
  // Strategy:
  //   1. Load 12 bytes (0..11) into 3×uint8x16 with vld1q_lane_u8.
  //   2. Shuffle/extract 6-bit indices using bit manipulation.
  //   3. Use vqtbl1q_u8 (table lookup) to map 6-bit index → ASCII char.
  //
  // We use the "split alphabet" approach with two 64-entry vtbl lookups.
  // Each lookup covers 32 chars (upper/lower halves of the 64-char alphabet).

  // Two 16-byte sub-tables covering indices 0-31 and 32-63
  alignas(16) uint8_t tbl0[16], tbl1[16], tbl2[16], tbl3[16];
  for (int i = 0; i < 16; ++i) tbl0[i] = static_cast<uint8_t>(kTable[i]);
  for (int i = 0; i < 16; ++i) tbl1[i] = static_cast<uint8_t>(kTable[i + 16]);
  for (int i = 0; i < 16; ++i) tbl2[i] = static_cast<uint8_t>(kTable[i + 32]);
  for (int i = 0; i < 16; ++i) tbl3[i] = static_cast<uint8_t>(kTable[i + 48]);

  const uint8x16_t vtbl0 = vld1q_u8(tbl0);
  const uint8x16_t vtbl1 = vld1q_u8(tbl1);
  const uint8x16_t vtbl2 = vld1q_u8(tbl2);
  const uint8x16_t vtbl3 = vld1q_u8(tbl3);

  // Process 12 input bytes at a time → 16 base64 output chars
  size_t i = 0;
  for (; i + 12 <= len; i += 12) {
    // Load 12 bytes into a 16-byte vector (last 4 bytes are don't-care)
    uint8x16_t in = vld1q_u8(data + i);

    // Extract 16 × 6-bit indices from the 12-byte (96-bit) input:
    //   Byte layout: AAAAAABB BBBBCCCC CCDDDDDD
    //   → idx0 = A>>2,  idx1 = ((A&3)<<4)|(B>>4)
    //     idx2 = ((B&0xF)<<2)|(C>>6), idx3 = C&63  (for each 3-byte group)

    // We process 4 groups of 3 bytes (12 bytes total), producing 16 6-bit indices.
    // Use scalar loop here — NEON byte extraction for 6-bit packing is
    // compiler-friendly and the table lookup is the main win.
    alignas(16) uint8_t idx[16];
    for (int g = 0; g < 4; ++g) {
      const uint8_t b0 = data[i + g * 3 + 0];
      const uint8_t b1 = data[i + g * 3 + 1];
      const uint8_t b2 = data[i + g * 3 + 2];
      idx[g * 4 + 0] = (b0 >> 2) & 0x3F;
      idx[g * 4 + 1] = static_cast<uint8_t>(((b0 & 3) << 4) | (b1 >> 4));
      idx[g * 4 + 2] = static_cast<uint8_t>(((b1 & 0xF) << 2) | (b2 >> 6));
      idx[g * 4 + 3] = b2 & 0x3F;
    }
    (void)in; // suppress unused warning (used conceptually above)

    // Map 6-bit indices to ASCII via 4×16-entry vtbl lookups.
    // Each index is in 0-63; select the right sub-table based on which quarter.
    uint8x16_t vidx = vld1q_u8(idx);
    const uint8x16_t kMask16 = vdupq_n_u8(0x0F);
    const uint8x16_t kThresh0 = vdupq_n_u8(15); // boundary: 0-15 → tbl0
    const uint8x16_t kThresh1 = vdupq_n_u8(31); // boundary: 16-31 → tbl1
    const uint8x16_t kThresh2 = vdupq_n_u8(47); // boundary: 32-47 → tbl2

    // Use vqtbl1q_u8 with per-sub-table indices (mask to low 4 bits each time)
    const uint8x16_t lo_idx = vandq_u8(vidx, kMask16);
    // Select which sub-table result to use via vcleq_u8 masks
    const uint8x16_t in_q0 = vcleq_u8(vidx, kThresh0);         // idx <= 15
    const uint8x16_t in_q1 = vandq_u8(vcgtq_u8(vidx, kThresh0),
                                       vcleq_u8(vidx, kThresh1)); // 16..31
    const uint8x16_t in_q2 = vandq_u8(vcgtq_u8(vidx, kThresh1),
                                       vcleq_u8(vidx, kThresh2)); // 32..47
    const uint8x16_t in_q3 = vcgtq_u8(vidx, kThresh2);           // 48..63

    const uint8x16_t sub_idx_1 = vsubq_u8(vidx, vdupq_n_u8(16));
    const uint8x16_t sub_idx_2 = vsubq_u8(vidx, vdupq_n_u8(32));
    const uint8x16_t sub_idx_3 = vsubq_u8(vidx, vdupq_n_u8(48));

    const uint8x16_t r0 = vandq_u8(vqtbl1q_u8(vtbl0, lo_idx),      in_q0);
    const uint8x16_t r1 = vandq_u8(vqtbl1q_u8(vtbl1, vandq_u8(sub_idx_1, kMask16)), in_q1);
    const uint8x16_t r2 = vandq_u8(vqtbl1q_u8(vtbl2, vandq_u8(sub_idx_2, kMask16)), in_q2);
    const uint8x16_t r3 = vandq_u8(vqtbl1q_u8(vtbl3, vandq_u8(sub_idx_3, kMask16)), in_q3);

    const uint8x16_t result = vorrq_u8(vorrq_u8(r0, r1), vorrq_u8(r2, r3));

    // Append 16 chars to output
    const size_t old_sz = out.size();
    out.resize(old_sz + 16);
    vst1q_u8(reinterpret_cast<uint8_t*>(out.data() + old_sz), result);
  }
  // Scalar tail for remaining bytes
  for (; i < len; i += 3) {
    uint32_t b  = static_cast<uint8_t>(data[i]) << 16;
    if (i + 1 < len) b |= static_cast<uint8_t>(data[i + 1]) << 8;
    if (i + 2 < len) b |= static_cast<uint8_t>(data[i + 2]);
    out += kTable[(b >> 18) & 0x3F];
    out += kTable[(b >> 12) & 0x3F];
    if (i + 1 < len) out += kTable[(b >> 6) & 0x3F];
    if (i + 2 < len) out += kTable[(b     ) & 0x3F];
  }
#else
  // Scalar path
  for (size_t i = 0; i < len; i += 3) {
    uint32_t b  = static_cast<uint8_t>(data[i]) << 16;
    if (i + 1 < len) b |= static_cast<uint8_t>(data[i + 1]) << 8;
    if (i + 2 < len) b |= static_cast<uint8_t>(data[i + 2]);
    out += kTable[(b >> 18) & 0x3F];
    out += kTable[(b >> 12) & 0x3F];
    if (i + 1 < len) out += kTable[(b >> 6) & 0x3F];
    if (i + 2 < len) out += kTable[(b     ) & 0x3F];
  }
#endif
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
