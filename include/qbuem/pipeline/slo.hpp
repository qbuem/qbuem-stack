#pragma once

/**
 * @file qbuem/pipeline/slo.hpp
 * @brief SLO(Service Level Objective) 추적 및 에러 버짓 관리
 * @defgroup qbuem_slo SLO Tracking
 * @ingroup qbuem_pipeline
 *
 * 파이프라인 액션의 레이턴시와 에러율을 실시간으로 측정하고
 * 사전 정의된 SLO 목표를 위반할 때 콜백을 호출합니다.
 *
 * ## 구성 요소
 * - `SloConfig` — p99/p999 목표, 에러 버짓, 위반 콜백
 * - `LatencyHistogram` — 롤링 윈도우 레이턴시 히스토그램 (1024 샘플)
 * - `ErrorBudgetTracker` — 에러율 추적 + SLO 위반 감지
 * - `SloObserver` — `PipelineObserver` 콜백 통합
 *
 * ## 사용 예시
 * ```cpp
 * SloConfig cfg;
 * cfg.p99_target   = 10ms;
 * cfg.error_budget = 0.001;   // 0.1% 허용 에러
 * cfg.on_violation = [](auto name, auto metric, auto measured, auto target) {
 *   std::cerr << name << " SLO 위반: " << metric << "\n";
 * };
 *
 * ErrorBudgetTracker tracker(cfg, "my_action");
 * tracker.record_success(5ms);   // 정상 처리
 * tracker.record_error();         // 에러 기록
 * tracker.check_slo();            // 위반 여부 검사 → on_violation 호출
 * ```
 * @{
 */

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace qbuem {

using std::chrono::microseconds;

// ---------------------------------------------------------------------------
// SloConfig
// ---------------------------------------------------------------------------

/**
 * @brief SLO 목표 설정 구조체.
 */
struct SloConfig {
  /// p99 레이턴시 목표 (기본 10ms).
  microseconds p99_target{10'000};

  /// p999 레이턴시 목표 (기본 50ms).
  microseconds p999_target{50'000};

  /// 허용 에러 비율 (0.0 ~ 1.0, 기본 0.1%).
  double error_budget{0.001};

  /**
   * @brief SLO 위반 시 호출되는 콜백.
   * @param action_name 위반이 발생한 액션 이름.
   * @param metric_name 위반된 지표 이름 ("p99", "p999", "error_rate").
   * @param measured_us 측정값 (µs 또는 비율의 경우 0).
   * @param target_us   목표값.
   */
  std::function<void(std::string_view action_name,
                     std::string_view metric_name,
                     uint64_t measured_us,
                     uint64_t target_us)> on_violation;
};

// ---------------------------------------------------------------------------
// LatencyHistogram — 롤링 윈도우 레이턴시 히스토그램
// ---------------------------------------------------------------------------

/**
 * @brief 롤링 윈도우 레이턴시 히스토그램.
 *
 * 최근 kWindow(1024)개의 샘플을 링 버퍼에 유지합니다.
 * p99, p999는 정렬된 복사본에서 백분위수를 계산합니다.
 *
 * ### 4구간 빠른 분류
 * - Bucket 0: < 1ms
 * - Bucket 1: < 10ms
 * - Bucket 2: < 100ms
 * - Bucket 3: ≥ 100ms
 */
class LatencyHistogram {
public:
  static constexpr size_t kWindow = 1024;

  /// @brief 레이턴시 샘플 하나를 기록합니다 (O(1)).
  void record(microseconds latency) noexcept {
    size_t idx = head_.fetch_add(1, std::memory_order_relaxed) % kWindow;
    samples_[idx] = static_cast<uint32_t>(
        std::min(latency.count(), static_cast<long long>(UINT32_MAX)));

    size_t c = count_.load(std::memory_order_relaxed);
    if (c < kWindow) count_.fetch_add(1, std::memory_order_relaxed);

    // 4구간 빠른 분류 카운터 갱신
    uint64_t us = static_cast<uint64_t>(latency.count());
    if      (us <   1'000) ++bucket_[0];
    else if (us <  10'000) ++bucket_[1];
    else if (us < 100'000) ++bucket_[2];
    else                   ++bucket_[3];
  }

  /// @brief p99 레이턴시를 반환합니다.
  [[nodiscard]] microseconds p99() const { return percentile(99); }

  /// @brief p999 레이턴시를 반환합니다.
  [[nodiscard]] microseconds p999() const { return percentile(999); }

  /// @brief 4구간 카운터를 반환합니다 {<1ms, <10ms, <100ms, ≥100ms}.
  [[nodiscard]] std::array<uint64_t, 4> bucket_counts() const noexcept {
    return {bucket_[0].load(), bucket_[1].load(),
            bucket_[2].load(), bucket_[3].load()};
  }

  /// @brief 히스토그램을 초기화합니다.
  void reset() noexcept {
    head_.store(0);
    count_.store(0);
    for (auto &b : bucket_) b.store(0);
    samples_.fill(0);
  }

private:
  [[nodiscard]] microseconds percentile(int pct) const {
    size_t n = count_.load(std::memory_order_relaxed);
    if (n == 0) return microseconds{0};

    // 현재 윈도우 복사
    std::array<uint32_t, kWindow> buf{};
    size_t head = head_.load(std::memory_order_relaxed) % kWindow;
    for (size_t i = 0; i < n; ++i)
      buf[i] = samples_[(head + kWindow - n + i) % kWindow];

    std::sort(buf.begin(), buf.begin() + n);

    // p99: index = ceil(n * pct / 1000) - 1
    size_t idx = static_cast<size_t>(
        std::ceil(static_cast<double>(n) * pct / 1000.0));
    if (idx >= n) idx = n - 1;
    return microseconds{buf[idx]};
  }

  alignas(64) std::atomic<size_t>  head_{0};
  alignas(64) std::atomic<size_t>  count_{0};
  std::array<uint32_t, kWindow>    samples_{};
  std::array<std::atomic<uint64_t>, 4> bucket_{};
};

// ---------------------------------------------------------------------------
// ErrorBudgetTracker
// ---------------------------------------------------------------------------

/**
 * @brief 에러 버짓 추적기.
 *
 * 롤링 윈도우(최근 1000건) 기준 에러율을 측정하고
 * `check_slo()` 호출 시 `SloConfig::on_violation`을 호출합니다.
 */
class ErrorBudgetTracker {
public:
  static constexpr size_t kRollingWindow = 1000;

  /**
   * @brief 트래커를 생성합니다.
   * @param cfg         SLO 목표 설정.
   * @param action_name 추적할 액션 이름 (위반 콜백에 전달).
   */
  ErrorBudgetTracker(SloConfig cfg, std::string_view action_name)
      : cfg_(std::move(cfg)), action_name_(action_name) {}

  // -------------------------------------------------------------------------
  // 기록
  // -------------------------------------------------------------------------

  /// @brief 성공 처리를 기록합니다.
  void record_success(microseconds latency) noexcept {
    histogram_.record(latency);
    ++total_;
  }

  /// @brief 에러 발생을 기록합니다.
  void record_error() noexcept {
    ++total_;
    ++errors_;
  }

  // -------------------------------------------------------------------------
  // SLO 검사
  // -------------------------------------------------------------------------

  /**
   * @brief SLO 위반 여부를 검사하고 위반 시 콜백을 호출합니다.
   *
   * 다음 조건을 검사합니다:
   * 1. p99 레이턴시 > p99_target
   * 2. p999 레이턴시 > p999_target
   * 3. 에러율 > error_budget
   */
  void check_slo() {
    if (!cfg_.on_violation) return;

    // p99 검사
    auto p99 = histogram_.p99();
    if (p99 > cfg_.p99_target) {
      cfg_.on_violation(action_name_, "p99",
                        static_cast<uint64_t>(p99.count()),
                        static_cast<uint64_t>(cfg_.p99_target.count()));
    }

    // p999 검사
    auto p999 = histogram_.p999();
    if (p999 > cfg_.p999_target) {
      cfg_.on_violation(action_name_, "p999",
                        static_cast<uint64_t>(p999.count()),
                        static_cast<uint64_t>(cfg_.p999_target.count()));
    }

    // 에러율 검사
    double rate = error_rate();
    if (rate > cfg_.error_budget) {
      cfg_.on_violation(action_name_, "error_rate",
                        static_cast<uint64_t>(rate * 1'000'000),  // ppm
                        static_cast<uint64_t>(cfg_.error_budget * 1'000'000));
    }
  }

  // -------------------------------------------------------------------------
  // 조회
  // -------------------------------------------------------------------------

  /// @brief 에러 버짓이 소진되었으면 true.
  [[nodiscard]] bool budget_exhausted() const noexcept {
    return error_rate() >= cfg_.error_budget;
  }

  /// @brief 현재 에러율 (0.0 ~ 1.0). 롤링 윈도우 기준.
  [[nodiscard]] double error_rate() const noexcept {
    uint64_t t = total_.load(std::memory_order_relaxed);
    return (t == 0) ? 0.0
                    : static_cast<double>(errors_.load()) /
                          static_cast<double>(std::min(t, uint64_t{kRollingWindow}));
  }

  [[nodiscard]] const LatencyHistogram &histogram() const noexcept {
    return histogram_;
  }

  void reset() {
    total_.store(0);
    errors_.store(0);
    histogram_.reset();
  }

private:
  SloConfig           cfg_;
  std::string         action_name_;
  LatencyHistogram    histogram_;
  std::atomic<uint64_t> total_{0};
  std::atomic<uint64_t> errors_{0};
};

// ---------------------------------------------------------------------------
// SloObserver — PipelineObserver 통합
// ---------------------------------------------------------------------------

/**
 * @brief SLO 위반을 `PipelineObserver` 콜백으로 전달하는 어댑터.
 *
 * `on_slo_violation()` 을 호출하면 등록된 핸들러를 통지합니다.
 */
class SloObserver {
public:
  using ViolationHandler =
      std::function<void(std::string_view action,
                         std::string_view metric,
                         uint64_t measured_us,
                         uint64_t target_us)>;

  explicit SloObserver(ViolationHandler handler)
      : handler_(std::move(handler)) {}

  /// @brief SLO 위반을 기록합니다.
  void on_slo_violation(std::string_view action,
                        std::string_view metric,
                        uint64_t measured_us,
                        uint64_t target_us) {
    if (handler_) handler_(action, metric, measured_us, target_us);
  }

  /**
   * @brief SloConfig::on_violation 에 주입할 콜백 함수를 반환합니다.
   * @param action_name 이 Observer가 감시할 액션 이름.
   */
  auto make_violation_fn(std::string_view action_name) {
    return [this, name = std::string(action_name)](
               std::string_view /*action*/,
               std::string_view metric,
               uint64_t measured,
               uint64_t target) {
      on_slo_violation(name, metric, measured, target);
    };
  }

private:
  ViolationHandler handler_;
};

/** @} */

} // namespace qbuem
