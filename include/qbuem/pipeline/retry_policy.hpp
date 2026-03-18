#pragma once

/**
 * @file qbuem/pipeline/retry_policy.hpp
 * @brief Retry policy — RetryAction, RetryConfig, BackoffStrategy
 * @defgroup qbuem_retry RetryPolicy
 * @ingroup qbuem_pipeline
 *
 * RetryAction wraps an action function with retry logic.
 * Supports exponential backoff, fixed delay, and jitter strategies.
 *
 * ## Usage example
 * @code
 * RetryConfig cfg{.max_attempts=5, .strategy=BackoffStrategy::Jitter};
 * auto retry_fn = make_retry_action<int, int>(my_fn, cfg);
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>

#include <chrono>
#include <coroutine>
#include <functional>
#include <random>
#include <system_error>
#include <thread>

namespace qbuem {

/**
 * @brief Backoff strategy enumeration.
 *
 * - Fixed:       Applies the same delay to every retry.
 * - Exponential: delay = base * 2^attempt (capped at max_delay).
 * - Jitter:      delay = base * 2^attempt + random(0, base * 2^attempt * 0.1).
 */
enum class BackoffStrategy { Fixed, Exponential, Jitter };

/**
 * @brief Retry policy configuration structure.
 */
struct RetryConfig {
    size_t max_attempts                              = 3;       ///< Maximum number of attempts
    std::chrono::milliseconds base_delay{100};                  ///< Base delay
    std::chrono::milliseconds max_delay{30000};                 ///< Maximum delay cap
    BackoffStrategy strategy                         = BackoffStrategy::Exponential; ///< Backoff strategy
    /// Function to determine whether an error is retriable (default: retry all errors)
    std::function<bool(const std::error_code&)> is_retriable = [](const std::error_code&) { return true; };
};

/**
 * @brief Retry wrapper action.
 *
 * Wraps an inner action function and retries on failure according to RetryConfig.
 * Returns the last error on final failure.
 *
 * @tparam In  Input type.
 * @tparam Out Output type.
 */
template <typename In, typename Out>
class RetryAction {
public:
    /** @brief Inner action function type. */
    using InnerFn = std::function<Task<Result<Out>>(In, ActionEnv)>;

    /**
     * @brief Constructs a RetryAction.
     * @param fn  Action function to wrap.
     * @param cfg Retry policy configuration.
     */
    RetryAction(InnerFn fn, RetryConfig cfg = {})
        : fn_(std::move(fn)), cfg_(std::move(cfg)) {}

    /**
     * @brief Executes the action with retry logic.
     *
     * Retries on failure according to RetryConfig.
     * Returns the last error on final failure.
     *
     * @param item Item to process.
     * @param env  Execution environment.
     * @returns Task<Result<Out>>
     */
    Task<Result<Out>> operator()(In item, ActionEnv env) {
        Result<Out> last_result = unexpected(std::make_error_code(std::errc::operation_not_permitted));

        for (size_t attempt = 0; attempt < cfg_.max_attempts; ++attempt) {
            // Check for cancellation
            if (env.stop.stop_requested()) {
                co_return unexpected(std::make_error_code(std::errc::operation_canceled));
            }

            // Apply delay if not the first attempt
            if (attempt > 0) {
                auto delay = compute_delay(attempt - 1);
                co_await sleep_async(delay);

                if (env.stop.stop_requested()) {
                    co_return unexpected(std::make_error_code(std::errc::operation_canceled));
                }
            }

            // Pass a copy of In (preserve original for retries)
            auto result = co_await fn_(item, env);

            if (result.has_value()) {
                co_return result;
            }

            // Check whether the error is retriable
            last_result = std::move(result);
            if (!cfg_.is_retriable(last_result.error())) {
                break;
            }
        }

        co_return last_result;
    }

private:
    InnerFn    fn_;
    RetryConfig cfg_;

    /**
     * @brief Asynchronous sleep using the Reactor timer.
     *
     * Falls back to std::this_thread::sleep_for if no Reactor is running on the current thread.
     *
     * @param delay Duration to wait.
     */
    Task<void> sleep_async(std::chrono::milliseconds delay) {
        auto *reactor = Reactor::current();
        if (!reactor) {
            std::this_thread::sleep_for(delay);
            co_return;
        }

        // Async wait via Reactor::register_timer
        struct TimerAwaiter {
            Reactor* reactor;
            int timeout_ms;
            bool ready = false;
            std::coroutine_handle<> handle;

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) {
                handle = h;
                reactor->register_timer(timeout_ms, [this](int) {
                    ready = true;
                    if (handle)
                        handle.resume();
                });
            }

            void await_resume() noexcept {}
        };

        int ms = static_cast<int>(std::min(
            static_cast<long long>(delay.count()),
            static_cast<long long>(std::numeric_limits<int>::max())
        ));
        co_await TimerAwaiter{reactor, ms};
        co_return;
    }

    /**
     * @brief Computes the delay for the given attempt number.
     *
     * @param attempt 0-based attempt index (first retry = 0).
     * @returns Computed delay duration.
     */
    std::chrono::milliseconds compute_delay(size_t attempt) const {
        using ms = std::chrono::milliseconds;

        ms delay = cfg_.base_delay;

        switch (cfg_.strategy) {
            case BackoffStrategy::Fixed:
                delay = cfg_.base_delay;
                break;

            case BackoffStrategy::Exponential: {
                // delay = base * 2^attempt
                long long factor = 1LL << std::min(attempt, size_t{62});
                long long val = cfg_.base_delay.count() * factor;
                delay = ms{val};
                break;
            }

            case BackoffStrategy::Jitter: {
                // delay = base * 2^attempt + random(0, base * 2^attempt * 0.1)
                long long factor = 1LL << std::min(attempt, size_t{62});
                long long base_val = cfg_.base_delay.count() * factor;

                // Jitter: random value in the range [0, base_val * 0.1]
                long long jitter_range = static_cast<long long>(base_val * 0.1);
                long long jitter = 0;
                if (jitter_range > 0) {
                    thread_local std::mt19937 rng{std::random_device{}()};
                    std::uniform_int_distribution<long long> dist(0, jitter_range);
                    jitter = dist(rng);
                }

                delay = ms{base_val + jitter};
                break;
            }
        }

        // Apply max_delay cap
        if (delay > cfg_.max_delay)
            delay = cfg_.max_delay;

        return delay;
    }
};

/**
 * @brief Factory function that creates a RetryAction from an arbitrary ActionFn.
 *
 * @tparam In  Input type.
 * @tparam Out Output type.
 * @tparam FnT Action function type.
 * @param fn  Action function to wrap.
 * @param cfg Retry policy configuration.
 * @returns RetryAction<In, Out>
 */
template <typename In, typename Out, typename FnT>
auto make_retry_action(FnT fn, RetryConfig cfg = {})
    -> RetryAction<In, Out>
{
    using InnerFn = typename RetryAction<In, Out>::InnerFn;
    return RetryAction<In, Out>(InnerFn(std::move(fn)), std::move(cfg));
}

} // namespace qbuem

/** @} */
