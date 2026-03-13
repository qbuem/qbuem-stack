#pragma once

#include <qbuem/http/router.hpp>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>

namespace qbuem::middleware {

namespace detail {

/**
 * Generate a UUID v4 string (xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx).
 *
 * Uses a thread_local Mersenne Twister seeded from std::random_device —
 * zero lock contention, one allocation per call (the returned std::string).
 */
inline std::string uuid_v4() {
  thread_local std::mt19937_64 rng([] {
    std::random_device rd;
    return std::mt19937_64(rd());
  }());

  uint64_t hi = rng();
  uint64_t lo = rng();

  // Set version 4: bits 12–15 of time_hi = 0100
  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  // Set variant 10xx: bits 62–63 of clock_seq = 10
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%08x-%04x-%04x-%04x-%012llx",
                static_cast<uint32_t>(hi >> 32),
                static_cast<uint16_t>(hi >> 16),
                static_cast<uint16_t>(hi),
                static_cast<uint16_t>(lo >> 48),
                static_cast<unsigned long long>(lo & 0x0000FFFFFFFFFFFFULL));
  return buf;
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
