#pragma once

#include <draco/http/router.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace draco::middleware {

namespace detail {

struct Bucket {
  double tokens;
  std::chrono::steady_clock::time_point last;
};

} // namespace detail

/** Configuration for rate_limit(). */
struct RateLimitConfig {
  /** Token refill rate (tokens added per second). */
  double rate_per_sec = 100.0;

  /**
   * Bucket capacity (maximum burst).
   *
   * A fresh connection starts with a full bucket, so the first `burst`
   * requests are allowed without any delay.
   */
  double burst = 20.0;

  /**
   * Function that returns the rate-limit key for a request.
   *
   * The default implementation uses X-Forwarded-For (first IP), then
   * X-Real-IP, then falls back to "__global__" (single shared bucket).
   */
  std::function<std::string(const Request &)> key_fn;
};

/**
 * Token-bucket rate limiter middleware.
 *
 * State is stored in a thread_local map (one bucket map per reactor thread),
 * so there is zero lock contention.  The effective global limit across N
 * threads is approximately rate_per_sec × N.
 *
 * When the limit is exceeded the middleware:
 *   - Responds with 429 Too Many Requests.
 *   - Sets X-RateLimit-Limit / X-RateLimit-Remaining / Retry-After headers.
 *   - Halts the middleware chain (returns false).
 *
 * Example:
 *   app.use(draco::middleware::rate_limit({.rate_per_sec = 50, .burst = 10}));
 */
inline Middleware rate_limit(RateLimitConfig cfg = {}) {
  if (!cfg.key_fn) {
    cfg.key_fn = [](const Request &req) -> std::string {
      // X-Forwarded-For may contain a comma-separated list; take the first IP.
      auto xff = req.header("X-Forwarded-For");
      if (!xff.empty()) {
        size_t comma = xff.find(',');
        return std::string(comma == std::string_view::npos
                               ? xff
                               : xff.substr(0, comma));
      }
      auto real_ip = req.header("X-Real-IP");
      if (!real_ip.empty()) return std::string(real_ip);
      return "__global__";
    };
  }

  double rate  = cfg.rate_per_sec;
  double burst = cfg.burst;

  return [rate, burst, key_fn = std::move(cfg.key_fn)](
             const Request &req, Response &res) -> bool {
    thread_local std::unordered_map<std::string, detail::Bucket> buckets;

    std::string key = key_fn(req);
    auto now = std::chrono::steady_clock::now();

    auto [it, inserted] = buckets.emplace(key, detail::Bucket{burst, now});
    detail::Bucket &b   = it->second;

    if (!inserted) {
      // Refill tokens proportional to elapsed time.
      double elapsed = std::chrono::duration<double>(now - b.last).count();
      b.last         = now;
      b.tokens       = std::min(burst, b.tokens + elapsed * rate);
    }

    long limit_long = static_cast<long>(burst);
    long remaining  = static_cast<long>(b.tokens);

    res.header("X-RateLimit-Limit",
               std::to_string(limit_long));
    res.header("X-RateLimit-Remaining",
               std::to_string(remaining > 0 ? remaining - 1 : 0));

    if (b.tokens < 1.0) {
      // Compute seconds until one token is available.
      double retry_secs = (1.0 - b.tokens) / rate;
      res.status(429)
         .header("Retry-After",
                 std::to_string(static_cast<int>(std::ceil(retry_secs))))
         .body("Too Many Requests");
      return false;
    }

    b.tokens -= 1.0;
    return true;
  };
}

} // namespace draco::middleware
