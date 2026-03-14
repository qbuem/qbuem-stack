#pragma once

/**
 * @file qbuem/pipeline/action.hpp
 * @brief 파이프라인 액션 — Action<In, Out>
 * @defgroup qbuem_action Action
 * @ingroup qbuem_pipeline
 *
 * Action은 파이프라인의 처리 단계입니다.
 * 워커 코루틴 풀을 관리하고 입력 채널에서 아이템을 꺼내
 * 처리 함수를 적용한 결과를 출력 채널에 넣습니다.
 *
 * ## Action 함수 형태
 * ```cpp
 * // Full: 컨텍스트 + 취소 신호 + 워커 인덱스
 * Task<Result<Out>>(In item, ActionEnv env)
 *
 * // Simple: 취소 신호만
 * Task<Result<Out>>(In item, std::stop_token stop)
 *
 * // Plain: 최소
 * Task<Result<Out>>(In item)
 * ```
 *
 * ## 상태 패턴
 * - **Stateless**: 람다 캡처 없음
 * - **Immutable**: `const shared_ptr<T>` 캡처
 * - **Mutable(WorkerLocal)**: `WorkerLocal<T>` + `env.worker_idx`
 * - **External**: `ServiceRegistry` 통해 접근
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/concepts.hpp>
#include <qbuem/pipeline/context.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace qbuem {

/**
 * @brief 파이프라인 액션 — 코루틴 워커 풀 기반 처리 단계.
 *
 * @tparam In  입력 타입.
 * @tparam Out 출력 타입.
 */
template <typename In, typename Out>
class Action {
public:
  /** @brief 정규화된 Action 처리 함수 타입 (Full 서명). */
  using Fn = std::function<Task<Result<Out>>(In, ActionEnv)>;

  /**
   * @brief Action 설정 구조체.
   */
  struct Config {
    size_t min_workers   = 1;    ///< 최소 워커 수
    size_t max_workers   = 4;    ///< 최대 워커 수
    size_t channel_cap   = 256;  ///< 입력 채널 용량
    bool   auto_scale    = true; ///< 부하 기반 자동 스케일링
    bool   keyed_ordering = false; ///< 동일 키 순서 보장 (순서 중요한 경우)
    ServiceRegistry *registry = nullptr; ///< 파이프라인 ServiceRegistry
  };

  /**
   * @brief Action을 생성합니다.
   *
   * @tparam FnT ActionFn<FnT, In, Out> concept을 만족하는 함수 타입.
   * @param fn   처리 함수 (Full/Simple/Plain 서명 모두 허용).
   * @param cfg  Action 설정.
   */
  template <typename FnT>
    requires ActionFn<FnT, In, Out>
  Action(FnT fn, Config cfg = {})
      : fn_(to_full_action_fn<FnT, In, Out>(std::move(fn))),
        cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<In>>>(cfg_.channel_cap)) {}

  Action(const Action &) = delete;
  Action &operator=(const Action &) = delete;
  Action(Action &&) = default;
  Action &operator=(Action &&) = default;

  // -------------------------------------------------------------------------
  // 아이템 전송
  // -------------------------------------------------------------------------

  /**
   * @brief 아이템을 Action의 입력 채널에 넣습니다 (backpressure).
   *
   * @param item 처리할 아이템.
   * @param ctx  아이템 컨텍스트 (기본: 빈 Context).
   */
  Task<Result<void>> push(In item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief 아이템을 논블로킹으로 넣으려 시도합니다.
   *
   * @returns 성공 시 `true`, 채널 포화 시 `false`.
   */
  bool try_push(In item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  // -------------------------------------------------------------------------
  // 라이프사이클
  // -------------------------------------------------------------------------

  /**
   * @brief Action을 시작합니다 — `min_workers`개 코루틴을 Dispatcher에 spawn.
   *
   * @param dispatcher 코루틴을 실행할 Dispatcher.
   * @param out        출력 채널 (nullptr이면 결과를 버림).
   */
  void start(Dispatcher &dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
    out_channel_ = out;
    stop_src_    = std::make_unique<std::stop_source>();

    for (size_t i = 0; i < cfg_.min_workers; ++i) {
      worker_count_.fetch_add(1, std::memory_order_relaxed);
      dispatcher.spawn(worker_loop(i, dispatcher));
    }
  }

  /**
   * @brief 입력 채널을 닫고 모든 워커가 완료될 때까지 기다립니다.
   *
   * `drain()` 후 출력 채널도 자동으로 닫힙니다.
   */
  Task<void> drain() {
    in_channel_->close();
    // Wait for all workers to exit
    while (worker_count_.load(std::memory_order_acquire) > 0) {
      struct Yield {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
          if (auto *r = Reactor::current())
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
   * @brief Action을 즉시 정지합니다 (취소 신호 전송).
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    in_channel_->close();
  }

  /**
   * @brief 워커 수를 `n`으로 조정합니다.
   *
   * @param n 새 목표 워커 수.
   * @param dispatcher 새 워커를 spawn할 Dispatcher.
   */
  void scale_to(size_t n, Dispatcher &dispatcher) {
    size_t current = worker_count_.load(std::memory_order_relaxed);
    if (n > current) {
      for (size_t i = current; i < n; ++i) {
        worker_count_.fetch_add(1, std::memory_order_relaxed);
        dispatcher.spawn(worker_loop(i, dispatcher));
      }
    } else if (n < current) {
      target_workers_.store(n, std::memory_order_release);
    }
  }

  void scale_out(Dispatcher &dispatcher) {
    scale_to(worker_count_.load() + 1, dispatcher);
  }

  void scale_in() {
    size_t c = worker_count_.load(std::memory_order_relaxed);
    if (c > cfg_.min_workers)
      target_workers_.store(c - 1, std::memory_order_release);
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
  // -------------------------------------------------------------------------
  // Worker loop
  // -------------------------------------------------------------------------
  Task<void> worker_loop(size_t worker_idx, Dispatcher &dispatcher) {
    auto stop_token = stop_src_ ? stop_src_->get_token() : std::stop_token{};

    for (;;) {
      // Scale-in check: if target_workers_ < our index, exit
      size_t target = target_workers_.load(std::memory_order_acquire);
      if (target > 0 && worker_idx >= target) {
        break;
      }

      // Receive next item from input channel
      auto citem = co_await in_channel_->recv();
      if (!citem) break; // EOS

      // Build ActionEnv
      ActionEnv env{
          .ctx        = citem->ctx,
          .stop       = stop_token,
          .worker_idx = worker_idx,
          .registry   = cfg_.registry ? cfg_.registry : &global_registry(),
      };

      // Execute the action
      auto result = co_await fn_(std::move(citem->value), env);

      // Forward result to output channel
      if (result.has_value() && out_channel_) {
        co_await out_channel_->send(
            ContextualItem<Out>{std::move(*result), env.ctx});
      }
      // On error: item is dropped (DLQ support comes in v0.8.0)
    }

    size_t remaining = worker_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0 && out_channel_)
      out_channel_->close();
    co_return;
  }

  // -------------------------------------------------------------------------
  // Data members
  // -------------------------------------------------------------------------
  Fn                                                    fn_;
  Config                                                cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<In>>>     in_channel_;
  std::shared_ptr<AsyncChannel<ContextualItem<Out>>>    out_channel_;
  std::unique_ptr<std::stop_source>                     stop_src_;
  std::atomic<size_t>                                   worker_count_{0};
  std::atomic<size_t>                                   target_workers_{0}; // 0 = unlimited
};

} // namespace qbuem

/** @} */
