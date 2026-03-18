#pragma once

/**
 * @file qbuem/pipeline/saga.hpp
 * @brief Saga pattern — SagaStep, SagaOrchestrator
 * @defgroup qbuem_saga Saga
 * @ingroup qbuem_pipeline
 *
 * The Saga pattern decomposes a distributed transaction into sequential steps,
 * and on intermediate failure compensates already-completed steps in reverse order.
 *
 * ## Behavior overview
 * 1. `SagaOrchestrator::run(input)` executes registered steps in order.
 * 2. If step N fails, compensation functions are called in reverse order: N-1, N-2, ... 0.
 * 3. Compensation failures are recorded in `compensation_failures_` without raising exceptions.
 *
 * ## Usage example
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
 * @brief A single execution step in a Saga distributed transaction.
 *
 * Each step consists of an `execute` function and a `compensate` function.
 * If `execute` succeeds, execution proceeds to the next step.
 * If a subsequent step fails, `compensate` is called in reverse order.
 *
 * @tparam In  Step input type.
 * @tparam Out Step output type.
 */
template <typename In, typename Out>
struct SagaStep {
    /** @brief Step name (used in logging and error messages). */
    std::string name;

    /**
     * @brief Step execution function.
     *
     * On success, returns a value wrapped in `Result<Out>`.
     * On failure, returns an error wrapped in `Result<Out>`.
     *
     * @param input Step input value.
     */
    std::function<Task<Result<Out>>(In)> execute;

    /**
     * @brief Compensation (rollback) function.
     *
     * Called during rollback caused by a failure in a subsequent step.
     * Receives the original input value and undoes the step's side effects.
     *
     * @param input Original input value that the step processed.
     */
    std::function<Task<void>(In)> compensate;
};

/**
 * @brief Orchestrator that executes Saga steps sequentially and performs reverse compensation on failure.
 *
 * ### Execution flow
 * ```
 * step[0].execute → step[1].execute → ... → step[N].execute
 *                                              ↓ failure
 * step[N-1].compensate ← ... ← step[1].compensate ← step[0].compensate
 * ```
 *
 * ### Compensation failure handling
 * Even if a compensation function throws, the orchestrator does not stop;
 * it continues executing the remaining compensation steps.
 * Compensation failures can be queried via `compensation_failures()`.
 *
 * @tparam T Pipeline item type (input/output type for each step).
 */
template <typename T>
class SagaOrchestrator {
public:
    /**
     * @brief Registers steps in order.
     *
     * The registration order determines the execution order.
     * Compensation is performed in reverse order.
     *
     * @param step Saga step to register.
     * @returns `*this` reference for method chaining.
     */
    SagaOrchestrator& add_step(SagaStep<T, T> step) {
        steps_.push_back(std::move(step));
        return *this;
    }

    /**
     * @brief Executes all registered steps sequentially.
     *
     * If step N fails, compensation functions are called in reverse from step N-1 to 0.
     * If all steps succeed, the final step's output value is returned.
     *
     * @param input Initial input value to pass to the first step.
     * @param ctx   Saga execution context (may include a `SagaId` slot).
     * @returns `Task<Result<T>>` with the final output on success, or an error on failure.
     */
    Task<Result<T>> run(T input, Context ctx = {}) {
        // Record the input passed to each step for use during compensation.
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
