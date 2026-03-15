#pragma once

/**
 * @file qbuem/pipeline/saga.hpp
 * @brief Saga 패턴 — SagaStep, SagaOrchestrator
 * @defgroup qbuem_saga Saga
 * @ingroup qbuem_pipeline
 *
 * Saga 패턴은 분산 트랜잭션을 순차적 단계로 분해하고,
 * 중간 단계 실패 시 이미 완료된 단계를 역순으로 보상(compensation)합니다.
 *
 * ## 동작 개요
 * 1. `SagaOrchestrator::run(input)`은 등록된 단계를 순서대로 실행합니다.
 * 2. 단계 N이 실패하면 단계 N-1, N-2, ... 0 순으로 보상 함수를 호출합니다.
 * 3. 보상 함수 실패는 예외를 발생시키지 않고 `compensation_failures_`에 기록됩니다.
 *
 * ## 사용 예시
 * @code
 * SagaOrchestrator<Order> saga;
 * saga.add_step(SagaStep<Order, Order>{
 *     .name      = "reserve_inventory",
 *     .execute   = [](Order o) -> Task<Result<Order>> { ... co_return o; },
 *     .compensate = [](Order o) -> Task<void> { ... co_return; },
 * });
 * auto result = co_await saga.run(order, ctx);
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/context.hpp>

#include <functional>
#include <string>
#include <vector>

namespace qbuem {

/**
 * @brief Saga 분산 트랜잭션의 단일 실행 단계.
 *
 * 각 단계는 `execute` 함수와 `compensate` 함수로 구성됩니다.
 * `execute`가 성공하면 다음 단계로 진행하고,
 * 이후 단계가 실패하면 `compensate`가 역순으로 호출됩니다.
 *
 * @tparam In  단계 입력 타입.
 * @tparam Out 단계 출력 타입.
 */
template <typename In, typename Out>
struct SagaStep {
    /** @brief 단계 이름 (로깅 및 오류 메시지에 사용). */
    std::string name;

    /**
     * @brief 단계 실행 함수.
     *
     * 성공 시 `Result<Out>`에 값을 담아 반환합니다.
     * 실패 시 `Result<Out>`에 에러를 담아 반환합니다.
     *
     * @param input 단계 입력값.
     */
    std::function<Task<Result<Out>>(In)> execute;

    /**
     * @brief 보상(롤백) 함수.
     *
     * 이후 단계 실패로 인한 롤백 시 호출됩니다.
     * 원본 입력값을 받아 단계의 부작용을 취소합니다.
     *
     * @param input 단계가 처리했던 원본 입력값.
     */
    std::function<Task<void>(In)> compensate;
};

/**
 * @brief Saga 단계를 순차 실행하고 실패 시 역순 보상을 수행하는 오케스트레이터.
 *
 * ### 실행 흐름
 * ```
 * step[0].execute → step[1].execute → ... → step[N].execute
 *                                              ↓ 실패
 * step[N-1].compensate ← ... ← step[1].compensate ← step[0].compensate
 * ```
 *
 * ### 보상 실패 처리
 * 보상 함수가 예외를 던지더라도 오케스트레이터는 중단하지 않고
 * 나머지 보상 단계를 계속 실행합니다.
 * 보상 실패는 `compensation_failures()`로 조회할 수 있습니다.
 *
 * @tparam T 파이프라인 아이템 타입 (각 단계의 입력/출력 타입).
 */
template <typename T>
class SagaOrchestrator {
public:
    /**
     * @brief 단계를 순서대로 등록합니다.
     *
     * 등록 순서가 실행 순서를 결정합니다.
     * 보상은 역순으로 수행됩니다.
     *
     * @param step 등록할 Saga 단계.
     * @returns 메서드 체이닝을 위한 `*this` 참조.
     */
    SagaOrchestrator& add_step(SagaStep<T, T> step) {
        steps_.push_back(std::move(step));
        return *this;
    }

    /**
     * @brief 등록된 모든 단계를 순차적으로 실행합니다.
     *
     * 단계 N이 실패하면 단계 N-1부터 0까지 역순으로 보상 함수를 호출합니다.
     * 모든 단계가 성공하면 마지막 단계의 출력값을 반환합니다.
     *
     * @param input 첫 번째 단계에 전달할 초기 입력값.
     * @param ctx   Saga 실행 컨텍스트 (`SagaId` 슬롯을 포함할 수 있음).
     * @returns 성공 시 최종 출력값, 실패 시 에러를 담은 `Task<Result<T>>`.
     */
    Task<Result<T>> run(T input, Context ctx = {}) {
        // 각 단계에 전달된 입력값을 기록해 보상 시 사용합니다.
        std::vector<T> step_inputs;
        step_inputs.reserve(steps_.size());

        T current = std::move(input);

        for (size_t i = 0; i < steps_.size(); ++i) {
            step_inputs.push_back(current);

            Result<T> result = co_await steps_[i].execute(current);

            if (!result.has_value()) {
                // 단계 i 실패 — i-1부터 0까지 역순 보상 수행
                co_await compensate_steps_(step_inputs, i);
                co_return result;
            }

            current = std::move(result.value());
        }

        co_return current;
    }

    /**
     * @brief 보상 실패 목록을 반환합니다.
     *
     * 보상 함수 실행 중 예외가 발생한 경우 해당 단계 이름이
     * 이 목록에 기록됩니다 (데드 레터 큐 역할).
     *
     * @returns 보상 실패가 발생한 단계 이름 목록.
     */
    const std::vector<std::string>& compensation_failures() const {
        return compensation_failures_;
    }

private:
    /** @brief 등록된 Saga 단계 목록. */
    std::vector<SagaStep<T, T>> steps_;

    /**
     * @brief 보상 실패 기록 (데드 레터 큐).
     *
     * 보상 함수가 실패한 단계의 이름이 저장됩니다.
     * 보상 실패가 발생해도 후속 보상 단계는 계속 실행됩니다.
     */
    std::vector<std::string> compensation_failures_;

    /**
     * @brief 지정한 범위의 단계를 역순으로 보상합니다.
     *
     * 단계 `failed_idx - 1`부터 0까지 각 단계의 `compensate`를 호출합니다.
     * 보상 중 예외가 발생하면 `compensation_failures_`에 단계 이름을 기록하고
     * 나머지 보상 단계를 계속 실행합니다.
     *
     * @param step_inputs 각 단계가 받았던 원본 입력값 목록.
     * @param failed_idx  실패한 단계 인덱스 (이 단계는 보상하지 않음).
     */
    Task<void> compensate_steps_(const std::vector<T>& step_inputs, size_t failed_idx) {
        // failed_idx 단계는 execute가 실패했으므로 보상 불필요.
        // 그 이전 단계(failed_idx - 1 ~ 0)를 역순으로 보상합니다.
        for (size_t j = failed_idx; j-- > 0; ) {
            try {
                co_await steps_[j].compensate(step_inputs[j]);
            } catch (...) {
                // 보상 실패를 데드 레터 큐에 기록하고 계속 진행합니다.
                compensation_failures_.push_back(steps_[j].name);
            }
        }
        co_return;
    }
};

} // namespace qbuem

/** @} */
