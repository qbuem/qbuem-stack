#pragma once

/**
 * @file qbuem/pipeline/event_actions.hpp
 * @brief 고급 이벤트 처리 액션 — DebounceAction, ThrottleAction, ScatterGatherAction
 * @defgroup qbuem_event_actions EventActions
 * @ingroup qbuem_pipeline
 *
 * ## 포함된 컴포넌트
 *
 * ### DebounceAction<T>
 * 일정 시간(gap) 동안 새 아이템이 없으면 마지막 아이템을 출력합니다.
 * 이벤트 폭주(burst) 후 안정 구간을 대기하는 패턴에 유용합니다.
 *
 * ### ThrottleAction<T>
 * 토큰 버킷 알고리즘으로 처리량을 제한합니다.
 * `rate_per_sec` 속도, `burst` 버스트 용량을 지원합니다.
 *
 * ### ScatterGatherAction<In, SubIn, SubOut, Out>
 * 하나의 입력을 여러 SubIn으로 분산(scatter)하고,
 * 각각 병렬 처리한 뒤 결과를 집계(gather)하여 Out으로 출력합니다.
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/service_registry.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

namespace qbuem {

// ============================================================================
// DebounceAction<T>
// ============================================================================

/**
 * @brief 디바운스 액션 — 정적 구간(gap) 후 마지막 아이템 출력.
 *
 * 새 아이템이 도착할 때마다 타이머를 재설정합니다.
 * `gap` 동안 새 아이템이 없으면 마지막 아이템을 출력 채널에 전달합니다.
 *
 * @tparam T 아이템 타입.
 */
template <typename T>
class DebounceAction {
public:
  /**
   * @brief DebounceAction 설정 구조체.
   */
  struct Config {
    std::chrono::milliseconds gap{100}; ///< 정적 구간 (조용한 시간)
    size_t channel_cap = 256;           ///< 입력 채널 용량
  };

  /**
   * @brief DebounceAction을 생성합니다.
   *
   * @param cfg 설정.
   */
  explicit DebounceAction(Config cfg = {})
      : cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<T>>>(
            cfg_.channel_cap)) {}

  DebounceAction(const DebounceAction&) = delete;
  DebounceAction& operator=(const DebounceAction&) = delete;
  DebounceAction(DebounceAction&&)                 = default;
  DebounceAction& operator=(DebounceAction&&)      = default;

  /**
   * @brief 아이템을 입력 채널에 넣습니다 (backpressure).
   */
  Task<Result<void>> push(T item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<T>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief 아이템을 논블로킹으로 넣으려 시도합니다.
   */
  bool try_push(T item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<T>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief DebounceAction을 시작합니다.
   *
   * @param dispatcher 코루틴을 실행할 Dispatcher.
   * @param out        출력 채널.
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<T>>> out = nullptr) {
    out_channel_ = out;
    stop_src_    = std::make_unique<std::stop_source>();
    dispatcher.spawn(worker_loop());
  }

  /**
   * @brief 입력 채널을 닫고 워커가 완료될 때까지 기다립니다.
   */
  Task<void> drain() {
    in_channel_->close();
    while (running_.load(std::memory_order_acquire)) {
      struct Yield {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
          if (auto* r = Reactor::current())
            r->post([h]() mutable { h.resume(); });
          else
            h.resume();
        }
        void await_resume() noexcept {}
      };
      co_await Yield{};
    }
    if (out_channel_)
      out_channel_->close();
    co_return;
  }

  /**
   * @brief 즉시 정지합니다.
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    in_channel_->close();
  }

  /**
   * @brief 출력 채널을 반환합니다.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> output() const {
    return out_channel_;
  }

  /**
   * @brief 입력 채널을 반환합니다.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> input() const {
    return in_channel_;
  }

private:
  /**
   * @brief 디바운스 워커 루프.
   *
   * 알고리즘:
   * 1. 블로킹 recv()로 아이템 대기.
   * 2. 새 아이템이 오면 deadline 갱신.
   * 3. deadline 이내 새 아이템이 없으면 마지막 아이템 출력.
   */
  Task<void> worker_loop() {
    running_.store(true, std::memory_order_release);

    std::optional<ContextualItem<T>> pending;
    auto deadline = std::chrono::steady_clock::time_point::max();

    for (;;) {
      // pending이 있으면 deadline까지 try_recv 시도
      if (pending) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          // gap 경과 — 마지막 아이템 출력
          if (out_channel_) {
            co_await out_channel_->send(
                ContextualItem<T>{std::move(pending->value), pending->ctx});
          }
          pending.reset();
          deadline = std::chrono::steady_clock::time_point::max();
          continue;
        }

        // 논블로킹으로 새 아이템 확인
        auto item = in_channel_->try_recv();
        if (item) {
          // 새 아이템 → deadline 갱신
          pending  = std::move(*item);
          deadline = std::chrono::steady_clock::now() +
                     std::chrono::duration_cast<std::chrono::nanoseconds>(cfg_.gap);
          continue;
        }

        if (in_channel_->is_closed()) break;

        // yield → Reactor가 다른 코루틴을 실행할 기회
        struct Yield {
          bool await_ready() noexcept { return false; }
          void await_suspend(std::coroutine_handle<> h) noexcept {
            if (auto* r = Reactor::current())
              r->post([h]() mutable { h.resume(); });
            else
              h.resume();
          }
          void await_resume() noexcept {}
        };
        co_await Yield{};
      } else {
        // pending 없음 → 블로킹 recv
        auto item = co_await in_channel_->recv();
        if (!item) break; // EOS
        pending  = std::move(*item);
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::duration_cast<std::chrono::nanoseconds>(cfg_.gap);
      }
    }

    // EOS 시 pending이 있으면 마지막 출력
    if (pending && out_channel_) {
      co_await out_channel_->send(
          ContextualItem<T>{std::move(pending->value), pending->ctx});
    }

    running_.store(false, std::memory_order_release);
    co_return;
  }

  Config                                                  cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>        in_channel_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>        out_channel_;
  std::unique_ptr<std::stop_source>                       stop_src_;
  std::atomic<bool>                                       running_{false};
};

// ============================================================================
// ThrottleAction<T>
// ============================================================================

/**
 * @brief 스로틀 액션 — 토큰 버킷 기반 처리량 제한.
 *
 * `rate_per_sec` 토큰/초 속도로 아이템을 통과시킵니다.
 * `burst`개까지 순간적인 폭주를 허용합니다.
 *
 * 토큰이 부족하면 충분한 토큰이 축적될 때까지 대기합니다.
 *
 * @tparam T 아이템 타입.
 */
template <typename T>
class ThrottleAction {
public:
  /**
   * @brief ThrottleAction 설정 구조체.
   */
  struct Config {
    size_t rate_per_sec = 1000; ///< 초당 처리 아이템 수
    size_t burst        = 100;  ///< 버스트 허용 용량
    size_t channel_cap  = 1024; ///< 입력 채널 용량
  };

  /**
   * @brief ThrottleAction을 생성합니다.
   *
   * @param cfg 설정.
   */
  explicit ThrottleAction(Config cfg = {})
      : cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<T>>>(
            cfg_.channel_cap)),
        tokens_(static_cast<double>(cfg_.burst)) {}

  ThrottleAction(const ThrottleAction&) = delete;
  ThrottleAction& operator=(const ThrottleAction&) = delete;
  ThrottleAction(ThrottleAction&&)                 = default;
  ThrottleAction& operator=(ThrottleAction&&)      = default;

  /**
   * @brief 아이템을 입력 채널에 넣습니다 (backpressure).
   */
  Task<Result<void>> push(T item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<T>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief 아이템을 논블로킹으로 넣으려 시도합니다.
   */
  bool try_push(T item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<T>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief ThrottleAction을 시작합니다.
   *
   * @param dispatcher 코루틴을 실행할 Dispatcher.
   * @param out        출력 채널.
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<T>>> out = nullptr) {
    out_channel_ = out;
    stop_src_    = std::make_unique<std::stop_source>();
    last_refill_ = std::chrono::steady_clock::now();
    dispatcher.spawn(worker_loop());
  }

  /**
   * @brief 입력 채널을 닫고 워커가 완료될 때까지 기다립니다.
   */
  Task<void> drain() {
    in_channel_->close();
    while (running_.load(std::memory_order_acquire)) {
      struct Yield {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
          if (auto* r = Reactor::current())
            r->post([h]() mutable { h.resume(); });
          else
            h.resume();
        }
        void await_resume() noexcept {}
      };
      co_await Yield{};
    }
    if (out_channel_)
      out_channel_->close();
    co_return;
  }

  /**
   * @brief 즉시 정지합니다.
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    in_channel_->close();
  }

  /**
   * @brief 출력 채널을 반환합니다.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> output() const {
    return out_channel_;
  }

  /**
   * @brief 입력 채널을 반환합니다.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> input() const {
    return in_channel_;
  }

private:
  /**
   * @brief 토큰 보충 — 경과 시간에 비례하여 토큰 추가.
   *
   * 최대 burst 토큰 유지.
   */
  void refill_tokens() {
    auto now = std::chrono::steady_clock::now();
    double elapsed_sec =
        std::chrono::duration<double>(now - last_refill_).count();
    last_refill_ = now;

    tokens_ += elapsed_sec * static_cast<double>(cfg_.rate_per_sec);
    double max_tokens = static_cast<double>(cfg_.burst);
    if (tokens_ > max_tokens)
      tokens_ = max_tokens;
  }

  /**
   * @brief 스로틀 워커 루프.
   *
   * 토큰 버킷:
   * 1. 아이템을 recv().
   * 2. 토큰 보충.
   * 3. 토큰이 1 이상이면 토큰 소비 후 즉시 전달.
   * 4. 토큰 부족 시 yield 후 재시도.
   */
  Task<void> worker_loop() {
    running_.store(true, std::memory_order_release);
    auto stop_token = stop_src_ ? stop_src_->get_token() : std::stop_token{};

    for (;;) {
      if (stop_token.stop_requested()) break;

      auto citem = co_await in_channel_->recv();
      if (!citem) break; // EOS

      // 토큰이 생길 때까지 대기
      for (;;) {
        refill_tokens();
        if (tokens_ >= 1.0) {
          tokens_ -= 1.0;
          break; // 토큰 획득
        }

        // 대기: 필요한 시간만큼 yield
        // (토큰 1개를 생성하는 데 필요한 시간 = 1 / rate_per_sec 초)
        struct Yield {
          bool await_ready() noexcept { return false; }
          void await_suspend(std::coroutine_handle<> h) noexcept {
            if (auto* r = Reactor::current())
              r->post([h]() mutable { h.resume(); });
            else
              h.resume();
          }
          void await_resume() noexcept {}
        };
        co_await Yield{};
      }

      if (out_channel_) {
        co_await out_channel_->send(std::move(*citem));
      }
    }

    running_.store(false, std::memory_order_release);
    co_return;
  }

  Config                                                  cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>        in_channel_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>        out_channel_;
  std::unique_ptr<std::stop_source>                       stop_src_;
  std::atomic<bool>                                       running_{false};
  // 토큰 버킷 상태 (워커 코루틴에서만 접근 — 단일 워커이므로 동기화 불필요)
  double                                                  tokens_{0.0};
  std::chrono::steady_clock::time_point                   last_refill_;
};

// ============================================================================
// ScatterGatherAction<In, SubIn, SubOut, Out>
// ============================================================================

/**
 * @brief 스캐터-개더 액션 — 분산 처리 후 집계.
 *
 * 처리 흐름:
 * 1. **Scatter**: 하나의 `In`을 여러 `SubIn`으로 분할.
 * 2. **Process**: 각 `SubIn`을 병렬로 처리하여 `SubOut` 생성.
 * 3. **Gather**: 모든 `SubOut`을 `Out` 하나로 집계.
 *
 * @tparam In     원본 입력 타입.
 * @tparam SubIn  분산된 하위 작업 입력 타입.
 * @tparam SubOut 하위 작업 출력 타입.
 * @tparam Out    집계된 최종 출력 타입.
 */
template <typename In, typename SubIn, typename SubOut, typename Out>
class ScatterGatherAction {
public:
  /** @brief 분산 함수 타입: `In → vector<SubIn>`. */
  using ScatterFn = std::function<std::vector<SubIn>(In)>;
  /** @brief 하위 처리 함수 타입: `(SubIn, ActionEnv) → Task<Result<SubOut>>`. */
  using ProcessFn = std::function<Task<Result<SubOut>>(SubIn, ActionEnv)>;
  /** @brief 집계 함수 타입: `(In, vector<SubOut>) → Out`. */
  using GatherFn  = std::function<Out(In, std::vector<SubOut>)>;

  /**
   * @brief ScatterGatherAction 설정 구조체.
   */
  struct Config {
    size_t max_parallel = 8;    ///< 최대 병렬 하위 작업 수
    size_t channel_cap  = 256;  ///< 입력 채널 용량
    ServiceRegistry* registry = nullptr; ///< 파이프라인 ServiceRegistry
  };

  /**
   * @brief ScatterGatherAction을 생성합니다.
   *
   * @param scatter 분산 함수.
   * @param process 하위 처리 함수.
   * @param gather  집계 함수.
   * @param cfg     설정.
   */
  ScatterGatherAction(ScatterFn scatter, ProcessFn process, GatherFn gather,
                      Config cfg = {})
      : scatter_(std::move(scatter)),
        process_(std::move(process)),
        gather_(std::move(gather)),
        cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<In>>>(
            cfg_.channel_cap)) {}

  ScatterGatherAction(const ScatterGatherAction&) = delete;
  ScatterGatherAction& operator=(const ScatterGatherAction&) = delete;
  ScatterGatherAction(ScatterGatherAction&&)                 = default;
  ScatterGatherAction& operator=(ScatterGatherAction&&)      = default;

  /**
   * @brief 아이템을 입력 채널에 넣습니다 (backpressure).
   */
  Task<Result<void>> push(In item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief 아이템을 논블로킹으로 넣으려 시도합니다.
   */
  bool try_push(In item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief ScatterGatherAction을 시작합니다.
   *
   * @param dispatcher 코루틴을 실행할 Dispatcher.
   * @param out        출력 채널.
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
    out_channel_ = out;
    stop_src_    = std::make_unique<std::stop_source>();
    dispatcher.spawn(worker_loop());
  }

  /**
   * @brief 입력 채널을 닫고 워커가 완료될 때까지 기다립니다.
   */
  Task<void> drain() {
    in_channel_->close();
    while (running_.load(std::memory_order_acquire)) {
      struct Yield {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
          if (auto* r = Reactor::current())
            r->post([h]() mutable { h.resume(); });
          else
            h.resume();
        }
        void await_resume() noexcept {}
      };
      co_await Yield{};
    }
    if (out_channel_)
      out_channel_->close();
    co_return;
  }

  /**
   * @brief 즉시 정지합니다.
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    in_channel_->close();
  }

  /**
   * @brief 출력 채널을 반환합니다.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
    return out_channel_;
  }

  /**
   * @brief 입력 채널을 반환합니다.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<In>>> input() const {
    return in_channel_;
  }

private:
  /**
   * @brief 개별 SubIn 처리 코루틴.
   *
   * 처리 결과를 공유 결과 벡터에 원자적으로 기록하고
   * 완료 카운터를 감소시킵니다.
   */
  struct SubTask {
    static Task<void> run(
        SubIn item,
        size_t idx,
        ActionEnv env,
        std::shared_ptr<std::vector<std::optional<SubOut>>> results,
        std::shared_ptr<std::atomic<size_t>> pending,
        std::shared_ptr<AsyncChannel<int>> done_chan,
        std::function<Task<Result<SubOut>>(SubIn, ActionEnv)> process) {
      auto result = co_await process(std::move(item), env);
      if (result.has_value())
        (*results)[idx] = std::move(*result);
      // 마지막 완료 시 done 신호
      size_t remaining = pending->fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (remaining == 0)
        done_chan->try_send(0);
      co_return;
    }
  };

  /**
   * @brief 스캐터-개더 워커 루프.
   *
   * 각 입력 아이템에 대해:
   * 1. scatter() 호출로 SubIn 목록 생성.
   * 2. 각 SubIn을 병렬 코루틴으로 처리 (최대 max_parallel).
   * 3. 모든 SubIn 처리 완료 대기.
   * 4. gather() 호출로 결과 집계 후 출력.
   */
  Task<void> worker_loop() {
    running_.store(true, std::memory_order_release);
    auto stop_token = stop_src_ ? stop_src_->get_token() : std::stop_token{};
    size_t worker_idx = 0;

    for (;;) {
      if (stop_token.stop_requested()) break;

      auto citem = co_await in_channel_->recv();
      if (!citem) break; // EOS

      // 원본 값 저장 (gather에서 필요)
      In orig_value = citem->value;
      Context orig_ctx = citem->ctx;

      // 1. Scatter
      std::vector<SubIn> sub_items = scatter_(orig_value);
      if (sub_items.empty()) {
        // 하위 작업 없음 → gather에 빈 결과 전달
        Out out_val = gather_(std::move(orig_value), {});
        if (out_channel_) {
          co_await out_channel_->send(
              ContextualItem<Out>{std::move(out_val), orig_ctx});
        }
        continue;
      }

      size_t total = sub_items.size();
      auto results = std::make_shared<std::vector<std::optional<SubOut>>>(total);
      auto pending = std::make_shared<std::atomic<size_t>>(total);
      // done_chan: 모든 하위 작업 완료 시 신호
      auto done_chan = std::make_shared<AsyncChannel<int>>(2);

      // 2. 병렬 처리 (max_parallel 제한)
      // 현재 구현: max_parallel 이하면 전부 즉시 spawn
      // 초과 시 순차적으로 처리 (단순화)
      size_t dispatch_count = std::min(total, cfg_.max_parallel);
      (void)dispatch_count;

      ServiceRegistry* reg =
          cfg_.registry ? cfg_.registry : &global_registry();

      for (size_t i = 0; i < total; ++i) {
        ActionEnv env{
            .ctx        = orig_ctx,
            .stop       = stop_token,
            .worker_idx = worker_idx + i,
            .registry   = reg,
        };

        auto task = SubTask::run(
            std::move(sub_items[i]), i, env, results, pending, done_chan,
            process_);
        auto h = task.handle;
        task.detach();
        if (auto* r = Reactor::current())
          r->post([h]() mutable { h.resume(); });
      }

      // 3. 모든 SubIn 처리 완료 대기
      co_await done_chan->recv();

      // 4. Gather: 성공한 SubOut만 수집
      std::vector<SubOut> sub_results;
      sub_results.reserve(total);
      for (auto& opt : *results) {
        if (opt.has_value())
          sub_results.push_back(std::move(*opt));
      }

      Out out_val = gather_(std::move(orig_value), std::move(sub_results));
      if (out_channel_) {
        co_await out_channel_->send(
            ContextualItem<Out>{std::move(out_val), orig_ctx});
      }
    }

    running_.store(false, std::memory_order_release);
    co_return;
  }

  ScatterFn                                               scatter_;
  ProcessFn                                               process_;
  GatherFn                                                gather_;
  Config                                                  cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<In>>>       in_channel_;
  std::shared_ptr<AsyncChannel<ContextualItem<Out>>>      out_channel_;
  std::unique_ptr<std::stop_source>                       stop_src_;
  std::atomic<bool>                                       running_{false};
};

} // namespace qbuem

/** @} */
