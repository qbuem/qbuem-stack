#pragma once

#include <qbuem/http/router.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace qbuem::middleware {

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
   * Maximum number of distinct keys retained in the per-thread bucket map.
   *
   * When the map exceeds this limit, the entry with the oldest last-seen
   * timestamp is evicted (LRU).  Set to 0 to disable the cap (unbounded).
   *
   * Default: 10,000 entries.
   */
  size_t max_keys = 10'000;

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

  /**
   * @brief Dynamic per-key configuration override callback.
   *
   * Used to apply different rate/burst values for specific keys (IP, etc.)
   * instead of the global settings.
   * Return value `std::nullopt` means use the global configuration.
   *
   * Example (specific IP whitelist):
   *   cfg.per_key_override = [](const std::string& key)
   *       -> std::optional<std::pair<double,double>> {
   *     if (key == "10.0.0.1") return std::make_pair(1000.0, 200.0);
   *     return std::nullopt;
   *   };
   */
  std::function<std::optional<std::pair<double, double>>(const std::string &key)>
      per_key_override;
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
 *   app.use(qbuem::middleware::rate_limit({.rate_per_sec = 50, .burst = 10}));
 */
inline Middleware rate_limit(RateLimitConfig cfg = {}) {
  if (!cfg.key_fn) {
    cfg.key_fn = [](const Request &req) -> std::string {
      // Hard cap on key length: a legitimate IPv4/IPv6 address is at most
      // 45 characters; 256 bytes stops a DoS via an oversized header that
      // would cause unbounded memory use in the per-thread bucket map.
      static constexpr size_t kMaxKeyLen = 256;

      // X-Forwarded-For may contain a comma-separated list; take the first IP.
      auto xff = req.header("X-Forwarded-For");
      if (!xff.empty()) {
        size_t comma = xff.find(',');
        auto s = std::string(comma == std::string_view::npos
                                 ? xff
                                 : xff.substr(0, comma));
        if (s.size() > kMaxKeyLen) s.resize(kMaxKeyLen);
        return s;
      }
      auto real_ip = req.header("X-Real-IP");
      if (!real_ip.empty()) {
        auto s = std::string(real_ip);
        if (s.size() > kMaxKeyLen) s.resize(kMaxKeyLen);
        return s;
      }
      return "__global__";
    };
  }

  double default_rate  = cfg.rate_per_sec;
  double default_burst = cfg.burst;
  size_t max_keys      = cfg.max_keys;

  return [default_rate, default_burst, max_keys,
          key_fn       = std::move(cfg.key_fn),
          override_fn  = std::move(cfg.per_key_override)](
             const Request &req, Response &res) -> bool {
    thread_local std::unordered_map<std::string, detail::Bucket> buckets;

    std::string key = key_fn(req);
    auto now = std::chrono::steady_clock::now();

    // LRU eviction: keep map bounded to prevent unbounded memory growth.
    if (max_keys > 0 && buckets.size() >= max_keys) {
      auto oldest = buckets.begin();
      for (auto it = std::next(oldest); it != buckets.end(); ++it) {
        if (it->second.last < oldest->second.last)
          oldest = it;
      }
      buckets.erase(oldest);
    }

    // Per-key dynamic override: allows whitelist / custom limits per IP.
    double rate  = default_rate;
    double burst = default_burst;
    if (override_fn) {
      if (auto ov = override_fn(key)) {
        rate  = ov->first;
        burst = ov->second;
      }
    }

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

    res.header("X-RateLimit-Limit",     std::to_string(limit_long));
    res.header("X-RateLimit-Remaining", std::to_string(remaining > 0 ? remaining - 1 : 0));

    if (b.tokens < 1.0) {
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

} // namespace qbuem::middleware
