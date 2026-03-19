#pragma once

/**
 * @file qbuem/middleware/adaptive_rate_limiter.hpp
 * @brief Adaptive token-bucket rate limiter that adjusts limits based on system load.
 * @defgroup qbuem_middleware_arlimit AdaptiveRateLimiter
 * @ingroup qbuem_middleware
 *
 * ## Overview
 * `AdaptiveRateLimiter` extends the basic token-bucket algorithm with
 * **load-proportional scaling**.  When system load (measured by CPU idle time
 * via `/proc/stat` on Linux) rises above a configurable threshold, the
 * effective rate and burst limits are scaled down automatically.  As load
 * drops, limits recover toward the configured maximums.
 *
 * ## Key Properties
 * - **Lock-free hot path**: each reactor thread maintains its own per-thread
 *   bucket map — zero cross-thread synchronisation on the request path.
 * - **Atomic load snapshot**: a single `std::atomic<float>` carries the
 *   current load factor, updated by a background sampler thread at configurable
 *   intervals.  Reads on the hot path are `memory_order_relaxed`.
 * - **No heap allocation per request**: the thread-local bucket map is bounded
 *   by `max_keys`; eviction removes the least-recently-used entry in place.
 * - **Graceful degradation**: at 100% load the effective rate falls to
 *   `min_rate_factor × rate_per_sec`, never to zero.
 *
 * ## Integration
 * ```cpp
 * auto limiter = qbuem::middleware::adaptive_rate_limit({
 *     .rate_per_sec   = 1000.0,   // max allowed requests/s per IP
 *     .burst          = 50.0,
 *     .high_load_pct  = 70.0,     // start throttling above 70 % CPU load
 *     .min_rate_factor = 0.2,     // floor: 20 % of max rate under full load
 * });
 * app.use(limiter);
 * ```
 *
 * ## Thread Safety
 * - Middleware closure: safe to call concurrently from N reactor threads.
 * - Background sampler: started once in `adaptive_rate_limit()`; joined when
 *   the returned `Middleware` is destroyed (via `shared_ptr` refcount).
 */

#include <qbuem/http/router.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>       // fopen, fscanf
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace qbuem::middleware {

// ── Configuration ─────────────────────────────────────────────────────────────

/**
 * @brief Configuration for `adaptive_rate_limit()`.
 */
struct AdaptiveRateLimitConfig {
    /** Maximum token refill rate (requests / second) at zero load. */
    double rate_per_sec    = 200.0;

    /** Maximum burst (bucket capacity) at zero load. */
    double burst           = 40.0;

    /**
     * CPU load percentage threshold above which throttling begins.
     *
     * Load is measured as 100 × (1 - idle / total) averaged over all cores.
     * At `high_load_pct` the effective rate is still `rate_per_sec`.
     * Above it, rate and burst scale linearly toward `min_rate_factor`.
     */
    double high_load_pct   = 70.0;

    /**
     * Minimum rate multiplier applied at 100 % CPU load.
     *
     * Effective rate = rate_per_sec × lerp(1.0, min_rate_factor, excess_load).
     * Must be in (0.0, 1.0].  Default: 0.2 (20 % of max at full load).
     */
    double min_rate_factor = 0.2;

    /** Maximum per-thread bucket entries retained before LRU eviction. */
    size_t max_keys        = 10'000;

    /** How often the background thread re-samples CPU load. */
    std::chrono::milliseconds sample_interval{500};

    /**
     * Function that extracts the rate-limit key from a request.
     *
     * Default: first IP from X-Forwarded-For, then X-Real-IP, then
     * the string `"__global__"` (single shared bucket).
     */
    std::function<std::string(const Request&)> key_fn;

    /**
     * Per-key override: return a custom (rate, burst) pair or `nullopt`
     * to use the global (load-adjusted) values.
     */
    std::function<std::optional<std::pair<double, double>>(const std::string&)>
        per_key_override;
};

// ── Implementation detail ─────────────────────────────────────────────────────

namespace detail {

/**
 * @brief Token bucket state — stored per key in a thread-local map.
 */
struct AdaptiveBucket {
    double tokens;
    std::chrono::steady_clock::time_point last;
};

/**
 * @brief Shared load-sampler owned by the middleware closure.
 *
 * A single background `std::jthread` reads `/proc/stat` (Linux) or returns
 * a stub 0 % on other POSIX platforms, and stores the result in the atomic
 * `load_factor` (0.0 = idle, 1.0 = 100 % busy).
 */
class LoadSampler {
public:
    explicit LoadSampler(std::chrono::milliseconds interval,
                         double high_load_pct,
                         double min_rate_factor) noexcept
        : high_load_pct_(high_load_pct)
        , min_rate_factor_(min_rate_factor)
        , load_factor_(1.0f)   // start at full capacity (no throttle)
        , sampler_(
            [this, interval](std::stop_token st) {
                while (!st.stop_requested()) {
                    std::this_thread::sleep_for(interval);
                    const double cpu = sample_cpu_pct();
                    load_factor_.store(compute_factor(cpu),
                                       std::memory_order_relaxed);
                }
            })
    {}

    ~LoadSampler() = default; // jthread joins on destruction

    /**
     * @brief Current rate multiplier in [min_rate_factor, 1.0].
     *
     * Hot path — `memory_order_relaxed` is sufficient; we tolerate
     * one stale sample per `sample_interval`.
     */
    [[nodiscard]] float load_factor() const noexcept {
        return load_factor_.load(std::memory_order_relaxed);
    }

private:
    /** @brief Sample overall CPU utilisation (0..100 %). */
    [[nodiscard]] double sample_cpu_pct() noexcept {
#if defined(__linux__)
        // Read /proc/stat first line: "cpu  user nice system idle iowait irq softirq"
        std::FILE* f = std::fopen("/proc/stat", "r");
        if (!f) return 0.0;

        unsigned long long user{}, nice_{}, sys{}, idle{}, iowait{},
                           irq{}, softirq{}, steal{};
        int matched = std::fscanf(f, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
                                  &user, &nice_, &sys, &idle,
                                  &iowait, &irq, &softirq, &steal);
        std::fclose(f);
        if (matched < 4) return 0.0;

        const unsigned long long total =
            user + nice_ + sys + idle + iowait + irq + softirq + steal;
        const unsigned long long dt_total = total - prev_total_;
        const unsigned long long dt_idle  = idle  - prev_idle_;
        prev_total_ = total;
        prev_idle_  = idle;

        if (dt_total == 0) return 0.0;
        return 100.0 * (1.0 - static_cast<double>(dt_idle) /
                               static_cast<double>(dt_total));
#else
        // Non-Linux POSIX: no /proc/stat; report 0 % (no throttling).
        return 0.0;
#endif
    }

    /** @brief Map cpu% → rate multiplier in [min_rate_factor_, 1.0]. */
    [[nodiscard]] double compute_factor(double cpu_pct) const noexcept {
        if (cpu_pct <= high_load_pct_) return 1.0;  // below threshold → full rate
        const double excess = (cpu_pct - high_load_pct_) / (100.0 - high_load_pct_);
        // Linear interpolation: 1.0 at threshold, min_rate_factor_ at 100 %
        return 1.0 - excess * (1.0 - min_rate_factor_);
    }

    double               high_load_pct_;
    double               min_rate_factor_;
    std::atomic<float>   load_factor_;
    // prev samples for differential CPU calculation
    unsigned long long   prev_total_{0};
    unsigned long long   prev_idle_{0};
    std::jthread         sampler_;
};

} // namespace detail

// ── Public factory function ───────────────────────────────────────────────────

/**
 * @brief Build an adaptive rate-limiter middleware.
 *
 * Creates and starts a background load-sampler thread that periodically
 * measures CPU utilisation.  The returned `Middleware` closure is safe to
 * call from any number of reactor threads concurrently.
 *
 * When the middleware rejects a request it:
 *   - Responds with **429 Too Many Requests**.
 *   - Sets `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `Retry-After`.
 *   - Adds `X-Load-Factor` (current effective multiplier, for diagnostics).
 *   - Returns `false` to halt the middleware chain.
 *
 * @param cfg  Configuration (all fields have sane defaults).
 * @returns    Middleware closure suitable for `app.use(...)`.
 */
inline Middleware adaptive_rate_limit(AdaptiveRateLimitConfig cfg = {}) {
    // Supply default key extractor.
    if (!cfg.key_fn) {
        cfg.key_fn = [](const Request& req) -> std::string {
            static constexpr size_t kMaxKeyLen = 256;
            auto xff = req.header("X-Forwarded-For");
            if (!xff.empty()) {
                size_t comma = xff.find(',');
                auto s = std::string(comma == std::string_view::npos
                                        ? xff : xff.substr(0, comma));
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

    // Sampler is shared across all reactor threads via shared_ptr.
    auto sampler = std::make_shared<detail::LoadSampler>(
        cfg.sample_interval, cfg.high_load_pct, cfg.min_rate_factor);

    const double base_rate  = cfg.rate_per_sec;
    const double base_burst = cfg.burst;
    const size_t max_keys   = cfg.max_keys;

    return [sampler,
            base_rate, base_burst, max_keys,
            key_fn      = std::move(cfg.key_fn),
            override_fn = std::move(cfg.per_key_override)](
                const Request& req, Response& res) -> bool
    {
        thread_local std::unordered_map<std::string, detail::AdaptiveBucket> buckets;

        const std::string key = key_fn(req);
        const auto now        = std::chrono::steady_clock::now();

        // Load-adjusted rate and burst.
        const float  factor     = sampler->load_factor();
        double       eff_rate   = base_rate  * factor;
        double       eff_burst  = base_burst * factor;

        // Per-key override (e.g., whitelisted IPs, premium accounts).
        if (override_fn) {
            if (auto ov = override_fn(key)) {
                eff_rate  = ov->first;
                eff_burst = ov->second;
            }
        }

        // LRU eviction: keep bucket map bounded.
        if (max_keys > 0 && buckets.size() >= max_keys) {
            auto oldest = buckets.begin();
            for (auto it = std::next(oldest); it != buckets.end(); ++it) {
                if (it->second.last < oldest->second.last)
                    oldest = it;
            }
            buckets.erase(oldest);
        }

        // Insert or refill the bucket.
        auto [it, inserted] = buckets.emplace(
            key, detail::AdaptiveBucket{eff_burst, now});
        auto& b = it->second;

        if (!inserted) {
            const double elapsed = std::chrono::duration<double>(now - b.last).count();
            b.last   = now;
            b.tokens = std::min(eff_burst, b.tokens + elapsed * eff_rate);
        }

        // Diagnostic headers.
        char factor_buf[16];
        std::snprintf(factor_buf, sizeof(factor_buf), "%.2f", static_cast<double>(factor));
        res.header("X-Load-Factor", factor_buf);
        res.header("X-RateLimit-Limit",
                   std::to_string(static_cast<long>(eff_burst)));
        res.header("X-RateLimit-Remaining",
                   std::to_string(static_cast<long>(b.tokens > 0.0 ? b.tokens - 1.0 : 0.0)));

        if (b.tokens < 1.0) {
            const double retry_secs = (1.0 - b.tokens) / eff_rate;
            res.status(429)
               .header("Retry-After",
                       std::to_string(static_cast<int>(std::ceil(retry_secs))))
               .body("Too Many Requests (adaptive)");
            return false;
        }

        b.tokens -= 1.0;
        return true;
    };
}

} // namespace qbuem::middleware
