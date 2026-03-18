#pragma once

/**
 * @file qbuem/pipeline/static_pipeline.hpp
 * @brief Static pipeline — StaticPipeline<In, Out>
 * @defgroup qbuem_static_pipeline StaticPipeline
 * @ingroup qbuem_pipeline
 *
 * StaticPipeline is a pipeline whose type chain is determined at compile time.
 *
 * ## Build pattern
 * ```cpp
 * auto pipeline = PipelineBuilder<RawEvent>{}
 *     .add<ParsedEvent>(parse_action)
 *     .add<EnrichedEvent>(enrich_action)
 *     .add<StoredEvent>(store_action)
 *     .build();
 *
 * pipeline.start(dispatcher);
 * co_await pipeline.push(raw_event);
 * ```
 *
 * ## Pipeline state machine
 * Created → Built → Starting → Running → Draining → Stopped
 *
 * ## Type safety
 * For each `add<Out>(action)`, `In` = the `Out` of the previous stage.
 * A mismatch results in a compile error.
 * @{
 */

#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/concepts.hpp>
#include <qbuem/pipeline/context.hpp>

#include <atomic>
#include <memory>
#include <tuple>
#include <vector>

namespace qbuem {

// Forward declaration
template <typename OrigIn, typename CurIn>
class PipelineBuilder;

/**
 * @brief Type-erased pipeline input interface (for fan-out).
 *
 * @tparam T Input type.
 */
template <typename T>
class IPipelineInput {
public:
  virtual ~IPipelineInput() = default;
  virtual Task<Result<void>> push(T item, Context ctx = {}) = 0;
  virtual bool try_push(T item, Context ctx = {}) = 0;
};

/**
 * @brief Static pipeline — compile-time type chain.
 *
 * @tparam In  Pipeline input type.
 * @tparam Out Pipeline output type (output of the last Action).
 */
template <typename In, typename Out>
class StaticPipeline : public IPipelineInput<In> {
public:
  enum class State {
    Created,
    Starting,
    Running,
    Draining,
    Stopped,
  };

  // Internal — constructed by PipelineBuilder
  struct Internal {
    std::shared_ptr<AsyncChannel<ContextualItem<In>>> head_channel;
    std::shared_ptr<AsyncChannel<ContextualItem<Out>>> tail_channel;
    // Lifecycle callbacks from all actions
    std::vector<std::function<void(Dispatcher &)>> starters;
    std::vector<std::function<Task<void>()>>       drainers;
    std::vector<std::function<void()>>             stoppers;
  };

  explicit StaticPipeline(Internal internal)
      : internal_(std::move(internal)) {}

  // std::atomic<State> deletes the implicit move constructor; define it explicitly.
  StaticPipeline(StaticPipeline&& other) noexcept
      : internal_(std::move(other.internal_))
      , state_(other.state_.load(std::memory_order_relaxed)) {}

  StaticPipeline& operator=(StaticPipeline&& other) noexcept {
    if (this != &other) {
      internal_ = std::move(other.internal_);
      state_.store(other.state_.load(std::memory_order_relaxed),
                   std::memory_order_relaxed);
    }
    return *this;
  }

  // -------------------------------------------------------------------------
  // Lifecycle
  // -------------------------------------------------------------------------

  /**
   * @brief Start the pipeline.
   *
   * @param dispatcher Dispatcher that will run the workers.
   */
  void start(Dispatcher &dispatcher) {
    State expected = State::Created;
    if (!state_.compare_exchange_strong(expected, State::Starting))
      return;
    for (auto &s : internal_.starters)
      s(dispatcher);
    state_.store(State::Running);
  }

  /**
   * @brief Close the input and wait for all workers to finish processing.
   */
  Task<void> drain() {
    State expected = State::Running;
    if (!state_.compare_exchange_strong(expected, State::Draining))
      co_return;
    for (auto &d : internal_.drainers)
      co_await d();
    state_.store(State::Stopped);
    co_return;
  }

  /**
   * @brief Stop the pipeline immediately (without draining).
   *
   * In addition to calling the stoppers, directly closes the head/tail channels
   * so that all pump coroutines (source_pump, pump_channels, sink_pump) receive
   * their termination signal within the same reactor poll cycle.
   */
  void stop() {
    state_.store(State::Stopped);
    for (auto &s : internal_.stoppers)
      s();
    // Close head and tail channels so all pump coroutines (pump_channels,
    // sink_pump) are woken in the same reactor poll cycle as the action
    // stoppers, preventing coroutine-frame leaks when the dispatcher stops.
    // When In == Out and with_source is used, head and tail may refer to the
    // same channel; closing it twice is harmless (close() is idempotent).
    if (internal_.head_channel)
      internal_.head_channel->close();
    if (internal_.tail_channel)
      internal_.tail_channel->close();
  }

  // -------------------------------------------------------------------------
  // Input
  // -------------------------------------------------------------------------

  /**
   * @brief Push an item into the pipeline (with backpressure).
   */
  Task<Result<void>> push(In item, Context ctx = {}) override {
    co_return co_await internal_.head_channel->send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief Non-blocking push.
   */
  bool try_push(In item, Context ctx = {}) override {
    return internal_.head_channel->try_send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  // -------------------------------------------------------------------------
  // Output channel
  // -------------------------------------------------------------------------

  /**
   * @brief Return the output channel (for connecting a downstream consumer).
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
    return internal_.tail_channel;
  }

  /**
   * @brief Return the current pipeline state.
   */
  [[nodiscard]] State state() const noexcept { return state_.load(); }

private:
  Internal internal_;
  std::atomic<State> state_{State::Created};
};

// ---------------------------------------------------------------------------
// PipelineBuilder — compile-time type chain construction
// ---------------------------------------------------------------------------

/**
 * @brief Static pipeline builder.
 *
 * Each `add<Out>(action)` call returns a new `PipelineBuilder<OrigIn, Out>` type.
 * Type mismatches result in compile errors.
 *
 * @tparam OrigIn Original input type of the pipeline.
 * @tparam CurOut Output type of the current stage (= input of the next stage).
 */
template <typename OrigIn, typename CurOut = OrigIn>
class PipelineBuilder {
public:
  using Config = typename Action<CurOut, CurOut>::Config; // placeholder

  PipelineBuilder() = default;

  // Internal constructor used by add()
  explicit PipelineBuilder(
      std::shared_ptr<AsyncChannel<ContextualItem<OrigIn>>> head,
      std::shared_ptr<AsyncChannel<ContextualItem<CurOut>>> tail,
      std::vector<std::function<void(Dispatcher &)>> starters,
      std::vector<std::function<Task<void>()>>       drainers,
      std::vector<std::function<void()>>             stoppers)
      : head_(std::move(head)),
        tail_(std::move(tail)),
        starters_(std::move(starters)),
        drainers_(std::move(drainers)),
        stoppers_(std::move(stoppers)) {}

  // -------------------------------------------------------------------------
  // Source / Sink wiring
  // -------------------------------------------------------------------------

  /**
   * @brief Attach an external source (SHMSource, MessageBusSource, etc.) to the pipeline head.
   *
   * Call with_source() before any add() calls. Once a source is attached,
   * the pipeline's push() still works but shares the channel with the source pump.
   * If you only want to use the source, do not call push().
   *
   * @tparam SourceT  Type that has `init() -> Result<void>` and
   *                  `next() -> Task<std::optional<const CurOut*>>`.
   * @param  src      Source instance (moved in).
   * @param  cap      Capacity of the source → pipeline internal channel (default 256).
   * @returns Current builder (chainable).
   */
  template <typename SourceT>
  PipelineBuilder<OrigIn, CurOut> with_source(SourceT src, size_t cap = 256) {
    auto src_ptr = std::make_shared<SourceT>(std::move(src));

    // Source-dedicated channel: source pump → this channel → first add() action
    auto src_ch =
        std::make_shared<AsyncChannel<ContextualItem<CurOut>>>(cap);

    head_ = src_ch;  // pipeline.push() entry point (shared with source)
    tail_ = src_ch;  // first add() wires this channel as its input

    starters_.push_back([src_ptr, src_ch](Dispatcher& d) mutable {
      auto init_res = src_ptr->init();
      if (!init_res) return;  // silent failure if init fails

      // Use a free-function-style coroutine (source_pump) instead of a lambda
      // coroutine to avoid GCC HALO stack-use-after-return:
      // lambda captures may be placed on the outer lambda's stack frame
      // (which is freed when the outer lambda returns), whereas function
      // parameters are always heap-allocated in the coroutine frame.
      d.spawn(PipelineBuilder::source_pump<CurOut, SourceT>(src_ptr, src_ch));
    });

    // Stopper: close the source (if it supports close()) so source_pump's
    // next() returns nullopt and the coroutine frame is freed when the
    // dispatcher stops.  The if constexpr guard lets source types that
    // don't have close() (e.g. SHMSource) work without compilation errors.
    if constexpr (requires { src_ptr->close(); }) {
      stoppers_.push_back([src_ptr]() { src_ptr->close(); });
    }

    return PipelineBuilder<OrigIn, CurOut>(head_, tail_,
                                           std::move(starters_),
                                           std::move(drainers_),
                                           std::move(stoppers_));
  }

  /**
   * @brief Attach an external sink (SHMSink, MessageBusSink, etc.) to the pipeline tail.
   *
   * Call with_sink() after the add() chain and before build().
   * Once a sink is attached, the pipeline's output() channel is still accessible.
   *
   * @tparam SinkT  Type that has `init() -> Result<void>` and
   *                `sink(const CurOut&) -> Task<Result<void>>`.
   * @param  snk    Sink instance (moved in).
   * @returns Current builder (chainable).
   */
  template <typename SinkT>
  PipelineBuilder<OrigIn, CurOut> with_sink(SinkT snk) {
    auto snk_ptr  = std::make_shared<SinkT>(std::move(snk));
    auto drain_ch = tail_;  // current tail (last action's output channel)

    starters_.push_back([snk_ptr, drain_ch](Dispatcher& d) mutable {
      auto init_res = snk_ptr->init();
      if (!init_res) return;

      // Use a free-function-style coroutine to avoid GCC stack-use-after-return
      // in lambda coroutines: function parameters are heap-allocated in the
      // coroutine frame, whereas lambda captures may be stack-allocated (HALO).
      d.spawn(PipelineBuilder::sink_pump<CurOut, SinkT>(drain_ch, snk_ptr));
    });

    return PipelineBuilder<OrigIn, CurOut>(head_, tail_,
                                           std::move(starters_),
                                           std::move(drainers_),
                                           std::move(stoppers_));
  }

  /**
   * @brief Add a new processing stage.
   *
   * @tparam NextOut Output type of this stage.
   * @tparam FnT     Type satisfying the `ActionFn<FnT, CurOut, NextOut>` concept.
   * @param  fn      Processing function.
   * @param  cfg     Action configuration.
   * @returns `PipelineBuilder<OrigIn, NextOut>` — builder for the next stage.
   */
  template <typename NextOut, typename FnT>
    requires ActionFn<FnT, CurOut, NextOut>
  PipelineBuilder<OrigIn, NextOut> add(FnT fn,
                                       typename Action<CurOut, NextOut>::Config cfg = {}) {
    // Create Action and connect tail → action input
    auto action_ptr =
        std::make_shared<Action<CurOut, NextOut>>(std::move(fn), cfg);

    // Wire: current tail channel → action's input channel
    if (tail_) {
      // Connect: pump from tail_ into action's in_channel
      // The pump coroutine is created lazily at start() time to avoid
      // storing a raw coroutine handle across the builder lifetime.
      auto src = tail_;
      auto dst = action_ptr->input();
      starters_.push_back([src, dst](Dispatcher &d) mutable {
        d.spawn(pump_channels<CurOut>(src, dst));
      });
    } else {
      // First action: head_ = action's in_channel.
      // Only valid when CurOut == OrigIn (i.e., no prior add() calls).
      if constexpr (std::is_same_v<CurOut, OrigIn>) {
        head_ = action_ptr->input();
      }
      // If CurOut != OrigIn and tail_ is null, the builder was constructed
      // incorrectly; head_ remains null and the pipeline is ill-formed.
    }

    auto next_tail = std::make_shared<AsyncChannel<ContextualItem<NextOut>>>(cfg.channel_cap);

    auto action_raw = action_ptr.get();
    starters_.push_back([action_raw, next_tail](Dispatcher &d) mutable {
      action_raw->start(d, next_tail);
    });

    auto action_for_drain = action_ptr;
    drainers_.push_back([action_for_drain]() -> Task<void> {
      return action_for_drain->drain();
    });

    auto action_for_stop = action_ptr;
    stoppers_.push_back([action_for_stop]() { action_for_stop->stop(); });

    // Keep action alive through shared_ptr captured in lambdas above
    return PipelineBuilder<OrigIn, NextOut>(head_, next_tail, std::move(starters_),
                                           std::move(drainers_), std::move(stoppers_));
  }

  /**
   * @brief Finalize and build the pipeline.
   *
   * @returns `StaticPipeline<OrigIn, CurOut>`.
   */
  [[nodiscard]] StaticPipeline<OrigIn, CurOut> build() {
    if (!head_) {
      // No actions added — trivial pass-through (only valid when OrigIn == CurOut)
      head_ = std::make_shared<AsyncChannel<ContextualItem<OrigIn>>>(256);
      if constexpr (std::is_same_v<OrigIn, CurOut>) {
        tail_ = head_;
      }
    }
    typename StaticPipeline<OrigIn, CurOut>::Internal internal{
        .head_channel = head_,
        .tail_channel = tail_,
        .starters     = std::move(starters_),
        .drainers     = std::move(drainers_),
        .stoppers     = std::move(stoppers_),
    };
    return StaticPipeline<OrigIn, CurOut>(std::move(internal));
  }

private:
  std::shared_ptr<AsyncChannel<ContextualItem<OrigIn>>> head_;
  std::shared_ptr<AsyncChannel<ContextualItem<CurOut>>> tail_;
  std::vector<std::function<void(Dispatcher &)>>        starters_;
  std::vector<std::function<Task<void>()>>              drainers_;
  std::vector<std::function<void()>>                    stoppers_;

  // Pump coroutine: forwards ContextualItem<T> between two channels.
  // If dst is closed (send returns broken_pipe), closes src so any
  // upstream source_pump is also notified and can terminate.
  template <typename T>
  static Task<void> pump_channels(
      std::shared_ptr<AsyncChannel<ContextualItem<T>>> src,
      std::shared_ptr<AsyncChannel<ContextualItem<T>>> dst) {
    for (;;) {
      auto item = co_await src->recv();
      if (!item) { dst->close(); co_return; }
      auto r = co_await dst->send(std::move(*item));
      if (!r) { src->close(); co_return; }  // dst closed → propagate upstream
    }
  }

  // Source pump coroutine: reads from SourceT and forwards into the pipeline
  // head channel.  Implemented as a free function (not a lambda coroutine)
  // so that parameters are heap-allocated in the coroutine frame —
  // avoids GCC HALO stack-use-after-return.
  template <typename T, typename SourceT>
  static Task<void> source_pump(
      std::shared_ptr<SourceT>                         src,
      std::shared_ptr<AsyncChannel<ContextualItem<T>>> ch) {
    for (;;) {
      auto opt = co_await src->next();
      if (!opt.has_value() || opt.value() == nullptr) {
        ch->close();
        co_return;
      }
      auto r = co_await ch->send(ContextualItem<T>{*opt.value(), {}});
      if (!r) co_return;
    }
  }

  // Sink pump coroutine: drains channel and forwards each value to SinkT::sink().
  // Implemented as a free function (not a lambda coroutine) so that parameters
  // are heap-allocated in the coroutine frame — avoids GCC HALO stack-use-after-return.
  template <typename T, typename SinkT>
  static Task<void> sink_pump(
      std::shared_ptr<AsyncChannel<ContextualItem<T>>> ch,
      std::shared_ptr<SinkT>                          snk) {
    for (;;) {
      auto item = co_await ch->recv();
      if (!item) co_return;
      co_await snk->sink(item->value);
    }
  }

};

// ---------------------------------------------------------------------------
// Deduction guide for PipelineBuilder
// ---------------------------------------------------------------------------

/**
 * @brief Helper that automatically deduces OrigIn when the first Action is added.
 *
 * @code
 * auto pipeline = pipeline_builder<RawEvent>()
 *     .add<ParsedEvent>(parse_fn)
 *     .build();
 * @endcode
 */
template <typename OrigIn>
PipelineBuilder<OrigIn, OrigIn> pipeline_builder() {
  return PipelineBuilder<OrigIn, OrigIn>{};
}

} // namespace qbuem

/** @} */
