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
                // Step i failed — perform reverse compensation from i-1 down to 0
                co_await compensate_steps_(step_inputs, i);
                co_return result;
            }

            current = std::move(result.value());
        }

        co_return current;
    }

    /**
     * @brief Returns the list of compensation failures.
     *
     * If an exception occurs during compensation, the name of the failed step
     * is recorded in this list (acting as a dead-letter queue).
     *
     * @returns List of step names where compensation failed.
     */
    const std::vector<std::string>& compensation_failures() const {
        return compensation_failures_;
    }

private:
    /** @brief List of registered Saga steps. */
    std::vector<SagaStep<T, T>> steps_;

    /**
     * @brief Compensation failure record (dead-letter queue).
     *
     * Stores the names of steps whose compensation function failed.
     * Subsequent compensation steps continue to execute even when a failure occurs.
     */
    std::vector<std::string> compensation_failures_;

    /**
     * @brief Compensates the specified range of steps in reverse order.
     *
     * Calls `compensate` on each step from `failed_idx - 1` down to 0.
     * If an exception occurs during compensation, the step name is recorded in
     * `compensation_failures_` and the remaining compensation steps continue.
     *
     * @param step_inputs List of original input values received by each step.
     * @param failed_idx  Index of the failed step (this step is not compensated).
     */
    Task<void> compensate_steps_(const std::vector<T>& step_inputs, size_t failed_idx) {
        // The step at failed_idx does not need compensation since its execute failed.
        // Compensate the preceding steps (failed_idx - 1 down to 0) in reverse order.
        for (size_t j = failed_idx; j-- > 0; ) {
            try {
                co_await steps_[j].compensate(step_inputs[j]);
            } catch (...) {
                // Record the compensation failure in the dead-letter queue and continue.
                compensation_failures_.push_back(steps_[j].name);
            }
        }
        co_return;
    }
};

} // namespace qbuem

/** @} */
