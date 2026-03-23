#pragma once

/**
 * @file qbuem/http/fetch_pipeline.hpp
 * @brief Composable middleware stages for the qbuem::fetch() outbound HTTP client.
 * @defgroup qbuem_http_fetch_pipeline HTTP Fetch Pipeline
 * @ingroup qbuem_http
 *
 * Provides retry, circuit-breaker, and rate-limit-guard wrappers that can be
 * layered around a `FetchRequest::send()` call.
 *
 * ## Usage
 * @code
 * using namespace qbuem::http;
 *
 * // Shared circuit breaker (keep alive across calls)
 * auto cb = std::make_shared<FetchCircuitBreaker>();
 *
 * Task<Result<FetchResponse>> my_api_call(std::stop_token st) {
 *     auto call = [&](std::stop_token t) {
 *         return fetch("https://api.example.com/data").send(t);
 *     };
 *
 *     co_return co_await retry_jitter(3)(call, st);          // retry layer
 *     // or, composing multiple layers:
 *     auto with_cb = circuit_breaker_wrap(cb, call);
 *     co_return co_await retry_jitter(3)(with_cb, st);
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/http/backoff.hpp>
#include <qbuem/http/fetch.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>

namespace qbuem::http {

using namespace std::chrono_literals;

// ─── FetchCall / FetchMiddleware ──────────────────────────────────────────────

/**
 * @brief A callable that performs (or wraps) an outbound HTTP fetch.
 *
 * Signature: `Task<Result<FetchResponse>>(std::stop_token)`
 */
using FetchCall = std::function<Task<Result<FetchResponse>>(std::stop_token)>;

// ─── retry ────────────────────────────────────────────────────────────────────

/**
 * @brief Retry wrapper: repeats `call` up to `cfg.max_attempts` times.
 *
 * Waits between retries using the backoff policy in `cfg`.  Reads the
 * `Retry-After` header on 429 responses and overrides the computed delay
 * when present.
 *
 * @param cfg  Retry configuration (attempts, backoff policy, retryable predicate).
 * @returns    A callable `(FetchCall call, std::stop_token) -> Task<Result<FetchResponse>>`
 */
[[nodiscard]] inline auto retry(backoff::RetryConfig cfg = {}) {
    return [cfg = std::move(cfg)](FetchCall call,
                                  std::stop_token st)
               -> Task<Result<FetchResponse>> {
        Result<FetchResponse> last =
            std::unexpected(std::make_error_code(std::errc::operation_not_permitted));

        for (int attempt = 0; attempt < cfg.max_attempts; ++attempt) {
            if (st.stop_requested())
                co_return std::unexpected(
                    std::make_error_code(std::errc::operation_canceled));

            last = co_await call(st);

            if (last.has_value() && !cfg.retryable(last->status()))
                co_return last; // success or non-retryable error code

            if (!last.has_value()) {
                // Network / transport error — retryable by default
            } else if (!cfg.retryable(last->status())) {
                co_return last; // non-retryable HTTP status
            }

            if (attempt + 1 == cfg.max_attempts)
                break; // no more retries

            // Compute delay — honour Retry-After header on 429
            auto delay = cfg.policy(attempt);
            if (last.has_value() && last->status() == 429) {
                std::string_view ra = last->header("retry-after");
                if (!ra.empty()) {
                    long long secs = 0;
                    if (auto [p, ec] = std::from_chars(
                            ra.data(), ra.data() + ra.size(), secs);
                        ec == std::errc{}) {
                        delay = std::chrono::milliseconds{secs * 1000};
                    }
                }
            }

            co_await backoff::async_sleep(delay);
        }

        co_return last;
    };
}

/**
 * @brief Retry with fixed-delay backoff.
 *
 * @param max_attempts  Maximum number of total attempts.
 * @param delay         Fixed delay between retries.
 */
[[nodiscard]] inline auto retry_fixed(int max_attempts,
                                       std::chrono::milliseconds delay) {
    return retry(backoff::RetryConfig{
        .max_attempts = max_attempts,
        .policy       = backoff::fixed(delay),
    });
}

/**
 * @brief Retry with AWS Full Jitter backoff (recommended default).
 *
 * @param max_attempts  Maximum number of total attempts.
 * @param base          Base delay for jitter calculation (default: 500 ms).
 * @param cap           Maximum delay cap (default: 30 s).
 */
[[nodiscard]] inline auto retry_jitter(
    int max_attempts = 3,
    std::chrono::milliseconds base = 500ms,
    std::chrono::milliseconds cap  = 30s)
{
    return retry(backoff::RetryConfig{
        .max_attempts = max_attempts,
        .policy       = backoff::jitter(base, cap),
    });
}

// ─── FetchCircuitBreaker ──────────────────────────────────────────────────────

/**
 * @brief Three-state (Closed → Open → HalfOpen) circuit breaker for outbound
 *        fetch calls.
 *
 * Thread-safe. Intended to be held in a `shared_ptr` and shared across
 * coroutines that call the same upstream service.
 */
class FetchCircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    struct Config {
        int                       fail_threshold = 5;     ///< Failures before opening
        int                       success_threshold = 2;  ///< Successes to re-close from HalfOpen
        std::chrono::seconds      reset_after{30};        ///< Open → HalfOpen timeout
    };

    explicit FetchCircuitBreaker(Config cfg = {}) : cfg_(cfg) {}

    /** @brief Returns true when a request should be allowed through. */
    bool allow() noexcept {
        State s = state_.load(std::memory_order_acquire);
        if (s == State::Closed || s == State::HalfOpen) return true;
        // Open — check if reset_after has elapsed
        std::lock_guard lock{mu_};
        if (std::chrono::steady_clock::now() - opened_at_ >= cfg_.reset_after) {
            transition(State::HalfOpen);
            return true;
        }
        return false;
    }

    void record_success() noexcept {
        std::lock_guard lock{mu_};
        State s = state_.load(std::memory_order_relaxed);
        if (s == State::HalfOpen) {
            if (++successes_ >= cfg_.success_threshold)
                transition(State::Closed);
        } else if (s == State::Closed) {
            failures_ = 0;
        }
    }

    void record_failure() noexcept {
        std::lock_guard lock{mu_};
        State s = state_.load(std::memory_order_relaxed);
        if (s == State::Closed) {
            if (++failures_ >= cfg_.fail_threshold)
                transition(State::Open);
        } else if (s == State::HalfOpen) {
            transition(State::Open);
        }
    }

    State state() const noexcept { return state_.load(std::memory_order_acquire); }

private:
    Config                                   cfg_;
    mutable std::mutex                       mu_;
    std::atomic<State>                       state_{State::Closed};
    int                                      failures_{0};
    int                                      successes_{0};
    std::chrono::steady_clock::time_point    opened_at_{};

    void transition(State to) noexcept {
        state_.store(to, std::memory_order_release);
        if (to == State::Open)  { opened_at_ = std::chrono::steady_clock::now(); failures_ = 0; successes_ = 0; }
        if (to == State::Closed) { failures_ = 0; successes_ = 0; }
        if (to == State::HalfOpen) { successes_ = 0; }
    }
};

// ─── circuit_breaker ─────────────────────────────────────────────────────────

/**
 * @brief Circuit-breaker wrapper for a `FetchCall`.
 *
 * Fast-fails with `errc::connection_refused` when the breaker is Open.
 * Records success/failure to transition the breaker state.
 *
 * @param cb    Shared circuit breaker instance.
 * @param call  Inner fetch call to protect.
 * @param st    Cancellation token.
 */
[[nodiscard]] inline Task<Result<FetchResponse>>
circuit_breaker_wrap(std::shared_ptr<FetchCircuitBreaker> cb,
                     FetchCall call,
                     std::stop_token st)
{
    if (!cb->allow())
        co_return std::unexpected(
            std::make_error_code(std::errc::connection_refused));

    auto result = co_await call(st);

    if (result.has_value() && result->status() > 0 && result->status() < 500)
        cb->record_success();
    else
        cb->record_failure();

    co_return result;
}

/**
 * @brief Returns a middleware factory that wraps a `FetchCall` with a
 *        `FetchCircuitBreaker`.
 *
 * @code
 * auto cb = std::make_shared<FetchCircuitBreaker>();
 * auto call = [&](std::stop_token t){ return fetch(url).send(t); };
 * auto result = co_await circuit_breaker(cb)(call, st);
 * @endcode
 *
 * @param cb          Shared circuit breaker instance.
 * @param fail_threshold  (Overrides cb config) — pass 0 to use cb's own config.
 * @param reset_after     (Overrides cb config) — pass 0s to use cb's own config.
 */
[[nodiscard]] inline auto circuit_breaker(
    std::shared_ptr<FetchCircuitBreaker> cb)
{
    return [cb = std::move(cb)](FetchCall call,
                                std::stop_token st)
               -> Task<Result<FetchResponse>> {
        co_return co_await circuit_breaker_wrap(cb, std::move(call), st);
    };
}

// ─── rate_limit_guard ────────────────────────────────────────────────────────

/**
 * @brief Rate-limit guard: on a 429 response, reads `Retry-After` and
 *        sleeps before propagating the error upward.
 *
 * Cooperates with the retry() wrapper: place rate_limit_guard() as the
 * inner call so retry() sees the 429 after the Retry-After wait has elapsed
 * and the delay budget has been consumed.
 *
 * @code
 * auto call = rate_limit_guard_wrap([&](std::stop_token t){
 *     return fetch(url).send(t);
 * });
 * co_return co_await retry_jitter(3)(call, st);
 * @endcode
 */
[[nodiscard]] inline FetchCall rate_limit_guard_wrap(FetchCall call) {
    return [call = std::move(call)](std::stop_token st)
               -> Task<Result<FetchResponse>> {
        auto result = co_await call(st);

        if (!result.has_value() || result->status() != 429)
            co_return result;

        // Honour Retry-After header if present
        std::string_view ra = result->header("retry-after");
        if (!ra.empty()) {
            long long secs = 0;
            if (auto [p, ec] = std::from_chars(
                    ra.data(), ra.data() + ra.size(), secs);
                ec == std::errc{} && secs > 0)
            {
                co_await backoff::async_sleep(
                    std::chrono::milliseconds{secs * 1000});
            }
        }

        co_return result;
    };
}

/**
 * @brief Returns a middleware factory that wraps a `FetchCall` with the
 *        rate-limit guard.
 */
[[nodiscard]] inline auto rate_limit_guard() {
    return [](FetchCall call, std::stop_token) -> FetchCall {
        return rate_limit_guard_wrap(std::move(call));
    };
}

} // namespace qbuem::http

/** @} */
