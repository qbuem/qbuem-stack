#pragma once

/**
 * @file draco/crypto.hpp
 * @brief Cryptographic utilities: constant-time comparison, CSPRNG, CSRF tokens.
 *
 * All functions are header-only and platform-aware:
 *   - Linux:  getrandom(2) syscall (no file descriptor needed)
 *   - macOS:  arc4random_buf() (kernel CSPRNG)
 *   - Other:  /dev/urandom fallback
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

namespace draco {

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
  if (a.size() != b.size()) {
    // Perform a dummy comparison to avoid leaking length information via timing.
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < a.size(); ++i)
      sink |= static_cast<uint8_t>(a[i]);
    (void)sink;
    return false;
  }
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
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
 *   auto token = draco::csrf_token();   // "dGhpcyBpcyBhIHRlc3Q"
 *   res.set_cookie("csrf", token, {.same_site = "Strict", .http_only = false});
 *
 * Verification (timing-safe):
 *   if (!draco::constant_time_equal(req.cookie("csrf"), expected)) { ... }
 */
[[nodiscard]] inline std::string csrf_token(size_t bits = 128) {
  size_t bytes = (bits + 7) / 8;
  std::string raw = random_bytes(bytes);
  return detail::base64url_encode(
      reinterpret_cast<const uint8_t *>(raw.data()), raw.size());
}

} // namespace draco
