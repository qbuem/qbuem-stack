#pragma once

/**
 * @file qbuem/pipeline/batch_action.hpp
 * @brief 배치 처리 액션 — BatchAction<In, Out>
 * @defgroup qbuem_batch_action BatchAction
 * @ingroup qbuem_pipeline
 *
 * BatchAction은 최대 N개 아이템을 한 번에 모아 배치로 처리하는 파이프라인 단계입니다.
 * 워커는 `max_batch_size`에 도달하거나 `max_wait_ms` 타임아웃이 만료될 때까지
 * 아이템을 수집한 뒤 처리 함수를 호출합니다.
 *
 * ## 함수 서명
 * ```cpp
 * Task<Result<std::vector<Out>>>(std::vector<In> batch, ActionEnv env)
 * ```
 *
 * ## 컨텍스트 전파
 * 출력 아이템 각각에는 배치의 **첫 번째** 아이템 컨텍스트가 부여됩니다.
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
#include <stop_token>
#include <vector>

namespace qbuem {

/**
 * @brief 배치 처리 액션 — 최대 N개씩 묶어 처리 함수 호출.
 *
 * @tparam In  입력 아이템 타입.
 * @tparam Out 출력 아이템 타입.
 */
template <typename In, typename Out>
class BatchAction {
public:
  /** @brief 정규화된 배치 처리 함수 타입. */
  using Fn = std::function<Task<Result<std::vector<Out>>>(std::vector<In>, ActionEnv)>;

  /**
   * @brief BatchAction 설정 구조체.
   */
  struct Config {
    size_t max_batch_size = 64;    ///< 배치 최대 크기
    size_t max_wait_ms    = 10;    ///< 배치 수집 최대 대기 시간 (ms)
    size_t workers        = 1;     ///< 워커 수
    size_t channel_cap    = 1024;  ///< 입력 채널 용량
    ServiceRegistry* registry = nullptr; ///< 파이프라인 ServiceRegistry
  };

  /**
   * @brief BatchAction을 생성합니다.
   *
   * 처리 함수 서명: `Task<Result<std::vector<Out>>>(std::vector<In>, ActionEnv)`
   *
   * @tparam FnT 처리 함수 타입.
   * @param fn   배치 처리 함수.
   * @param cfg  설정.
   */
  template <typename FnT>
    requires requires(FnT f, std::vector<In> v, ActionEnv e) {
      { f(std::move(v), e) } -> std::same_as<Task<Result<std::vector<Out>>>>;
    }
  BatchAction(FnT fn, Config cfg = {})
      : fn_(std::move(fn)),
        cfg_(std::move(cfg)),
        in_channel_(std::make_shared<AsyncChannel<ContextualItem<In>>>(cfg_.channel_cap)) {}

  BatchAction(const BatchAction&) = delete;
  BatchAction& operator=(const BatchAction&) = delete;
  BatchAction(BatchAction&&) = default;
  BatchAction& operator=(BatchAction&&) = default;

  // -------------------------------------------------------------------------
  // 아이템 전송
  // -------------------------------------------------------------------------

  /**
   * @brief 아이템을 입력 채널에 넣습니다 (backpressure).
   *
   * @param item 처리할 아이템.
   * @param ctx  아이템 컨텍스트.
   */
  Task<Result<void>> push(In item, Context ctx = {}) {
    co_return co_await in_channel_->send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  /**
   * @brief 아이템을 논블로킹으로 넣으려 시도합니다.
   *
   * @returns 성공 시 true, 채널 포화 시 false.
   */
  bool try_push(In item, Context ctx = {}) {
    return in_channel_->try_send(
        ContextualItem<In>{std::move(item), std::move(ctx)});
  }

  // -------------------------------------------------------------------------
  // 라이프사이클
  // -------------------------------------------------------------------------

  /**
   * @brief BatchAction을 시작합니다 — 워커 코루틴을 Dispatcher에 spawn.
   *
   * @param dispatcher 코루틴을 실행할 Dispatcher.
   * @param out        출력 채널 (nullptr이면 결과를 버림).
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
    out_channel_ = out ? out : std::make_shared<AsyncChannel<ContextualItem<Out>>>(cfg_.channel_cap);
    stop_src_    = std::make_unique<std::stop_source>();

    for (size_t i = 0; i < cfg_.workers; ++i) {
      worker_count_.fetch_add(1, std::memory_order_relaxed);
      dispatcher.spawn(worker_loop(i));
    }
  }

  /**
   * @brief 입력 채널을 닫고 모든 워커가 완료될 때까지 기다립니다.
   *
   * drain() 후 출력 채널도 자동으로 닫힙니다.
   */
  Task<void> drain() {
    in_channel_->close();
    while (worker_count_.load(std::memory_order_acquire) > 0) {
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
   * @brief BatchAction을 즉시 정지합니다 (취소 신호 전송).
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
  // -------------------------------------------------------------------------
  // Worker loop
  // -------------------------------------------------------------------------

  /**
   * @brief 배치 수집 및 처리 워커 루프.
   *
   * 배치 수집 전략:
   * 1. 블로킹 recv()로 첫 번째 아이템 대기 (EOS이면 종료).
   * 2. deadline(max_wait_ms) 이내에 try_recv()로 추가 아이템 수집.
   * 3. max_batch_size 충족 또는 deadline 도달 시 즉시 처리.
   */
  Task<void> worker_loop(size_t worker_idx) {
    auto stop_token = stop_src_ ? stop_src_->get_token() : std::stop_token{};

    for (;;) {
      if (stop_token.stop_requested()) break;

      // --- 배치 수집 ---
      std::vector<ContextualItem<In>> batch_items;
      batch_items.reserve(cfg_.max_batch_size);

      // 첫 번째 아이템: 블로킹 recv로 대기 (EOS 감지)
      auto first = co_await in_channel_->recv();
      if (!first) break; // EOS
      batch_items.push_back(std::move(*first));

      // 나머지: max_wait_ms 이내 논블로킹 수집
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(cfg_.max_wait_ms);

      while (batch_items.size() < cfg_.max_batch_size) {
        auto item = in_channel_->try_recv();
        if (item) {
          batch_items.push_back(std::move(*item));
          continue;
        }

        // 채널 닫힘 → 현재까지 수집된 배치로 처리
        if (in_channel_->is_closed()) break;

        // 타임아웃 → 즉시 처리
        if (std::chrono::steady_clock::now() >= deadline) break;

        // Reactor에 제어권 반환: 다른 코루틴이 아이템을 생산할 기회
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

        if (std::chrono::steady_clock::now() >= deadline) break;
      }

      // --- 배치 분리: 값 벡터 + 첫 번째 컨텍스트 ---
      Context first_ctx = batch_items.front().ctx;
      std::vector<In> values;
      values.reserve(batch_items.size());
      for (auto& ci : batch_items)
        values.push_back(std::move(ci.value));

      // --- ActionEnv 구성 ---
      ActionEnv env{
          .ctx        = first_ctx,
          .stop       = stop_token,
          .worker_idx = worker_idx,
          .registry   = cfg_.registry ? cfg_.registry : &global_registry(),
      };

      // --- 처리 함수 호출 ---
      auto result = co_await fn_(std::move(values), env);

      // --- 결과를 출력 채널에 전달 ---
      // 모든 출력 아이템에 첫 번째 입력 아이템의 컨텍스트를 부여
      if (result.has_value() && out_channel_) {
        for (auto& out_item : *result) {
          auto send_r = co_await out_channel_->send(
              ContextualItem<Out>{std::move(out_item), first_ctx});
          if (!send_r.has_value())
            break; // 채널 닫힘 — 전송 중단
        }
      }
      // 에러 시 배치 드롭 (DLQ는 미래 버전에서 지원 예정)
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
};

} // namespace qbuem

/** @} */
