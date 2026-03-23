#pragma once

/**
 * @file qbuem/http/backoff.hpp
 * @brief Async-aware backoff strategies for HTTP retry logic.
 * @defgroup qbuem_http_backoff HTTP Backoff
 * @ingroup qbuem_http
 *
 * Provides composable backoff strategy functions and an async sleep primitive
 * that integrates with the Reactor event loop (no thread blocking).
 *
 * ## Usage
 * @code
 * using namespace qbuem::backoff;
 *
 * RetryConfig cfg{
 *     .max_attempts = 4,
 *     .policy       = jitter(200ms, 30s),
 * };
 *
 * // In a coroutine:
 * co_await async_sleep(cfg.policy(attempt));
 * @endcode
 *
 * @{
 */

#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <functional>
#include <limits>
#include <random>

namespace qbuem::backoff {

using namespace std::chrono_literals;

/**
 * @brief Backoff policy function: given attempt index (0-based), returns delay.
 *
 * Attempt 0 is the delay before the first retry (i.e., after the first failure).
 */
using BackoffFn = std::function<std::chrono::milliseconds(int attempt)>;

// ─── Backoff factories ────────────────────────────────────────────────────────

/**
 * @brief Fixed-delay backoff — every retry waits the same amount.
 * @param delay  Constant delay applied to every retry attempt.
 */
[[nodiscard]] inline BackoffFn fixed(std::chrono::milliseconds delay) {
    return [delay](int) { return delay; };
}

/**
 * @brief Linear backoff — delay grows as base * (attempt + 1), capped at cap.
 * @param base  Delay increment per attempt.
 * @param cap   Maximum delay (default: 30 s).
 */
[[nodiscard]] inline BackoffFn linear(
    std::chrono::milliseconds base,
    std::chrono::milliseconds cap = 30s)
{
    return [base, cap](int attempt) -> std::chrono::milliseconds {
        const long long val = base.count() * static_cast<long long>(attempt + 1);
        return std::min(std::chrono::milliseconds{val}, cap);
    };
}

/**
 * @brief Exponential backoff — delay = base * 2^attempt, capped at cap.
 * @param base  Starting delay.
 * @param cap   Maximum delay (default: 30 s).
 */
[[nodiscard]] inline BackoffFn exponential(
    std::chrono::milliseconds base,
    std::chrono::milliseconds cap = 30s)
{
    return [base, cap](int attempt) -> std::chrono::milliseconds {
        const int    shift = std::min(attempt, 62);
        const long long val = base.count() * (1LL << shift);
        return std::min(std::chrono::milliseconds{val}, cap);
    };
}

/**
 * @brief AWS Full Jitter backoff — delay = random(0, min(cap, base * 2^attempt)).
 *
 * Recommended strategy for distributed systems: avoids thundering-herd retries
 * by spreading retries uniformly across the window [0, cap].
 *
 * Reference: https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/
 *
 * @param base  Starting delay.
 * @param cap   Maximum delay (default: 30 s).
 */
[[nodiscard]] inline BackoffFn jitter(
    std::chrono::milliseconds base = 500ms,
    std::chrono::milliseconds cap  = 30s)
{
    return [base, cap](int attempt) -> std::chrono::milliseconds {
        const int    shift  = std::min(attempt, 62);
        const long long ceil = std::min(base.count() * (1LL << shift), cap.count());
        if (ceil <= 0) return std::chrono::milliseconds{0};
        thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<long long> dist{0, ceil};
        return std::chrono::milliseconds{dist(rng)};
    };
}

/**
 * @brief Decorrelated Jitter — delay = random(base, prev_delay * 3), capped at cap.
 *
 * An alternative to full jitter with slightly higher average delays but better
 * decorrelation across concurrent clients.
 *
 * @param base  Minimum delay.
 * @param cap   Maximum delay (default: 30 s).
 */
[[nodiscard]] inline BackoffFn decorrelated(
    std::chrono::milliseconds base = 500ms,
    std::chrono::milliseconds cap  = 30s)
{
    // prev_delay is captured in a shared_ptr so the returned lambda is copyable.
    auto prev = std::make_shared<long long>(base.count());
    return [base, cap, prev](int) -> std::chrono::milliseconds {
        thread_local std::mt19937_64 rng{std::random_device{}()};
        const long long lo   = base.count();
        const long long hi   = std::min(*prev * 3, cap.count());
        const long long hi2  = std::max(lo, hi); // guard lo > hi if cap < base
        std::uniform_int_distribution<long long> dist{lo, hi2};
        *prev = dist(rng);
        return std::chrono::milliseconds{*prev};
    };
}

// ─── RetryConfig ─────────────────────────────────────────────────────────────

/**
 * @brief Configuration passed to retry middleware / wrappers.
 */
struct RetryConfig {
    int       max_attempts = 3;        ///< Total attempts (initial + retries)
    BackoffFn policy       = jitter(); ///< Backoff strategy (default: AWS Full Jitter)

    /**
     * @brief Predicate that decides whether a given HTTP status is retryable.
     *
     * Default: retry on network error (status 0), 429 Too Many Requests, and
     * all 5xx server errors.
     */
    std::function<bool(int status)> retryable = [](int s) {
        return s == 0 || s == 429 || s >= 500;
    };
};

// ─── async_sleep ─────────────────────────────────────────────────────────────

namespace detail {

/**
 * @brief Reactor-integrated timer awaiter — suspends the coroutine without
 *        blocking the event-loop thread.
 *
 * Integrates with `Reactor::register_timer()` on whichever reactor is current.
 * Falls back to `std::this_thread::sleep_for` when called outside a reactor
 * context (e.g., unit tests).
 */
struct TimerAwaiter {
    int      timeout_ms;
    Reactor *reactor;

    bool await_ready() const noexcept { return timeout_ms <= 0; }

    void await_suspend(std::coroutine_handle<> h) {
        reactor->register_timer(timeout_ms, [h](int) mutable { h.resume(); });
    }

    void await_resume() noexcept {}
};

} // namespace detail

/**
 * @brief Asynchronously sleep for `delay` without blocking the reactor thread.
 *
 * Uses `Reactor::register_timer()` so that other coroutines continue to run
 * during the wait — unlike `std::this_thread::sleep_for`.
 *
 * Falls back to blocking sleep when called without a running Reactor
 * (e.g., unit-test contexts).
 *
 * @param delay  How long to wait.
 */
inline Task<void> async_sleep(std::chrono::milliseconds delay) {
    if (delay.count() <= 0) co_return;

    Reactor *reactor = Reactor::current();
    if (!reactor) {
        // Off-reactor context: fall back to a blocking sleep.
        std::this_thread::sleep_for(delay);
        co_return;
    }

    const int ms = static_cast<int>(
        std::min(delay.count(),
                 static_cast<long long>(std::numeric_limits<int>::max())));
    co_await detail::TimerAwaiter{ms, reactor};
}

} // namespace qbuem::backoff

/** @} */
