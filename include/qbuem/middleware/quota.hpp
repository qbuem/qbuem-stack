#pragma once

/**
 * @file qbuem/middleware/quota.hpp
 * @brief Per-tenant request **quota** middleware — a fixed-window request budget
 *        (e.g. "10,000 requests/day per tenant"), distinct from the token-bucket
 *        `rate_limit` (sustained rate + burst).
 * @ingroup qbuem_middleware
 *
 * SaaS multi-tenancy primitive: key by the authenticated tenant (the `X-Auth-Sub`
 * claim injected by `bearer_auth`, or `X-Tenant-Id`), apply a per-window budget,
 * and allow per-tenant (plan-tier) overrides. Place this AFTER the auth
 * middleware so the tenant identity is available.
 *
 * State lives in a thread_local map (one per reactor thread) — lock-free, on the
 * hot path (Pillar 1). As with `rate_limit`, the effective budget is therefore
 * `limit × N reactor threads`. For an EXACT process- or cluster-wide quota, plug
 * a shared counter store (Redis/DB) — that is a future opt-in adapter behind the
 * BYO-DB port (see docs/saas-readiness.md); the in-process default needs no
 * dependency.
 *
 * On exceed: 429 Too Many Requests + `X-Quota-Limit` / `X-Quota-Remaining` /
 * `Retry-After` (seconds until the window resets), and the chain halts.
 *
 * @code
 * app.use(qbuem::middleware::quota({.limit = 10000, .window = std::chrono::hours{24}}));
 * @endcode
 */

#include <qbuem/http/router.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qbuem::middleware {

namespace detail {

struct QuotaWindow {
  long                                  count; ///< Requests counted in the current window.
  std::chrono::steady_clock::time_point start; ///< When the current window began.
};

} // namespace detail

/** Configuration for quota(). */
struct QuotaConfig {
  /** Maximum requests allowed per window per key. */
  long limit = 1000;

  /** Window length. The window is rolling: it begins at a key's first request
   *  and resets `window` later. Default: one hour. */
  std::chrono::seconds window = std::chrono::seconds{3600};

  /** Max distinct keys retained in the per-thread map (LRU eviction). 0 = no cap. */
  size_t max_keys = 10'000;

  /**
   * Returns the quota key for a request. Default: the authenticated tenant —
   * `X-Auth-Sub` (set by bearer_auth), then `X-Tenant-Id`, then the client IP
   * (X-Forwarded-For / X-Real-IP), then "__global__".
   */
  std::function<std::string(const Request &)> key_fn;

  /**
   * Per-key limit override (e.g. plan tiers). Return std::nullopt to use `limit`.
   *
   *   cfg.per_key_override = [](const std::string& tenant) -> std::optional<long> {
   *     if (is_premium(tenant)) return 1'000'000;
   *     return std::nullopt;
   *   };
   */
  std::function<std::optional<long>(const std::string &key)> per_key_override;
};

/**
 * @brief Fixed-window per-tenant quota middleware.
 *
 * Returns true to continue the chain; on quota exhaustion sets a 429 response and
 * returns false.
 */
inline Middleware quota(QuotaConfig cfg = {}) {
  if (!cfg.key_fn) {
    cfg.key_fn = [](const Request &req) -> std::string {
      static constexpr size_t kMaxKeyLen = 256;
      auto clamp = [](std::string_view v) {
        std::string s(v);
        if (s.size() > kMaxKeyLen) s.resize(kMaxKeyLen);
        return s;
      };
      auto sub = req.header("X-Auth-Sub"); // tenant identity from bearer_auth
      if (!sub.empty()) return clamp(sub);
      auto tenant = req.header("X-Tenant-Id");
      if (!tenant.empty()) return clamp(tenant);
      auto xff = req.header("X-Forwarded-For");
      if (!xff.empty()) {
        size_t comma = xff.find(',');
        return clamp(comma == std::string_view::npos ? xff : xff.substr(0, comma));
      }
      auto real_ip = req.header("X-Real-IP");
      if (!real_ip.empty()) return clamp(real_ip);
      return "__global__";
    };
  }

  const long             default_limit = cfg.limit;
  const std::chrono::seconds window     = cfg.window;
  const size_t           max_keys      = cfg.max_keys;

  return [default_limit, window, max_keys,
          key_fn      = std::move(cfg.key_fn),
          override_fn = std::move(cfg.per_key_override)](
             const Request &req, Response &res) -> bool {
    thread_local std::unordered_map<std::string, detail::QuotaWindow> windows;

    const std::string key = key_fn(req);
    const auto now = std::chrono::steady_clock::now();

    // O(1) bounded eviction. A per-request O(n) "find oldest" scan would itself
    // become a latency cliff / DoS vector once the map fills (exactly what this
    // middleware is meant to prevent), so we drop one arbitrary entry
    // (hash-order begin()) instead — a memory bound, not strict LRU.
    if (max_keys > 0 && windows.size() >= max_keys) {
      windows.erase(windows.begin());
    }

    long limit = default_limit;
    if (override_fn) {
      if (auto ov = override_fn(key)) limit = *ov;
    }

    auto [it, inserted] = windows.emplace(key, detail::QuotaWindow{0, now});
    detail::QuotaWindow &w = it->second;
    if (!inserted && (now - w.start) >= window) {
      // Window elapsed → reset.
      w.count = 0;
      w.start = now;
    }

    res.header("X-Quota-Limit", std::to_string(limit));

    if (w.count >= limit) {
      const auto reset_in =
          std::chrono::duration_cast<std::chrono::seconds>((w.start + window) - now)
              .count();
      const long retry = reset_in > 0 ? static_cast<long>(reset_in) : 1;
      res.header("X-Quota-Remaining", "0")
         .header("Retry-After", std::to_string(retry))
         .status(429)
         .body("Quota Exceeded");
      return false;
    }

    ++w.count;
    res.header("X-Quota-Remaining", std::to_string(limit - w.count));
    return true;
  };
}

} // namespace qbuem::middleware
