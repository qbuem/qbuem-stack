#pragma once

/**
 * @file qbuem/pipeline/hot_swap.hpp
 * @brief Hot-swap mixin for DynamicPipeline — HotSwapMixin
 * @defgroup qbuem_hot_swap HotSwap
 * @ingroup qbuem_pipeline
 *
 * HotSwapMixin adds live action replacement to DynamicPipeline via the
 * Seal -> Drain -> Swap -> Resume procedure.
 *
 * ## Hot-swap procedure
 * 1. **Seal**  — stop new items from entering the target stage.
 * 2. **Drain** — wait up to `drain_timeout_ms` for in-flight items to finish.
 * 3. **Swap**  — atomically install the new action.
 * 4. **Resume** — re-open the stage input channel.
 *
 * ## Error codes returned
 * - `errc::operation_not_permitted` — pipeline is not in Running state.
 * - `errc::timed_out`               — drain phase exceeded timeout.
 * - `errc::invalid_argument`        — input/output schema mismatch.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace qbuem {

// ─── HotSwapError ────────────────────────────────────────────────────────────

/**
 * @brief Enumeration of hot-swap failure reasons.
 */
enum class HotSwapError {
  NotRunning,         ///< Pipeline is not in Running state.
  SchemaIncompatible, ///< Input/output types of the replacement do not match.
  TimedOut,           ///< Drain phase exceeded the configured timeout.
};

// ─── HotSwapConfig ───────────────────────────────────────────────────────────

/**
 * @brief Drain timeout policy — controls what happens to in-flight items when drain times out.
 */
enum class DrainTimeoutPolicy {
  /**
   * @brief Move in-flight items to a Dead Letter Queue (DLQ).
   *
   * Items are preserved for inspection/retry. Requires the concrete pipeline to
   * implement DLQ support. This is the default and safest option.
   */
  Dlq,

  /**
   * @brief Silently drop in-flight items.
   *
   * Fast but items are permanently lost. Use only when data loss is acceptable
   * (e.g., metrics aggregation, non-critical events).
   */
  Drop,

  /**
   * @brief Resume processing with the new action without draining.
   *
   * In-flight items in the old action's channel continue to be processed by the
   * new action implementation. Use when the new action is fully backward-compatible.
   */
  Resume,
};

/**
 * @brief Configuration for a single hot-swap operation.
 */
struct HotSwapConfig {
  std::string        action_name;              ///< Name of the action stage to replace.
  uint64_t           drain_timeout_ms = 5000;  ///< Maximum time (ms) to wait for in-flight items.
  DrainTimeoutPolicy timeout_policy   = DrainTimeoutPolicy::Dlq; ///< Timeout behavior.
};

// ─── HotSwapMixin ─────────────────────────────────────────────────────────────

/**
 * @brief Mixin that adds hot-swap capability to DynamicPipeline.
 *
 * Intended to be composed into (or used as a base class of) DynamicPipeline.
 * The concrete pipeline type is responsible for implementing the actual
 * channel plumbing; this mixin provides the public API and audit log.
 *
 * ### Thread safety
 * All public methods are safe to call concurrently.
 * The swap_history_ vector is append-only after construction and is
 * protected by the pipeline's own stage mutex when accessed here.
 */
class HotSwapMixin {
public:
  // -------------------------------------------------------------------------
  // Audit record
  // -------------------------------------------------------------------------

  /**
   * @brief Record of a single hot-swap attempt.
   */
  struct SwapRecord {
    std::string                             action_name; ///< Target stage name.
    std::chrono::system_clock::time_point   when;        ///< Wall-clock timestamp.
    bool                                    success;     ///< Whether the swap succeeded.
  };

  // -------------------------------------------------------------------------
  // Hot-swap API
  // -------------------------------------------------------------------------

  /**
   * @brief Replace a running action stage with a new one.
   *
   * Follows the Seal -> Drain -> Swap -> Resume procedure.
   *
   * @tparam In  Input type of the stage being replaced.
   * @tparam Out Output type of the stage being replaced.
   * @param action_name       Name of the stage to replace.
   * @param new_action        Replacement action.
   * @param drain_timeout_ms  Maximum time (ms) to wait for in-flight items.
   *
   * @returns `Result<void>::ok()` on success, or one of:
   *   - `errc::operation_not_permitted` — not Running.
   *   - `errc::timed_out`              — drain exceeded timeout.
   *   - `errc::invalid_argument`       — schema mismatch.
   */
  template <typename In, typename Out>
  Task<Result<void>> hot_swap(std::string_view action_name,
                               Action<In, Out>  new_action,
                               uint64_t         drain_timeout_ms = 5000) {
    // Record attempt (success determined at end)
    SwapRecord rec{
        .action_name = std::string(action_name),
        .when        = std::chrono::system_clock::now(),
        .success     = false,
    };

    auto result = co_await do_hot_swap(action_name, std::move(new_action),
                                       drain_timeout_ms);

    rec.success = result.has_value();
    swap_history_.push_back(std::move(rec));

    co_return result;
  }

  /**
   * @brief Return the complete hot-swap audit history.
   *
   * The returned reference is valid for the lifetime of the mixin.
   */
  const std::vector<SwapRecord>& swap_history() const noexcept {
    return swap_history_;
  }

  /**
   * @brief Roll back the most recent successful swap for the named action.
   *
   * Locates the last successful swap record for `action_name` and re-applies
   * the preceding action (if a prior version was captured).  If no prior
   * version is available the call returns `errc::no_such_process`.
   *
   * @param action_name  Name of the action stage to roll back.
   * @returns `Result<void>::ok()` on success, or an error code.
   */
  Task<Result<void>> rollback(std::string_view action_name) {
    // Walk history in reverse to find the most recent successful swap.
    // The concrete subclass stores previous action versions alongside the
    // history; here we delegate to do_rollback().
    co_return co_await do_rollback(action_name);
  }

protected:
  // -------------------------------------------------------------------------
  // Extension points (override in concrete pipeline)
  // -------------------------------------------------------------------------

  /**
   * @brief Perform the actual Seal -> Drain -> Swap -> Resume procedure.
   *
   * Must be overridden by the concrete pipeline type that embeds this mixin.
   * The default implementation returns `errc::operation_not_permitted`.
   */
  template <typename In, typename Out>
  virtual Task<Result<void>> do_hot_swap(std::string_view /*action_name*/,
                                          Action<In, Out>  /*new_action*/,
                                          uint64_t         /*drain_timeout_ms*/) {
    co_return unexpected(
        std::make_error_code(std::errc::operation_not_permitted));
  }

  /**
   * @brief Perform the rollback to the previous action version.
   *
   * Must be overridden by the concrete pipeline type.
   * The default implementation returns `errc::no_such_process`.
   */
  virtual Task<Result<void>> do_rollback(std::string_view /*action_name*/) {
    co_return unexpected(
        std::make_error_code(std::errc::no_such_process));
  }

  // -------------------------------------------------------------------------
  // State
  // -------------------------------------------------------------------------

  /** @brief Append-only log of all swap attempts. */
  std::vector<SwapRecord> swap_history_;
};

} // namespace qbuem

/** @} */
