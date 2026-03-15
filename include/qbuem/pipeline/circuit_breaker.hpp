#pragma once

/**
 * @file qbuem/pipeline/circuit_breaker.hpp
 * @brief 서킷 브레이커 — CircuitBreaker, CircuitBreakerAction
 * @defgroup qbuem_circuit_breaker CircuitBreaker
 * @ingroup qbuem_pipeline
 *
 * 3상태(Closed → Open → HalfOpen → Closed) 서킷 브레이커 구현.
 *
 * ## 상태 전이
 * - Closed:   정상 동작. 실패가 failure_threshold에 도달하면 Open으로 전환.
 * - Open:     빠른 실패(fast fail). timeout 후 HalfOpen으로 전환.
 * - HalfOpen: 제한적 요청 허용. success_threshold 성공 시 Closed로 전환.
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

/** @brief 서킷 브레이커 상태 (네임스페이스 레벨 — designated initializer 호환). */
enum class CircuitBreakerState { Closed, Open, HalfOpen };

/**
 * @brief 서킷 브레이커 설정 (네임스페이스 레벨 aggregate — designated initializer 지원).
 *
 * CircuitBreaker 외부에 정의되어 GCC C++20 aggregate 제약을 우회합니다.
 * `CircuitBreaker::Config` alias로도 접근 가능합니다.
 */
struct CircuitBreakerConfig {
    size_t failure_threshold                                         = 5;       ///< Open 전환까지 허용 실패 수
    size_t success_threshold                                         = 2;       ///< HalfOpen→Closed 전환 성공 수
    std::chrono::milliseconds timeout{30000};                                  ///< Open→HalfOpen 대기 시간
    std::function<void(CircuitBreakerState, CircuitBreakerState)> on_state_change = nullptr; ///< 상태 전환 콜백
};

/**
 * @brief 3상태 서킷 브레이커.
 *
 * 스레드 안전한 서킷 브레이커 구현입니다.
 * 분산 시스템에서 연속 장애 확산을 방지합니다.
 */
class CircuitBreaker {
public:
    /** @brief 서킷 브레이커 상태 (하위 호환 alias). */
    using State = CircuitBreakerState;

    /** @brief 서킷 브레이커 설정 (하위 호환 alias). */
    using Config = CircuitBreakerConfig;

    /**
     * @brief 서킷 브레이커를 생성합니다.
     * @param cfg 설정 (기본값 사용 가능).
     */
    explicit CircuitBreaker(Config cfg = {})
        : cfg_(std::move(cfg)) {}

    /**
     * @brief 요청 허용 여부를 확인합니다.
     *
     * Open 상태에서는 timeout 경과 여부를 확인하여 HalfOpen으로 전환 시도합니다.
     *
     * @returns Open 상태(fast fail)이면 false, 그 외 true.
     */
    bool allow_request() noexcept {
        std::lock_guard lock(mtx_);
        try_recover();
        return state_ != State::Open;
    }

    /**
     * @brief 성공을 기록합니다.
     *
     * - Closed: 실패 카운터 리셋.
     * - HalfOpen: 성공 카운터 증가, success_threshold 도달 시 Closed 전환.
     */
    void record_success() noexcept {
        std::lock_guard lock(mtx_);
        switch (state_) {
            case State::Closed:
                failures_ = 0;
                break;
            case State::HalfOpen:
                ++successes_;
                if (successes_ >= cfg_.success_threshold) {
                    transition(State::Closed);
                }
                break;
            case State::Open:
                // Open 상태에서 성공 기록은 무시
                break;
        }
    }

    /**
     * @brief 실패를 기록합니다.
     *
     * - Closed: 실패 카운터 증가, failure_threshold 도달 시 Open 전환.
     * - HalfOpen: Open으로 즉시 전환.
     * - Open: 무시.
     */
    void record_failure() noexcept {
        std::lock_guard lock(mtx_);
        switch (state_) {
            case State::Closed:
                ++failures_;
                if (failures_ >= cfg_.failure_threshold) {
                    transition(State::Open);
                }
                break;
            case State::HalfOpen:
                transition(State::Open);
                break;
            case State::Open:
                // Open 상태에서 추가 실패는 무시
                break;
        }
    }

    /**
     * @brief 현재 상태를 반환합니다.
     */
    State state() const noexcept {
        std::lock_guard lock(mtx_);
        return state_;
    }

    /**
     * @brief 현재 실패 카운터를 반환합니다.
     */
    size_t failure_count() const noexcept {
        std::lock_guard lock(mtx_);
        return failures_;
    }

    /**
     * @brief 현재 성공 카운터를 반환합니다 (HalfOpen 상태에서 유효).
     */
    size_t success_count() const noexcept {
        std::lock_guard lock(mtx_);
        return successes_;
    }

    /**
     * @brief 서킷 브레이커를 Closed 상태로 리셋합니다.
     */
    void reset() noexcept {
        std::lock_guard lock(mtx_);
        failures_  = 0;
        successes_ = 0;
        state_     = State::Closed;
        opened_at_ = {};
    }

private:
    Config                                    cfg_;
    mutable std::mutex                        mtx_;
    State                                     state_{State::Closed};
    size_t                                    failures_{0};
    size_t                                    successes_{0};
    std::chrono::steady_clock::time_point     opened_at_{};

    /**
     * @brief Open 상태에서 timeout이 경과했는지 확인하고 HalfOpen 전환을 시도합니다.
     *
     * @note mtx_ 보유 상태에서 호출해야 합니다.
     */
    void try_recover() noexcept {
        if (state_ != State::Open) return;
        auto now = std::chrono::steady_clock::now();
        if (now - opened_at_ >= cfg_.timeout) {
            transition(State::HalfOpen);
        }
    }

    /**
     * @brief 상태 전환을 수행하고 콜백을 호출합니다.
     *
     * @param to 전환할 대상 상태.
     * @note mtx_ 보유 상태에서 호출해야 합니다.
     */
    void transition(State to) noexcept {
        State from = state_;
        state_     = to;

        if (to == State::Open) {
            opened_at_ = std::chrono::steady_clock::now();
            failures_  = 0;
            successes_ = 0;
        } else if (to == State::Closed) {
            failures_  = 0;
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
 * @brief CircuitBreaker로 보호되는 액션 래퍼.
 *
 * 서킷이 Open 상태이면 fast fail (errc::connection_refused).
 * 성공/실패 결과를 서킷 브레이커에 자동으로 기록합니다.
 *
 * @tparam In  입력 타입.
 * @tparam Out 출력 타입.
 */
template <typename In, typename Out>
class CircuitBreakerAction {
public:
    /** @brief 내부 액션 함수 타입. */
    using InnerFn = std::function<Task<Result<Out>>(In, ActionEnv)>;

    /**
     * @brief CircuitBreakerAction을 생성합니다.
     * @param cb  공유 CircuitBreaker 인스턴스.
     * @param fn  보호할 내부 액션 함수.
     */
    CircuitBreakerAction(std::shared_ptr<CircuitBreaker> cb, InnerFn fn)
        : cb_(std::move(cb)), fn_(std::move(fn)) {}

    /**
     * @brief CircuitBreaker를 통해 액션을 실행합니다.
     *
     * @param item 처리할 아이템.
     * @param env  실행 환경.
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
     * @brief 내부 CircuitBreaker에 대한 참조를 반환합니다.
     */
    const CircuitBreaker& breaker() const { return *cb_; }

private:
    std::shared_ptr<CircuitBreaker> cb_;
    InnerFn                         fn_;
};

} // namespace qbuem

/** @} */
