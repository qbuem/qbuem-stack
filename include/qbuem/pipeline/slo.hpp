#pragma once

/**
 * @file qbuem/pipeline/slo.hpp
 * @brief SLO(서비스 레벨 목표) 추적 및 에러 버짓 관리
 * @defgroup qbuem_slo SLO Tracking
 * @ingroup qbuem_pipeline
 *
 * 이 헤더는 파이프라인 액션의 레이턴시 및 에러율 SLO를 추적합니다:
 *
 * - `SloConfig`           : SLO 정책 설정 (p99/p999 목표, 에러 버짓, 위반 콜백)
 * - `LatencyHistogram`    : 롤링 윈도우 레이턴시 히스토그램 (kWindow=1024 샘플)
 * - `ErrorBudgetTracker`  : 레이턴시 + 에러율 SLO 추적기
 * - `SloObserver`         : `PipelineObserver` 확장 — SLO 위반 이벤트 훅
 * - `LoggingSloObserver`  : 표준 에러 출력 기본 구현
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/observability.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

namespace qbuem {

using std::chrono::microseconds;

// ─── SloConfig ────────────────────────────────────────────────────────────────

/**
 * @brief SLO 목표 설정 구조체.
 *
 * 레이턴시 퍼센타일 목표, 허용 에러 비율, 그리고 위반 발생 시
 * 호출될 콜백 함수를 담습니다.
 *
 * ### 기본값
 * - p99 목표: 10ms (10,000µs)
 * - p999 목표: 50ms (50,000µs)
 * - 에러 버짓: 0.1% (0.001)
 */
struct SloConfig {
  /** @brief p99 레이턴시 목표 (기본: 10ms). */
  microseconds p99_target{10'000};

  /** @brief p99.9 레이턴시 목표 (기본: 50ms). */
  microseconds p999_target{50'000};

  /**
   * @brief 허용 에러 비율 (0.0 ~ 1.0).
   *
   * 롤링 윈도우(1000개 아이템) 기준 에러율이 이 값을 초과하면
   * 에러 버짓이 소진된 것으로 판단합니다. 기본값은 0.1%(0.001)입니다.
   */
  double error_budget{0.001};

  /**
   * @brief SLO 위반 시 호출되는 콜백.
   *
   * @param action_name 위반이 감지된 액션 이름.
   *
   * 콜백 내에서 메트릭 수집, 알림 전송, 서킷 브레이커 트리거 등을
   * 수행할 수 있습니다.
   */
  std::function<void(std::string_view action_name)> on_violation;
};

// ─── LatencyHistogram ─────────────────────────────────────────────────────────

/**
 * @brief 롤링 윈도우 레이턴시 히스토그램.
 *
 * 최대 `kWindow`(1024)개의 최신 레이턴시 샘플을 링 버퍼에 보관하며
 * p99 및 p99.9 백분위수를 계산합니다.
 *
 * ### 4구간 빠른 분류
 * - 버킷 0: < 1ms  (< 1,000µs)
 * - 버킷 1: < 10ms (< 10,000µs)
 * - 버킷 2: < 100ms (< 100,000µs)
 * - 버킷 3: >= 100ms (>= 100,000µs)
 *
 * ### 스레드 안전성
 * `head_`와 `count_`는 원자 연산으로 보호됩니다.
 * 단일 생산자 환경에서 가장 정확하게 동작하며, 다중 생산자 환경에서는
 * 통계적으로 무해한 경미한 샘플 유실이 발생할 수 있습니다.
 */
class LatencyHistogram {
public:
  /** @brief 롤링 윈도우 크기 (보관할 최대 샘플 수). */
  static constexpr size_t kWindow = 1024;

  /**
   * @brief 레이턴시 샘플 하나를 기록합니다 (O(1)).
   *
   * 링 버퍼에 순환 기록하며, 버퍼가 가득 차면 가장 오래된 샘플을 덮어씁니다.
   * 4구간 버킷 카운터도 동시에 갱신합니다.
   *
   * @param latency 측정된 레이턴시.
   */
  void record(microseconds latency) noexcept {
    size_t idx = head_.fetch_add(1, std::memory_order_relaxed) % kWindow;
    auto raw = latency.count();
    samples_[idx] = static_cast<uint32_t>(
        raw > static_cast<decltype(raw)>(UINT32_MAX)
            ? UINT32_MAX
            : static_cast<uint32_t>(raw));

    size_t c = count_.load(std::memory_order_relaxed);
    if (c < kWindow)
      count_.fetch_add(1, std::memory_order_relaxed);

    // 4구간 빠른 분류 카운터 갱신
    uint64_t us = static_cast<uint64_t>(latency.count());
    if      (us <   1'000u) bucket_[0].fetch_add(1, std::memory_order_relaxed);
    else if (us <  10'000u) bucket_[1].fetch_add(1, std::memory_order_relaxed);
    else if (us < 100'000u) bucket_[2].fetch_add(1, std::memory_order_relaxed);
    else                    bucket_[3].fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief p99 레이턴시를 반환합니다.
   *
   * 현재 보관된 샘플을 정렬하여 99번째 백분위수를 반환합니다.
   * 샘플이 없으면 0을 반환합니다.
   *
   * @returns p99 레이턴시.
   */
  [[nodiscard]] microseconds p99() const { return percentile(990); }

  /**
   * @brief p99.9 레이턴시를 반환합니다.
   *
   * 현재 보관된 샘플을 정렬하여 99.9번째 백분위수를 반환합니다.
   * 샘플이 없으면 0을 반환합니다.
   *
   * @returns p99.9 레이턴시.
   */
  [[nodiscard]] microseconds p999() const { return percentile(999); }

  /**
   * @brief 4구간 버킷 카운터를 반환합니다.
   *
   * @returns {<1ms, <10ms, <100ms, >=100ms} 순서의 카운터 배열.
   */
  [[nodiscard]] std::array<uint64_t, 4> bucket_counts() const noexcept {
    return {bucket_[0].load(std::memory_order_relaxed),
            bucket_[1].load(std::memory_order_relaxed),
            bucket_[2].load(std::memory_order_relaxed),
            bucket_[3].load(std::memory_order_relaxed)};
  }

  /**
   * @brief 히스토그램을 초기화합니다.
   *
   * 모든 샘플과 카운터를 0으로 재설정합니다.
   */
  void reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    count_.store(0, std::memory_order_relaxed);
    samples_.fill(0);
    for (auto& b : bucket_)
      b.store(0, std::memory_order_relaxed);
  }

private:
  /**
   * @brief 주어진 퍼밀(per-mille) 백분위수를 계산합니다.
   *
   * @param pmille 0 ~ 1000 범위의 퍼밀 값 (990 = p99, 999 = p99.9).
   * @returns 해당 백분위수의 레이턴시. 샘플이 없으면 0.
   */
  [[nodiscard]] microseconds percentile(int pmille) const {
    size_t n = count_.load(std::memory_order_relaxed);
    if (n == 0) return microseconds{0};

    // 현재 유효 샘플을 정렬 버퍼에 복사
    std::array<uint32_t, kWindow> buf{};
    size_t head = head_.load(std::memory_order_relaxed) % kWindow;
    for (size_t i = 0; i < n; ++i)
      buf[i] = samples_[(head + kWindow - n + i) % kWindow];

    std::sort(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(n));

    size_t idx = static_cast<size_t>(
        std::ceil(static_cast<double>(n) * static_cast<double>(pmille) / 1000.0));
    if (idx > 0) --idx;
    if (idx >= n) idx = n - 1;
    return microseconds{buf[idx]};
  }

  alignas(64) std::atomic<size_t>  head_{0};
  alignas(64) std::atomic<size_t>  count_{0};
  std::array<uint32_t, kWindow>    samples_{};
  std::array<std::atomic<uint64_t>, 4> bucket_{};
};

// ─── ErrorBudgetTracker ───────────────────────────────────────────────────────

/**
 * @brief 레이턴시 및 에러율 SLO 추적기.
 *
 * 각 아이템 처리 결과(성공 + 레이턴시 / 에러)를 기록하고,
 * `SloConfig`에 설정된 목표와 비교해 위반 여부를 판단합니다.
 *
 * ### 에러율 계산
 * 롤링 1000개 아이템 윈도우 기반으로 에러율을 근사합니다.
 * 전체 누적 카운터(`total_`, `errors_`)를 사용해 계산합니다.
 *
 * ### 스레드 안전성
 * 원자 카운터(`total_`, `errors_`)는 스레드 안전합니다.
 * `LatencyHistogram`은 다중 생산자 환경에서 경미한 부정확성이 있을 수 있습니다.
 */
class ErrorBudgetTracker {
public:
  /** @brief 에러율 계산에 사용하는 롤링 윈도우 크기. */
  static constexpr size_t kRollingWindow = 1000;

  /**
   * @brief ErrorBudgetTracker를 생성합니다.
   *
   * @param cfg         SLO 정책 설정.
   * @param action_name 이 추적기가 담당하는 액션 이름.
   */
  ErrorBudgetTracker(SloConfig cfg, std::string_view action_name)
      : cfg_(std::move(cfg)), action_name_(action_name) {}

  // ─── 기록 ────────────────────────────────────────────────────────────────

  /**
   * @brief 성공한 아이템 처리를 기록합니다.
   *
   * 레이턴시를 히스토그램에 기록하고 총 카운터를 증가시킵니다.
   *
   * @param latency 처리 소요 레이턴시.
   */
  void record_success(microseconds latency) noexcept {
    histogram_.record(latency);
    total_.fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief 에러를 기록합니다.
   *
   * 총 카운터와 에러 카운터를 증가시킵니다.
   */
  void record_error() noexcept {
    total_.fetch_add(1, std::memory_order_relaxed);
    errors_.fetch_add(1, std::memory_order_relaxed);
  }

  // ─── SLO 검사 ────────────────────────────────────────────────────────────

  /**
   * @brief SLO 위반 여부를 검사하고 위반 시 `SloConfig::on_violation`을 호출합니다.
   *
   * 다음 조건 중 하나라도 충족되면 위반으로 판단합니다:
   * 1. p99 레이턴시 > `SloConfig::p99_target`
   * 2. p99.9 레이턴시 > `SloConfig::p999_target`
   * 3. 에러율 > `SloConfig::error_budget`
   *
   * `on_violation`이 설정되지 않은 경우 아무것도 하지 않습니다.
   */
  void check_slo() {
    if (!cfg_.on_violation) return;

    bool violated = false;

    if (histogram_.p99()  > cfg_.p99_target)  violated = true;
    if (histogram_.p999() > cfg_.p999_target) violated = true;
    if (budget_exhausted())                    violated = true;

    if (violated) cfg_.on_violation(action_name_);
  }

  // ─── 조회 ────────────────────────────────────────────────────────────────

  /**
   * @brief 에러 버짓이 소진되었는지 확인합니다.
   *
   * 현재 에러율이 `SloConfig::error_budget`을 초과하면 true를 반환합니다.
   *
   * @returns 에러 버짓 소진 여부.
   */
  [[nodiscard]] bool budget_exhausted() const noexcept {
    return error_rate() > cfg_.error_budget;
  }

  /**
   * @brief 롤링 윈도우 기준 에러율을 반환합니다.
   *
   * 전체 누적 카운터에서 롤링 1000개 샘플 내 에러 비율을 근사합니다.
   * 총 처리 수가 0이면 0.0을 반환합니다.
   *
   * @returns 0.0 ~ 1.0 범위의 에러율.
   */
  [[nodiscard]] double error_rate() const noexcept {
    uint64_t total  = total_.load(std::memory_order_relaxed);
    uint64_t errors = errors_.load(std::memory_order_relaxed);
    if (total == 0) return 0.0;
    uint64_t window = std::min(total, static_cast<uint64_t>(kRollingWindow));
    return static_cast<double>(errors) / static_cast<double>(window);
  }

  /**
   * @brief 내부 레이턴시 히스토그램에 대한 const 참조를 반환합니다.
   * @returns 레이턴시 히스토그램 const 참조.
   */
  [[nodiscard]] const LatencyHistogram& histogram() const noexcept {
    return histogram_;
  }

  /**
   * @brief 총 처리 아이템 수를 반환합니다 (성공 + 에러).
   * @returns 누적 처리 수.
   */
  [[nodiscard]] uint64_t total_count() const noexcept {
    return total_.load(std::memory_order_relaxed);
  }

  /**
   * @brief 총 에러 수를 반환합니다.
   * @returns 누적 에러 수.
   */
  [[nodiscard]] uint64_t error_count() const noexcept {
    return errors_.load(std::memory_order_relaxed);
  }

  /**
   * @brief 모든 카운터와 히스토그램을 초기화합니다.
   */
  void reset() noexcept {
    total_.store(0, std::memory_order_relaxed);
    errors_.store(0, std::memory_order_relaxed);
    histogram_.reset();
  }

private:
  SloConfig             cfg_;
  std::string           action_name_;
  LatencyHistogram      histogram_;

  /** @brief 총 처리 아이템 수 (성공 + 에러). */
  std::atomic<uint64_t> total_{0};

  /** @brief 총 에러 수. */
  std::atomic<uint64_t> errors_{0};
};

// ─── SloObserver ──────────────────────────────────────────────────────────────

/**
 * @brief SLO 위반 이벤트를 지원하는 `PipelineObserver` 확장.
 *
 * `PipelineObserver`를 상속하여 기존 파이프라인 이벤트 훅을 모두 상속하면서,
 * SLO 위반 시 호출되는 `on_slo_violation()` 훅을 추가합니다.
 *
 * 구현체는 `on_slo_violation()`을 오버라이드하여
 * 알림 전송, 메트릭 기록, 서킷 브레이커 트리거 등을 수행할 수 있습니다.
 *
 * ### 스레드 안전성
 * `on_slo_violation()`은 여러 워커 스레드에서 동시에 호출될 수 있습니다.
 * 구현체는 내부 상태를 적절히 보호해야 합니다.
 *
 * ### 사용 예시
 * ```cpp
 * class MyObserver : public SloObserver {
 * public:
 *   void on_slo_violation(std::string_view action_name,
 *                          std::string_view metric_name,
 *                          double measured, double target) override {
 *     std::fprintf(stderr, "[SLO 위반] %s: %s %.2f > %.2f\n",
 *       std::string(action_name).c_str(),
 *       std::string(metric_name).c_str(),
 *       measured, target);
 *   }
 * };
 * ```
 */
class SloObserver : public PipelineObserver {
public:
  virtual ~SloObserver() = default;

  /**
   * @brief SLO 위반 발생 시 호출됩니다.
   *
   * @param action_name 위반이 감지된 액션 이름.
   * @param metric_name 위반된 지표 이름.
   *                    예: `"p99_latency"`, `"p999_latency"`, `"error_rate"`.
   * @param measured    측정된 값.
   *                    레이턴시 지표의 경우 마이크로초(µs),
   *                    에러율 지표의 경우 0.0 ~ 1.0 비율.
   * @param target      목표 임계값 (`measured`와 동일한 단위).
   */
  virtual void on_slo_violation(std::string_view /*action_name*/,
                                  std::string_view /*metric_name*/,
                                  double           /*measured*/,
                                  double           /*target*/) {}
};

// ─── LoggingSloObserver ───────────────────────────────────────────────────────

/**
 * @brief SLO 위반을 표준 에러에 로깅하는 기본 구현.
 *
 * 개발 및 디버깅 목적으로 사용합니다.
 * 출력 형식: `[qbuem/slo] violation: action=<이름> metric=<지표> measured=<값> target=<목표>`
 */
class LoggingSloObserver : public SloObserver {
public:
  /**
   * @brief SLO 위반을 표준 에러(`stderr`)에 출력합니다.
   *
   * @param action_name 위반이 감지된 액션 이름.
   * @param metric_name 위반된 지표 이름.
   * @param measured    측정된 값.
   * @param target      목표 임계값.
   */
  void on_slo_violation(std::string_view action_name,
                         std::string_view metric_name,
                         double           measured,
                         double           target) override {
    std::fprintf(stderr,
                 "[qbuem/slo] violation: action=%.*s metric=%.*s"
                 " measured=%.3f target=%.3f\n",
                 static_cast<int>(action_name.size()), action_name.data(),
                 static_cast<int>(metric_name.size()), metric_name.data(),
                 measured, target);
  }
};

} // namespace qbuem

/** @} */ // end of qbuem_slo
