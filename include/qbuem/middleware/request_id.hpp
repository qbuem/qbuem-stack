#pragma once

#include <qbuem/crypto/random.hpp>
#include <qbuem/http/router.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <random>
#include <string>
#include <string_view>

namespace qbuem::middleware {

namespace detail {

/**
 * Generate a UUID v4 string (xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx).
 *
 * Uses the in-tree CSPRNG (`crypto::random_fill`, backed by getrandom/arc4random)
 * so request IDs are unpredictable — Mersenne Twister IDs are forgeable after
 * observing a few outputs, which is unsafe if IDs ever flow into a trust path
 * (idempotency keys, correlation tokens). On the (very rare) CSPRNG failure it
 * falls back to a thread_local PRNG so request handling never throws/blocks.
 */
inline std::string uuid_v4() {
  std::array<uint8_t, 16> b{};
  if (!qbuem::crypto::random_fill(b)) {
    thread_local std::mt19937_64 rng([] {
      std::random_device rd;
      return std::mt19937_64(rd());
    }());
    const uint64_t hi = rng();
    const uint64_t lo = rng();
    std::memcpy(b.data(), &hi, 8);
    std::memcpy(b.data() + 8, &lo, 8);
  }

  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // variant 10xx

  return std::format(
      "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
      "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

} // namespace detail

/**
 * Request ID middleware.
 *
 * For each incoming request:
 *   - If the request already carries the ID header (set by a reverse proxy),
 *     the existing value is echoed back in the response.
 *   - Otherwise a new UUID v4 is generated and set on the response.
 *
 * The ID is placed in the response header so callers can include it in
 * application-level logs (correlate gateway logs with app logs).
 *
 * @param header_name  Header name to read / write (default: "X-Request-ID").
 *
 * Example:
 *   app.use(qbuem::middleware::request_id());
 *   // or with a custom header:
 *   app.use(qbuem::middleware::request_id("X-Trace-ID"));
 */
inline Middleware request_id(std::string_view header_name = "X-Request-ID") {
  return [name = std::string(header_name)](const Request &req,
                                           Response &res) -> bool {
    auto incoming = req.header(name);
    if (!incoming.empty()) {
      res.header(name, incoming);
    } else {
      res.header(name, detail::uuid_v4());
    }
    return true;
  };
}

} // namespace qbuem::middleware
