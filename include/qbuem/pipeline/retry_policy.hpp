#pragma once

/**
 * @file qbuem/pipeline/retry_policy.hpp
 * @brief 재시도 정책 — RetryAction, RetryConfig, BackoffStrategy
 * @defgroup qbuem_retry RetryPolicy
 * @ingroup qbuem_pipeline
 *
 * RetryAction은 액션 함수를 재시도 로직으로 감쌉니다.
 * 지수 백오프, 고정 지연, 지터(jitter) 전략을 지원합니다.
 *
 * ## 사용 예시
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
 * @brief 백오프 전략 열거형.
 *
 * - Fixed:       모든 재시도에 동일한 지연 적용.
 * - Exponential: 지연 = base * 2^attempt (max_delay 상한).
 * - Jitter:      지연 = base * 2^attempt + random(0, base * 2^attempt * 0.1).
 */
enum class BackoffStrategy { Fixed, Exponential, Jitter };

/**
 * @brief 재시도 정책 설정 구조체.
 */
struct RetryConfig {
    size_t max_attempts                              = 3;       ///< 최대 시도 횟수
    std::chrono::milliseconds base_delay{100};                  ///< 기본 지연
    std::chrono::milliseconds max_delay{30000};                 ///< 최대 지연 상한
    BackoffStrategy strategy                         = BackoffStrategy::Exponential; ///< 백오프 전략
    /// 재시도 가능 여부 판단 함수 (기본: 모든 에러 재시도)
    std::function<bool(const std::error_code&)> is_retriable = [](const std::error_code&) { return true; };
};

/**
 * @brief 재시도 래퍼 액션.
 *
 * 내부 액션 함수를 감싸 RetryConfig에 따라 실패 시 재시도합니다.
 * 최종 실패 시 마지막 에러를 반환합니다.
 *
 * @tparam In  입력 타입.
 * @tparam Out 출력 타입.
 */
template <typename In, typename Out>
class RetryAction {
public:
    /** @brief 내부 액션 함수 타입. */
    using InnerFn = std::function<Task<Result<Out>>(In, ActionEnv)>;

    /**
     * @brief RetryAction을 생성합니다.
     * @param fn  감쌀 액션 함수.
     * @param cfg 재시도 정책 설정.
     */
    RetryAction(InnerFn fn, RetryConfig cfg = {})
        : fn_(std::move(fn)), cfg_(std::move(cfg)) {}

    /**
     * @brief 재시도 로직으로 액션을 실행합니다.
     *
     * 실패 시 RetryConfig에 따라 재시도하며,
     * 최종 실패 시 마지막 에러를 반환합니다.
     *
     * @param item 처리할 아이템.
     * @param env  실행 환경.
     * @returns Task<Result<Out>>
     */
    Task<Result<Out>> operator()(In item, ActionEnv env) {
        Result<Out> last_result = unexpected(std::make_error_code(std::errc::operation_not_permitted));

        for (size_t attempt = 0; attempt < cfg_.max_attempts; ++attempt) {
            // 취소 확인
            if (env.stop.stop_requested()) {
                co_return unexpected(std::make_error_code(std::errc::operation_canceled));
            }

            // 첫 번째 시도가 아니면 지연 적용
            if (attempt > 0) {
                auto delay = compute_delay(attempt - 1);
                co_await sleep_async(delay);

                if (env.stop.stop_requested()) {
                    co_return unexpected(std::make_error_code(std::errc::operation_canceled));
                }
            }

            // In을 복사해서 전달 (재시도를 위해 원본 보존)
            auto result = co_await fn_(item, env);

            if (result.has_value()) {
                co_return result;
            }

            // 재시도 가능 여부 확인
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
     * @brief Reactor 타이머를 이용한 비동기 슬립.
     *
     * Reactor가 현재 스레드에 없으면 std::this_thread::sleep_for로 대체합니다.
     *
     * @param delay 대기 시간.
     */
    Task<void> sleep_async(std::chrono::milliseconds delay) {
        auto *reactor = Reactor::current();
        if (!reactor) {
            std::this_thread::sleep_for(delay);
            co_return;
        }

        // Reactor::register_timer를 통한 비동기 대기
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
            delay.count(),
            static_cast<long long>(std::numeric_limits<int>::max())
        ));
        co_await TimerAwaiter{reactor, ms};
        co_return;
    }

    /**
     * @brief 시도 횟수에 따른 지연 시간을 계산합니다.
     *
     * @param attempt 0-based 시도 횟수 (첫 재시도 = 0).
     * @returns 계산된 지연 시간.
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

                // 지터: 0 ~ base_val * 0.1 범위의 랜덤 값
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

        // max_delay 상한 적용
        if (delay > cfg_.max_delay)
            delay = cfg_.max_delay;

        return delay;
    }
};

/**
 * @brief 임의의 ActionFn에서 RetryAction을 생성하는 팩토리 함수.
 *
 * @tparam In  입력 타입.
 * @tparam Out 출력 타입.
 * @tparam FnT 액션 함수 타입.
 * @param fn  감쌀 액션 함수.
 * @param cfg 재시도 정책 설정.
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
