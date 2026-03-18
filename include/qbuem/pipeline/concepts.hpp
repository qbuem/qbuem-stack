#pragma once

/**
 * @file qbuem/pipeline/concepts.hpp
 * @brief C++20 Concepts for pipeline type safety
 * @defgroup qbuem_pipeline_concepts Concepts
 * @ingroup qbuem_pipeline
 *
 * Validates Action function signatures at compile time.
 * An invalid signature produces a clear compile error.
 *
 * ## Action function signature forms
 * 1. **FullActionFn**: `Task<Result<Out>>(In, ActionEnv)` — includes context + cancellation signal
 * 2. **SimpleActionFn**: `Task<Result<Out>>(In, std::stop_token)` — cancellation signal only
 * 3. **PlainActionFn**: `Task<Result<Out>>(In)` — minimal signature
 * 4. **BatchActionFn**: `Task<Result<void>>(std::span<In>, std::span<Out>, ActionEnv)`
 *
 * `ActionFn<Fn,In,Out>` = FullActionFn ∨ SimpleActionFn ∨ PlainActionFn
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>

#include <concepts>
#include <span>
#include <stop_token>
#include <type_traits>

namespace qbuem {

// ---------------------------------------------------------------------------
// Action function concepts
// ---------------------------------------------------------------------------

/**
 * @brief Full Action signature validation: `Task<Result<Out>>(In, ActionEnv)`.
 *
 * Provides access to context (TraceCtx, RequestId, etc.), cancellation signal, and worker index.
 */
template <typename Fn, typename In, typename Out>
concept FullActionFn =
    requires(Fn fn, In in, ActionEnv env) {
      { fn(std::move(in), env) } -> std::same_as<Task<Result<Out>>>;
    };

/**
 * @brief Simple Action signature validation: `Task<Result<Out>>(In, std::stop_token)`.
 *
 * For cases where only the cancellation signal is needed and context is not required.
 */
template <typename Fn, typename In, typename Out>
concept SimpleActionFn =
    requires(Fn fn, In in, std::stop_token stop) {
      { fn(std::move(in), stop) } -> std::same_as<Task<Result<Out>>>;
    };

/**
 * @brief Plain Action signature validation: `Task<Result<Out>>(In)`.
 *
 * A pure transformation function that requires neither cancellation signal nor context.
 */
template <typename Fn, typename In, typename Out>
concept PlainActionFn =
    requires(Fn fn, In in) {
      { fn(std::move(in)) } -> std::same_as<Task<Result<Out>>>;
    };

/**
 * @brief Action function signature validation — one of three forms.
 *
 * `FullActionFn ∨ SimpleActionFn ∨ PlainActionFn`
 */
template <typename Fn, typename In, typename Out>
concept ActionFn = FullActionFn<Fn, In, Out> ||
                   SimpleActionFn<Fn, In, Out> ||
                   PlainActionFn<Fn, In, Out>;

/**
 * @brief Batch Action signature validation.
 *
 * Signature: `Task<Result<void>>(std::span<In>, std::span<Out>, ActionEnv)`
 *
 * The size of `span<Out>` must be equal to or greater than the size of `span<In>`.
 * Used for DB bulk inserts, batch ML inference, etc.
 */
template <typename Fn, typename In, typename Out>
concept BatchActionFn =
    requires(Fn fn, std::span<In> in, std::span<Out> out, ActionEnv env) {
      { fn(in, out, env) } -> std::same_as<Task<Result<void>>>;
    };

// ---------------------------------------------------------------------------
// Pipeline input concept
// ---------------------------------------------------------------------------

/**
 * @brief Pipeline input interface validation.
 *
 * Ensures the type has a `push(In)` method.
 * Used to verify type safety when connecting fan-out pipelines.
 */
template <typename Pipeline, typename In>
concept PipelineInputFor =
    requires(Pipeline &p, In item) {
      { p.push(std::move(item)) } -> std::same_as<Task<Result<void>>>;
    };

// ---------------------------------------------------------------------------
// Adapter: wrap ActionFn as FullActionFn
// ---------------------------------------------------------------------------

/**
 * @brief Convert a `SimpleActionFn` or `PlainActionFn` to a `FullActionFn`.
 *
 * Used internally to process all Action functions under a unified signature.
 *
 * @tparam Fn  Original function type.
 * @tparam In  Input type.
 * @tparam Out Output type.
 * @param  fn  Original function.
 * @returns A wrapper lambda with the `Task<Result<Out>>(In, ActionEnv)` signature.
 */
template <typename Fn, typename In, typename Out>
  requires ActionFn<Fn, In, Out>
auto to_full_action_fn(Fn fn) {
  if constexpr (FullActionFn<Fn, In, Out>) {
    return fn; // Already Full signature — return as-is
  } else if constexpr (SimpleActionFn<Fn, In, Out>) {
    return [fn = std::move(fn)](In in, ActionEnv env) mutable
               -> Task<Result<Out>> {
      return fn(std::move(in), env.stop);
    };
  } else { // PlainActionFn
    return [fn = std::move(fn)](In in, ActionEnv) mutable
               -> Task<Result<Out>> {
      return fn(std::move(in));
    };
  }
}

} // namespace qbuem

/** @} */
