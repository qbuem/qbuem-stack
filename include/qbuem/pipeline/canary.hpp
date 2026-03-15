#pragma once

/**
 * @file qbuem/pipeline/canary.hpp
 * @brief 카나리 배포 자동화 — CanaryRouter
 * @defgroup qbuem_canary CanaryRouter
 * @ingroup qbuem_pipeline
 *
 * CanaryRouter는 트래픽을 stable(기존) 파이프라인과 canary(신규) 파이프라인으로
 * 비율에 따라 분배하고, 지표를 모니터링하여 자동/수동 롤백을 지원합니다.
 *
 * ## 롤아웃 절차
 * ```
 * 1% → 5% → 25% → 100%  (단계별 자동 증가)
 *                         ↑ 각 단계에서 error_delta / p99 / budget 검사
 * ```
 *
 * ## 사용 예시
 * ```cpp
 * CanaryRouter<int> router;
 * router.set_stable(stable_pipeline);
 * router.set_canary(canary_pipeline);
 * router.start_gradual_rollout({
 *   .steps             = {1, 5, 25, 100},
 *   .step_duration     = 60s,
 *   .max_error_delta   = 0.01,   // 1% 이상 오류 증가 시 롤백
 *   .max_p99_ratio     = 1.5,    // p99가 50% 이상 악화 시 롤백
 * });
 * ```
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace qbuem {

// ---------------------------------------------------------------------------
// CanaryMetrics — 파이프라인별 지표 수집
// ---------------------------------------------------------------------------

/**
 * @brief 카나리 배포에서 사용할 파이프라인 지표.
 */
struct CanaryMetrics {
  std::atomic<uint64_t> total{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> latency_sum_us{0};   // 레이턴시 합계 (µs)
  std::atomic<uint64_t> latency_count{0};

  void record_success(uint64_t latency_us) {
    ++total;
    ++latency_count;
    latency_sum_us.fetch_add(latency_us, std::memory_order_relaxed);
  }

  void record_error() {
    ++total;
    ++errors;
  }

  [[nodiscard]] double error_rate() const noexcept {
    uint64_t t = total.load(std::memory_order_relaxed);
    return (t == 0) ? 0.0 : static_cast<double>(errors.load()) / t;
  }

  [[nodiscard]] uint64_t avg_latency_us() const noexcept {
    uint64_t c = latency_count.load(std::memory_order_relaxed);
    return (c == 0) ? 0 : latency_sum_us.load() / c;
  }

  void reset() {
    total.store(0);
    errors.store(0);
    latency_sum_us.store(0);
    latency_count.store(0);
  }
};

// ---------------------------------------------------------------------------
// CanaryRouter<T>
// ---------------------------------------------------------------------------

/**
 * @brief 트래픽 분배 + 지표 기반 자동 롤백 카나리 라우터.
 *
 * @tparam T 파이프라인 메시지 타입.
 */
template <typename T>
class CanaryRouter {
public:
  // -------------------------------------------------------------------------
  // 설정
  // -------------------------------------------------------------------------

  /**
   * @brief 단계별 롤아웃 설정.
   */
  struct RolloutConfig {
    /// 각 단계의 카나리 트래픽 비율 (%) — 예: {1, 5, 25, 100}
    std::vector<uint32_t> steps{1, 5, 25, 100};

    /// 각 단계의 지속 시간 (기본 60초)
    std::chrono::seconds step_duration{60};

    /// 카나리 오류율이 stable 대비 이 값 이상 증가하면 롤백
    double max_error_delta{0.01};

    /// 카나리 평균 레이턴시가 stable 대비 이 배수 초과 시 롤백
    double max_latency_ratio{1.5};

    /// 롤백 발생 시 호출할 콜백
    std::function<void(std::string_view reason)> on_rollback;

    /// 단계 완료 시 호출할 콜백 (현재 step %)
    std::function<void(uint32_t step_pct)> on_step_complete;
  };

  // 파이프라인 push 인터페이스
  using PushFn = std::function<bool(T)>;  // try_push 래퍼

  // -------------------------------------------------------------------------
  // 파이프라인 등록
  // -------------------------------------------------------------------------

  /**
   * @brief stable 파이프라인의 push 함수를 설정합니다.
   */
  CanaryRouter &set_stable(PushFn fn) {
    stable_push_ = std::move(fn);
    return *this;
  }

  /**
   * @brief canary 파이프라인의 push 함수를 설정합니다.
   */
  CanaryRouter &set_canary(PushFn fn) {
    canary_push_ = std::move(fn);
    return *this;
  }

  // -------------------------------------------------------------------------
  // 트래픽 라우팅
  // -------------------------------------------------------------------------

  /**
   * @brief 아이템을 현재 비율에 따라 stable 또는 canary로 전송합니다.
   *
   * @param item 전송할 아이템.
   * @returns true if pushed successfully.
   */
  bool push(T item) {
    uint32_t pct = canary_pct_.load(std::memory_order_relaxed);
    bool send_to_canary = (pct > 0) && (rng_() % 100 < pct);

    if (send_to_canary && canary_push_) {
      auto t0 = std::chrono::steady_clock::now();
      bool ok = canary_push_(item);
      auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
      if (ok) canary_metrics_.record_success(static_cast<uint64_t>(dt));
      else    canary_metrics_.record_error();
      return ok;
    }

    if (stable_push_) {
      auto t0 = std::chrono::steady_clock::now();
      bool ok = stable_push_(item);
      auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
      if (ok) stable_metrics_.record_success(static_cast<uint64_t>(dt));
      else    stable_metrics_.record_error();
      return ok;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // 롤아웃 제어
  // -------------------------------------------------------------------------

  /**
   * @brief 단계별 점진적 롤아웃을 시작합니다.
   *
   * 각 단계에서 지표를 평가하고 조건 위반 시 자동 롤백합니다.
   * 이 함수는 백그라운드 코루틴으로 실행되어야 합니다.
   *
   * @param cfg 롤아웃 설정.
   * @returns Task<void> — 롤아웃 완료 또는 롤백 시 종료.
   */
  Task<void> start_gradual_rollout(RolloutConfig cfg) {
    cfg_ = std::move(cfg);
    rollout_active_.store(true);

    for (uint32_t step_pct : cfg_.steps) {
      if (!rollout_active_.load()) co_return;  // 수동 롤백됨

      // 현재 단계 비율 적용
      set_canary_percent(step_pct);
      stable_metrics_.reset();
      canary_metrics_.reset();

      // step_duration 동안 대기하며 지표 모니터링
      auto deadline = std::chrono::steady_clock::now() + cfg_.step_duration;
      while (std::chrono::steady_clock::now() < deadline) {
        // 간단한 yield — 실제 환경에서는 co_await sleep 사용
        co_await SleepAwaiter{};

        if (!rollout_active_.load()) co_return;

        // 자동 롤백 조건 검사
        if (auto reason = check_rollback_condition()) {
          rollout_active_.store(false);
          set_canary_percent(0);
          if (cfg_.on_rollback) cfg_.on_rollback(*reason);
          co_return;
        }
      }

      if (cfg_.on_step_complete) cfg_.on_step_complete(step_pct);
    }

    // 100% 달성 → 롤아웃 완료
    rollout_active_.store(false);
  }

  /**
   * @brief 즉시 stable로 수동 롤백합니다.
   */
  void rollback_to_stable() {
    rollout_active_.store(false);
    set_canary_percent(0);
  }

  /**
   * @brief 카나리 트래픽 비율을 직접 설정합니다 (0–100).
   */
  void set_canary_percent(uint32_t pct) noexcept {
    canary_pct_.store(std::min(pct, 100u), std::memory_order_relaxed);
  }

  [[nodiscard]] uint32_t canary_percent() const noexcept {
    return canary_pct_.load(std::memory_order_relaxed);
  }

  // -------------------------------------------------------------------------
  // 지표 조회
  // -------------------------------------------------------------------------

  [[nodiscard]] const CanaryMetrics &stable_metrics() const noexcept { return stable_metrics_; }
  [[nodiscard]] const CanaryMetrics &canary_metrics() const noexcept { return canary_metrics_; }

private:
  // -------------------------------------------------------------------------
  // 자동 롤백 조건 검사
  // -------------------------------------------------------------------------

  [[nodiscard]] std::optional<std::string> check_rollback_condition() const {
    double s_err = stable_metrics_.error_rate();
    double c_err = canary_metrics_.error_rate();
    if (c_err - s_err > cfg_.max_error_delta) {
      return "error_delta exceeded: canary=" + std::to_string(c_err) +
             " stable=" + std::to_string(s_err);
    }

    uint64_t s_lat = stable_metrics_.avg_latency_us();
    uint64_t c_lat = canary_metrics_.avg_latency_us();
    if (s_lat > 0 &&
        static_cast<double>(c_lat) / s_lat > cfg_.max_latency_ratio) {
      return "latency_ratio exceeded: canary=" + std::to_string(c_lat) +
             "us stable=" + std::to_string(s_lat) + "us";
    }

    return std::nullopt;
  }

  // 간단한 1-tick yield awaiter (실 사용 시 sleep awaiter 대체 권장)
  struct SleepAwaiter {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
      // no-op: 즉시 재개 (실제 sleep은 TimerWheel / AsyncSleep 사용)
      h.resume();
    }
    void await_resume() const noexcept {}
  };

  // -------------------------------------------------------------------------
  // 데이터 멤버
  // -------------------------------------------------------------------------
  PushFn              stable_push_;
  PushFn              canary_push_;
  CanaryMetrics       stable_metrics_;
  CanaryMetrics       canary_metrics_;
  std::atomic<uint32_t> canary_pct_{0};
  std::atomic<bool>   rollout_active_{false};
  RolloutConfig       cfg_;

  // 스레드-로컬 RNG (간단한 구현; 정밀도보다 속도 우선)
  static thread_local std::mt19937 rng_;
};

template <typename T>
thread_local std::mt19937 CanaryRouter<T>::rng_{std::random_device{}()};

/** @} */

} // namespace qbuem
