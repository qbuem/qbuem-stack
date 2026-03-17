#pragma once

/**
 * @file qbuem/pipeline/static_pipeline.hpp
 * @brief 정적 파이프라인 — StaticPipeline<In, Out>
 * @defgroup qbuem_static_pipeline StaticPipeline
 * @ingroup qbuem_pipeline
 *
 * StaticPipeline은 컴파일 타임에 타입 체인이 결정되는 파이프라인입니다.
 *
 * ## 빌드 방식
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
 * ## 파이프라인 상태 머신
 * Created → Built → Starting → Running → Draining → Stopped
 *
 * ## 타입 안전성
 * 각 `add<Out>(action)`에서 `In` = 이전 단계의 `Out`.
 * 불일치 시 컴파일 에러 발생.
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
 * @brief 파이프라인 입력 타입 소거 인터페이스 (fan-out용).
 *
 * @tparam T 입력 타입.
 */
template <typename T>
class IPipelineInput {
public:
  virtual ~IPipelineInput() = default;
  virtual Task<Result<void>> push(T item, Context ctx = {}) = 0;
  virtual bool try_push(T item, Context ctx = {}) = 0;
};

/**
 * @brief 정적 파이프라인 — 컴파일 타임 타입 체인.
 *
 * @tparam In  파이프라인 입력 타입.
 * @tparam Out 파이프라인 출력 타입 (마지막 Action의 출력).
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
   * @brief 파이프라인을 시작합니다.
   *
   * @param dispatcher 워커를 실행할 Dispatcher.
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
   * @brief 입력을 닫고 모든 워커가 처리를 마칠 때까지 기다립니다.
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
   * @brief 파이프라인을 즉시 정지합니다 (drain 없음).
   *
   * stoppers 호출에 더해 head/tail 채널을 직접 닫아
   * source_pump, pump_channels, sink_pump 등 모든 펌프 코루틴이
   * 같은 리액터 poll 사이클에서 종료 신호를 받도록 보장합니다.
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
  // 입력
  // -------------------------------------------------------------------------

  /**
   * @brief 아이템을 파이프라인에 넣습니다 (backpressure).
   */
  Task<Result<void>> push(In item, Context ctx = {}) override {
    co_return co_await internal_.head_channel->send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief 논블로킹 push.
   */
  bool try_push(In item, Context ctx = {}) override {
    return internal_.head_channel->try_send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  // -------------------------------------------------------------------------
  // 출력 채널
  // -------------------------------------------------------------------------

  /**
   * @brief 출력 채널을 반환합니다 (소비자 연결용).
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
    return internal_.tail_channel;
  }

  /**
   * @brief 현재 파이프라인 상태를 반환합니다.
   */
  [[nodiscard]] State state() const noexcept { return state_.load(); }

private:
  Internal internal_;
  std::atomic<State> state_{State::Created};
};

// ---------------------------------------------------------------------------
// PipelineBuilder — 컴파일 타임 타입 체인 구성
// ---------------------------------------------------------------------------

/**
 * @brief 정적 파이프라인 빌더.
 *
 * `add<Out>(action)` 호출마다 새 `PipelineBuilder<OrigIn, Out>` 타입 반환.
 * 타입 불일치는 컴파일 에러.
 *
 * @tparam OrigIn 파이프라인 원본 입력 타입.
 * @tparam CurOut 현재 단계의 출력 타입 (= 다음 단계 입력).
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
  // Source / Sink 연결
  // -------------------------------------------------------------------------

  /**
   * @brief 외부 소스(SHMSource, MessageBusSource 등)를 Pipeline Head에 연결합니다.
   *
   * with_source()를 add() 호출 전에 사용합니다. 소스가 attach되면
   * Pipeline의 push()도 여전히 동작하지만 소스 펌프와 채널을 공유합니다.
   * 소스만 사용하고 싶다면 push()를 호출하지 마세요.
   *
   * @tparam SourceT  `init() -> Result<void>` 와
   *                  `next() -> Task<std::optional<const CurOut*>>`를 갖는 타입.
   * @param  src      소스 인스턴스 (이동됨).
   * @param  cap      소스 → 파이프라인 내부 채널 용량 (기본 256).
   * @returns 현재 빌더 (체이닝 가능).
   */
  template <typename SourceT>
  PipelineBuilder<OrigIn, CurOut> with_source(SourceT src, size_t cap = 256) {
    auto src_ptr = std::make_shared<SourceT>(std::move(src));

    // Source 전용 채널: 소스 펌프 → 이 채널 → 첫 번째 add() 액션
    auto src_ch =
        std::make_shared<AsyncChannel<ContextualItem<CurOut>>>(cap);

    head_ = src_ch;  // pipeline.push() 진입점 (소스와 공유)
    tail_ = src_ch;  // 첫 번째 add()가 이 채널을 입력으로 wiring

    starters_.push_back([src_ptr, src_ch](Dispatcher& d) mutable {
      auto init_res = src_ptr->init();
      if (!init_res) return;  // init 실패 시 무음 처리

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
   * @brief 외부 싱크(SHMSink, MessageBusSink 등)를 Pipeline Tail에 연결합니다.
   *
   * with_sink()는 add() 체인 뒤, build() 전에 호출합니다.
   * 싱크가 attach되면 Pipeline의 output() 채널도 여전히 접근 가능합니다.
   *
   * @tparam SinkT  `init() -> Result<void>` 와
   *                `sink(const CurOut&) -> Task<Result<void>>`를 갖는 타입.
   * @param  snk    싱크 인스턴스 (이동됨).
   * @returns 현재 빌더 (체이닝 가능).
   */
  template <typename SinkT>
  PipelineBuilder<OrigIn, CurOut> with_sink(SinkT snk) {
    auto snk_ptr  = std::make_shared<SinkT>(std::move(snk));
    auto drain_ch = tail_;  // 현재 tail (마지막 액션 출력 채널)

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
   * @brief 새 처리 단계를 추가합니다.
   *
   * @tparam NextOut 이 단계의 출력 타입.
   * @tparam FnT     `ActionFn<FnT, CurOut, NextOut>` concept을 만족하는 타입.
   * @param  fn      처리 함수.
   * @param  cfg     Action 설정.
   * @returns `PipelineBuilder<OrigIn, NextOut>` — 다음 단계 빌더.
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
   * @brief 파이프라인을 완성합니다.
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
 * @brief 첫 Action 추가 시 OrigIn을 자동 추론하는 헬퍼.
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
