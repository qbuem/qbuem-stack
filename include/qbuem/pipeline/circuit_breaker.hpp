#pragma once

/**
 * @file qbuem/pipeline/circuit_breaker.hpp
 * @brief Circuit breaker — CircuitBreaker, CircuitBreakerAction
 * @defgroup qbuem_circuit_breaker CircuitBreaker
 * @ingroup qbuem_pipeline
 *
 * Three-state (Closed → Open → HalfOpen → Closed) circuit breaker implementation.
 *
 * ## State transitions
 * - Closed:   Normal operation. Transitions to Open after `failure_threshold`
 *             **consecutive** failures. Any success resets the failure streak,
 *             so this is a consecutive-failure breaker (not a failure-rate one);
 *             alternating success/failure traffic will not trip it.
 * - Open:     Fast fail. Transitions to HalfOpen after timeout.
 * - HalfOpen: Limited requests allowed. Transitions to Closed when success_threshold is reached.
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>

namespace qbuem {

/** @brief Circuit breaker state (namespace-level — designated initializer compatible). */
enum class CircuitBreakerState { Closed, Open, HalfOpen };

/**
 * @brief Circuit breaker configuration (namespace-level aggregate — designated initializer support).
 *
 * Defined outside CircuitBreaker to work around GCC C++20 aggregate restrictions.
 * Also accessible as the `CircuitBreaker::Config` alias.
 */
struct CircuitBreakerConfig {
    size_t failure_threshold                                         = 5;       ///< Consecutive failures before Open (any success resets the streak)
    size_t success_threshold                                         = 2;       ///< Successes required for HalfOpen→Closed transition
    std::chrono::milliseconds timeout{30000};                                  ///< Wait time for Open→HalfOpen transition
    std::function<void(CircuitBreakerState, CircuitBreakerState)> on_state_change = nullptr; ///< State transition callback
};

/**
 * @brief Three-state circuit breaker.
 *
 * Thread-safe circuit breaker implementation.
 * Prevents cascading failures in distributed systems.
 */
class CircuitBreaker {
public:
    /** @brief Circuit breaker state (backward-compatible alias). */
    using State = CircuitBreakerState;

    /** @brief Circuit breaker configuration (backward-compatible alias). */
    using Config = CircuitBreakerConfig;

    /**
     * @brief Creates the circuit breaker.
     * @param cfg Configuration (defaults are usable as-is).
     */
    explicit CircuitBreaker(Config cfg = {})
        : cfg_(std::move(cfg)) {}

    /**
     * @brief Checks whether a request is permitted.
     *
     * In the Open state, checks whether timeout has elapsed and attempts to transition to HalfOpen.
     *
     * @returns false if in Open state (fast fail), true otherwise.
     */
    bool allow_request() noexcept {
        // Fast path: Closed or HalfOpen — no lock needed.
        State s = state_.load(std::memory_order_acquire);
        if (s != State::Open) return true;
        // Slow path: Open — check timeout under lock and possibly transition.
        std::lock_guard lock(mtx_);
        try_recover();
        return state_.load(std::memory_order_relaxed) != State::Open;
    }

    /**
     * @brief Records a success.
     *
     * - Closed: resets the failure counter.
     * - HalfOpen: increments the success counter; transitions to Closed when success_threshold is reached.
     */
    void record_success() noexcept {
        // Lock-free fast path: in Closed (the overwhelmingly common state) a
        // success just clears the consecutive-failure streak — no mutex, so a
        // shared breaker behind a hot stage does not serialize all workers.
        if (state_.load(std::memory_order_acquire) == State::Closed) {
            failures_.store(0, std::memory_order_relaxed);
            return;
        }
        std::lock_guard lock(mtx_);
        switch (state_) {
            case State::Closed:
                failures_.store(0, std::memory_order_relaxed);
                break;
            case State::HalfOpen:
                ++successes_;
                if (successes_ >= cfg_.success_threshold) {
                    transition(State::Closed);
                }
                break;
            case State::Open:
                // Success records in Open state are ignored
                break;
        }
    }

    /**
     * @brief Records a failure.
     *
     * - Closed: increments the failure counter; transitions to Open when failure_threshold is reached.
     * - HalfOpen: transitions to Open immediately.
     * - Open: ignored.
     */
    void record_failure() noexcept {
        // Lock-free fast path in Closed: atomically bump the failure counter and
        // only take the mutex to perform the Closed→Open transition once the
        // threshold is crossed (double-checked so exactly one thread transitions).
        if (state_.load(std::memory_order_acquire) == State::Closed) {
            const size_t f =
                failures_.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (f >= cfg_.failure_threshold) {
                std::lock_guard lock(mtx_);
                if (state_.load(std::memory_order_relaxed) == State::Closed)
                    transition(State::Open);
            }
            return;
        }
        std::lock_guard lock(mtx_);
        switch (state_) {
            case State::Closed:
                if ((failures_.fetch_add(1, std::memory_order_acq_rel) + 1) >=
                    cfg_.failure_threshold) {
                    transition(State::Open);
                }
                break;
            case State::HalfOpen:
                transition(State::Open);
                break;
            case State::Open:
                // Additional failures in Open state are ignored
                break;
        }
    }

    /**
     * @brief Returns the current state.
     */
    State state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    /**
     * @brief Returns the current failure counter.
     */
    size_t failure_count() const noexcept {
        return failures_.load(std::memory_order_acquire);
    }

    /**
     * @brief Returns the current success counter (meaningful in the HalfOpen state).
     */
    size_t success_count() const noexcept {
        std::lock_guard lock(mtx_);
        return successes_;
    }

    /**
     * @brief Resets the circuit breaker to the Closed state.
     */
    void reset() noexcept {
        std::lock_guard lock(mtx_);
        failures_.store(0, std::memory_order_relaxed);
        successes_ = 0;
        state_.store(State::Closed, std::memory_order_release);
        opened_at_ = {};
    }

private:
    Config                                    cfg_;
    mutable std::mutex                        mtx_;
    std::atomic<State>                        state_{State::Closed};
    std::atomic<size_t>                       failures_{0};  // lock-free Closed path
    size_t                                    successes_{0};  // HalfOpen only (under mtx_)
    std::chrono::steady_clock::time_point     opened_at_{};

    /**
     * @brief Checks whether timeout has elapsed in the Open state and attempts to transition to HalfOpen.
     *
     * @note Must be called while holding mtx_.
     */
    void try_recover() noexcept {
        if (state_ != State::Open) return;
        auto now = std::chrono::steady_clock::now();
        if (now - opened_at_ >= cfg_.timeout) {
            transition(State::HalfOpen);
        }
    }

    /**
     * @brief Performs a state transition and invokes the callback.
     *
     * @param to Target state to transition to.
     * @note Must be called while holding mtx_.
     */
    void transition(State to) noexcept {
        State from = state_.load(std::memory_order_relaxed);
        state_.store(to, std::memory_order_release);

        if (to == State::Open) {
            opened_at_ = std::chrono::steady_clock::now();
            failures_.store(0, std::memory_order_relaxed);
            successes_ = 0;
        } else if (to == State::Closed) {
            failures_.store(0, std::memory_order_relaxed);
            successes_ = 0;
        } else if (to == State::HalfOpen) {
            successes_ = 0;
        }

        if (cfg_.on_state_change) {
            cfg_.on_state_change(from, to);
        }
    }
};

/**
 * @brief Action wrapper protected by a CircuitBreaker.
 *
 * Fast-fails with errc::connection_refused when the circuit is Open.
 * Automatically records success/failure results to the circuit breaker.
 *
 * @tparam In  Input type.
 * @tparam Out Output type.
 */
template <typename In, typename Out>
class CircuitBreakerAction {
public:
    /** @brief Inner action function type. */
    using InnerFn = std::function<Task<Result<Out>>(In, ActionEnv)>;

    /**
     * @brief Creates a CircuitBreakerAction.
     * @param cb  Shared CircuitBreaker instance.
     * @param fn  Inner action function to protect.
     */
    CircuitBreakerAction(std::shared_ptr<CircuitBreaker> cb, InnerFn fn)
        : cb_(std::move(cb)), fn_(std::move(fn)) {}

    /**
     * @brief Executes the action through the CircuitBreaker.
     *
     * @param item Item to process.
     * @param env  Execution environment.
     * @returns Task<Result<Out>>
     */
    Task<Result<Out>> operator()(In item, ActionEnv env) {
        if (!cb_->allow_request()) {
            co_return unexpected(std::make_error_code(std::errc::connection_refused));
        }

        auto result = co_await fn_(std::move(item), env);

        if (result.has_value()) {
            cb_->record_success();
        } else {
            cb_->record_failure();
        }

        co_return result;
    }

    /**
     * @brief Returns a reference to the underlying CircuitBreaker.
     */
    const CircuitBreaker& breaker() const { return *cb_; }

private:
    std::shared_ptr<CircuitBreaker> cb_;
    InnerFn                         fn_;
};

} // namespace qbuem

/** @} */
