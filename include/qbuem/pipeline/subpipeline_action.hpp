#pragma once

/**
 * @file qbuem/pipeline/subpipeline_action.hpp
 * @brief SubpipelineAction — embed a StaticPipeline as an Action<In, Out>
 * @defgroup qbuem_subpipeline_action SubpipelineAction
 * @ingroup qbuem_pipeline
 *
 * SubpipelineAction<In, Out> wraps a fully-built StaticPipeline<In, Out>
 * so that it can be used anywhere an Action<In, Out> processing function
 * is accepted.  This enables pipeline reuse and hierarchical composition.
 *
 * ## Usage
 * ```cpp
 * // Build an inner pipeline
 * auto inner = pipeline_builder<RawEvent>()
 *     .add<NormEvent>(normalize_fn)
 *     .add<RichEvent>(enrich_fn)
 *     .build();
 *
 * // Wrap it as an action and add to an outer pipeline
 * SubpipelineAction<RawEvent, RichEvent> sub(std::move(inner));
 *
 * DynamicPipeline<...> outer;
 * outer.add_stage("enrich", std::ref(sub));
 * ```
 * @{
 */

#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>

namespace qbuem {

/**
 * @brief Wraps a StaticPipeline<In, Out> as a callable satisfying the
 *        Action<In, Out> processing-function contract.
 *
 * The inner pipeline must have been started (`start()`) before the first
 * call to `operator()`, or the call will block indefinitely waiting for a
 * worker that never exists.  A common pattern is to start the inner pipeline
 * from the same Dispatcher as the outer pipeline.
 *
 * ### Ownership
 * SubpipelineAction owns the inner StaticPipeline by value.
 * Move the SubpipelineAction (or hold it behind a shared_ptr) to avoid
 * copies of the pipeline, which is non-copyable.
 *
 * @tparam In  Input item type (must match StaticPipeline<In, Out>).
 * @tparam Out Output item type (must match StaticPipeline<In, Out>).
 */
template <typename In, typename Out>
class SubpipelineAction {
public:
  /**
   * @brief Construct from a fully-built StaticPipeline.
   *
   * @param pipeline  The inner pipeline to embed.  Moved into this object.
   */
  explicit SubpipelineAction(StaticPipeline<In, Out> pipeline)
      : pipeline_(std::move(pipeline)) {}

  SubpipelineAction(const SubpipelineAction&) = delete;
  SubpipelineAction& operator=(const SubpipelineAction&) = delete;
  SubpipelineAction(SubpipelineAction&&) = default;
  SubpipelineAction& operator=(SubpipelineAction&&) = default;

  // -------------------------------------------------------------------------
  // Action contract
  // -------------------------------------------------------------------------

  /**
   * @brief Process a single item through the inner pipeline.
   *
   * Pushes `item` into the inner pipeline and waits for one output item.
   * The `env` argument is forwarded (stop token, registry, context) for
   * cancellation and observability.
   *
   * @param item  Input item to process.
   * @param env   Execution environment provided by the outer pipeline worker.
   * @returns Processed output item, or an error if:
   *   - The inner pipeline input channel is closed (`errc::broken_pipe`).
   *   - The inner pipeline output channel is closed before emitting a result
   *     (`errc::no_message_available`).
   */
  Task<Result<Out>> operator()(In item, ActionEnv env) {
    // Forward the item into the inner pipeline.
    auto push_result = co_await pipeline_.push(std::move(item), env.ctx);
    if (!push_result) {
      co_return unexpected(push_result.error());
    }

    // Collect exactly one output item from the inner pipeline tail channel.
    auto out_channel = pipeline_.output();
    if (!out_channel) {
      co_return unexpected(
          std::make_error_code(std::errc::no_message_available));
    }

    auto citem = co_await out_channel->recv();
    if (!citem) {
      co_return unexpected(
          std::make_error_code(std::errc::no_message_available));
    }

    co_return std::move(citem->value);
  }

  // -------------------------------------------------------------------------
  // Inner pipeline access
  // -------------------------------------------------------------------------

  /**
   * @brief Access the inner pipeline for metrics, state queries, or control.
   */
  StaticPipeline<In, Out>& inner() noexcept { return pipeline_; }

  /**
   * @brief Const access to the inner pipeline.
   */
  const StaticPipeline<In, Out>& inner() const noexcept { return pipeline_; }

private:
  StaticPipeline<In, Out> pipeline_;
};

} // namespace qbuem

/** @} */
