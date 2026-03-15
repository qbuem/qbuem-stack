#pragma once

/**
 * @file qbuem/pipeline/observability.hpp
 * @brief 파이프라인 관찰 가능성 — ActionMetrics, PipelineMetrics, PipelineObserver
 * @defgroup qbuem_observability Pipeline Observability
 * @ingroup qbuem_pipeline
 *
 * 이 헤더는 파이프라인의 메트릭 수집 및 이벤트 훅 인프라를 제공합니다:
 *
 * - `ActionMetrics`    : 캐시 라인 정렬 원자 카운터 + 레이턴시 히스토그램
 * - `PipelineMetrics`  : 파이프라인 전체 집계 메트릭
 * - `PipelineObserver` : 이벤트 훅 인터페이스
 * - `LoggingObserver`  : 표준 출력 로깅 구현
 * - `NoopObserver`     : 제로 오버헤드 비활성 구현
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <cinttypes>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace qbuem {

// ─── HistogramMetrics ─────────────────────────────────────────────────────────

/**
 * @brief 사용자 정의 버킷 경계를 지원하는 레이턴시 히스토그램.
 *
 * `ActionMetrics`의 고정 4-버킷 히스토그램 대신 사용할 수 있으며
 * Prometheus-style cumulative histogram과 호환되는 구조입니다.
 *
 * ### 사용 예시
 * @code
 * // p50/p90/p99/p999에 최적화된 5-버킷 히스토그램
 * HistogramMetrics hist({500, 1000, 5000, 10000, 50000}); // µs 단위
 * hist.observe(latency_us);
 * auto counts = hist.bucket_counts(); // 각 버킷의 누적 카운트
 * @endcode
 *
 * @note 스레드 안전: `observe()`는 `std::atomic` relaxed 연산으로 구현됩니다.
 */
class HistogramMetrics {
public:
  /**
   * @brief 사용자 정의 상한 경계(µs)로 히스토그램을 생성합니다.
   *
   * @param upper_bounds µs 단위 버킷 상한값 목록 (오름차순 정렬).
   *                     마지막 버킷은 최대값 이상의 모든 관측값을 포함합니다.
   *
   * 예) `{1000, 10000, 100000}` → 버킷: [0,1ms), [1ms,10ms), [10ms,100ms), [100ms,∞)
   */
  explicit HistogramMetrics(std::initializer_list<uint64_t> upper_bounds)
      : bounds_(upper_bounds), buckets_(upper_bounds.size() + 1) {
    for (auto &b : buckets_) b.store(0, std::memory_order_relaxed);
  }

  explicit HistogramMetrics(std::vector<uint64_t> upper_bounds)
      : bounds_(std::move(upper_bounds)), buckets_(bounds_.size() + 1) {
    for (auto &b : buckets_) b.store(0, std::memory_order_relaxed);
  }

  /**
   * @brief 관측값을 히스토그램에 기록합니다.
   *
   * @param us 레이턴시 (마이크로초).
   */
  void observe(uint64_t us) noexcept {
    // Binary search for the first bucket whose upper bound >= us.
    auto it = std::lower_bound(bounds_.begin(), bounds_.end(), us);
    size_t idx = static_cast<size_t>(it - bounds_.begin());
    buckets_[idx].fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief 각 버킷의 카운트를 스냅샷으로 반환합니다.
   *
   * @returns 버킷 카운트 벡터. 크기 = `upper_bounds.size() + 1`.
   */
  [[nodiscard]] std::vector<uint64_t> bucket_counts() const noexcept {
    std::vector<uint64_t> counts(buckets_.size());
    for (size_t i = 0; i < buckets_.size(); ++i)
      counts[i] = buckets_[i].load(std::memory_order_relaxed);
    return counts;
  }

  /** @brief 버킷 상한 경계 목록을 반환합니다. */
  [[nodiscard]] const std::vector<uint64_t> &upper_bounds() const noexcept {
    return bounds_;
  }

  /** @brief 모든 카운터를 0으로 초기화합니다. */
  void reset() noexcept {
    for (auto &b : buckets_) b.store(0, std::memory_order_relaxed);
  }

private:
  std::vector<uint64_t>            bounds_;   ///< 버킷 상한 경계 (µs)
  std::vector<std::atomic<uint64_t>> buckets_; ///< 버킷별 카운터
};

// ─── ActionMetrics ────────────────────────────────────────────────────────────

/**
 * @brief 단일 액션의 성능 메트릭 (캐시 라인 정렬).
 *
 * `alignas(64)` 로 폴스 셰어링(false sharing)을 방지합니다.
 *
 * ### 레이턴시 버킷
 * - 버킷 0: < 1ms
 * - 버킷 1: 1ms ~ < 10ms
 * - 버킷 2: 10ms ~ < 100ms
 * - 버킷 3: >= 100ms
 */
struct alignas(64) ActionMetrics {
  /** @brief 처리 완료된 아이템 수. */
  std::atomic<uint64_t> items_processed{0};
  /** @brief 처리 중 발생한 에러 수. */
  std::atomic<uint64_t> errors{0};
  /** @brief 재시도 횟수. */
  std::atomic<uint64_t> retried{0};
  /** @brief 데드 레터 큐(DLQ)로 이동된 아이템 수. */
  std::atomic<uint64_t> dlq_count{0};

  /**
   * @brief 레이턴시 히스토그램 버킷 (4단계).
   *
   * - [0]: < 1,000 µs  (< 1ms)
   * - [1]: < 10,000 µs (< 10ms)
   * - [2]: < 100,000 µs (< 100ms)
   * - [3]: >= 100,000 µs (>= 100ms)
   */
  std::atomic<uint64_t> lat_buckets[4] = {};

  /**
   * @brief 사용자 정의 버킷 히스토그램 (선택 사항).
   *
   * 기본 4-버킷 히스토그램 대신 또는 함께 사용할 수 있습니다.
   * `nullptr`이면 사용 안 함.
   */
  std::shared_ptr<HistogramMetrics> histogram;

  /**
   * @brief 레이턴시를 고정 버킷과 사용자 정의 히스토그램 모두에 기록합니다.
   *
   * @param us 측정된 레이턴시 (마이크로초).
   */
  void record_latency_us(uint64_t us) noexcept {
    // Fixed 4-bucket histogram (backward compatible).
    if (us < 1000u)
      lat_buckets[0].fetch_add(1, std::memory_order_relaxed);
    else if (us < 10000u)
      lat_buckets[1].fetch_add(1, std::memory_order_relaxed);
    else if (us < 100000u)
      lat_buckets[2].fetch_add(1, std::memory_order_relaxed);
    else
      lat_buckets[3].fetch_add(1, std::memory_order_relaxed);
    // User-configurable histogram (if attached).
    if (histogram) histogram->observe(us);
  }

  /**
   * @brief 모든 카운터를 0으로 초기화합니다.
   */
  void reset() noexcept {
    items_processed.store(0, std::memory_order_relaxed);
    errors.store(0, std::memory_order_relaxed);
    retried.store(0, std::memory_order_relaxed);
    dlq_count.store(0, std::memory_order_relaxed);
    for (auto &b : lat_buckets)
      b.store(0, std::memory_order_relaxed);
    if (histogram) histogram->reset();
  }
};

// ─── PipelineMetrics ──────────────────────────────────────────────────────────

/**
 * @brief 파이프라인 전체 집계 메트릭.
 *
 * 각 액션의 `ActionMetrics`를 이름과 함께 보관합니다.
 */
struct PipelineMetrics {
  /** @brief 파이프라인 이름. */
  std::string name;
  /** @brief 액션별 메트릭 목록 (액션 이름, 메트릭 참조). */
  std::vector<std::pair<std::string, ActionMetrics>> actions;

  /**
   * @brief 모든 액션의 처리 아이템 수 합계를 반환합니다.
   */
  [[nodiscard]] uint64_t total_processed() const noexcept {
    uint64_t total = 0;
    for (const auto &[name, m] : actions)
      total += m.items_processed.load(std::memory_order_relaxed);
    return total;
  }

  /**
   * @brief 모든 액션의 에러 수 합계를 반환합니다.
   */
  [[nodiscard]] uint64_t total_errors() const noexcept {
    uint64_t total = 0;
    for (const auto &[name, m] : actions)
      total += m.errors.load(std::memory_order_relaxed);
    return total;
  }

  /**
   * @brief 에러율을 반환합니다 (처리된 아이템이 없으면 0.0).
   *
   * @returns `total_errors() / total_processed()`. 처리 아이템 0이면 0.0.
   */
  [[nodiscard]] double error_rate() const noexcept {
    uint64_t processed = total_processed();
    if (processed == 0) return 0.0;
    return static_cast<double>(total_errors()) /
           static_cast<double>(processed);
  }
};

// ─── PipelineObserver ─────────────────────────────────────────────────────────

/**
 * @brief 파이프라인 이벤트 훅 인터페이스.
 *
 * 모든 메서드는 기본 구현(아무것도 하지 않음)을 제공하므로
 * 관심 있는 이벤트만 오버라이드하면 됩니다.
 *
 * ### 스레드 안전성
 * 훅은 여러 워커 스레드에서 동시에 호출될 수 있습니다.
 * 구현체는 내부 상태를 적절히 보호해야 합니다.
 */
class PipelineObserver {
public:
  virtual ~PipelineObserver() = default;

  /**
   * @brief 아이템 처리 시작 시 호출됩니다.
   *
   * @param action_name 처리 중인 액션 이름.
   * @param item_id     아이템 고유 식별자.
   */
  virtual void on_item_start(std::string_view /*action_name*/,
                              uint64_t /*item_id*/) {}

  /**
   * @brief 아이템 처리 완료 시 호출됩니다.
   *
   * @param action_name 처리 완료된 액션 이름.
   * @param item_id     아이템 고유 식별자.
   * @param latency_us  처리 소요 시간 (마이크로초).
   */
  virtual void on_item_done(std::string_view /*action_name*/,
                             uint64_t /*item_id*/,
                             uint64_t /*latency_us*/) {}

  /**
   * @brief 처리 중 에러 발생 시 호출됩니다.
   *
   * @param action_name 에러가 발생한 액션 이름.
   * @param ec          에러 코드.
   */
  virtual void on_error(std::string_view /*action_name*/,
                         std::error_code /*ec*/) {}

  /**
   * @brief 워커 스케일 이벤트 발생 시 호출됩니다.
   *
   * @param action_name  스케일이 변경된 액션 이름.
   * @param old_workers  변경 전 워커 수.
   * @param new_workers  변경 후 워커 수.
   */
  virtual void on_scale_event(std::string_view /*action_name*/,
                               size_t /*old_workers*/,
                               size_t /*new_workers*/) {}

  /**
   * @brief 파이프라인 상태 전환 시 호출됩니다.
   *
   * @param pipeline_name 파이프라인 이름.
   * @param old_state     이전 상태 이름.
   * @param new_state     새 상태 이름.
   */
  virtual void on_state_change(std::string_view /*pipeline_name*/,
                                std::string_view /*old_state*/,
                                std::string_view /*new_state*/) {}

  /**
   * @brief 아이템이 DLQ로 이동될 때 호출됩니다.
   *
   * @param action_name DLQ를 트리거한 액션 이름.
   * @param ec          실패 원인 에러 코드.
   */
  virtual void on_dlq_item(std::string_view /*action_name*/,
                            std::error_code /*ec*/) {}

  /**
   * @brief 서킷 브레이커가 열릴 때 호출됩니다.
   *
   * @param action_name 서킷이 열린 액션 이름.
   */
  virtual void on_circuit_open(std::string_view /*action_name*/) {}

  /**
   * @brief 서킷 브레이커가 닫힐 때 호출됩니다.
   *
   * @param action_name 서킷이 닫힌 액션 이름.
   */
  virtual void on_circuit_close(std::string_view /*action_name*/) {}
};

// ─── LoggingObserver ──────────────────────────────────────────────────────────

/**
 * @brief 표준 출력에 이벤트를 로깅하는 기본 구현.
 *
 * 프로덕션보다는 개발 및 디버깅 목적으로 사용합니다.
 * 출력 형식: `[qbuem] <이벤트>: <내용>` (스레드 안전, `fprintf` 기반).
 */
class LoggingObserver : public PipelineObserver {
public:
  /**
   * @brief 아이템 처리 완료를 로깅합니다.
   *
   * @param action_name 완료된 액션 이름.
   * @param item_id     아이템 ID.
   * @param latency_us  레이턴시 (µs).
   */
  void on_item_done(std::string_view action_name,
                    uint64_t item_id,
                    uint64_t latency_us) override {
    std::fprintf(stderr,
                 "[qbuem] item_done: action=%.*s id=%" PRIu64
                 " latency=%" PRIu64 "us\n",
                 static_cast<int>(action_name.size()), action_name.data(),
                 item_id, latency_us);
  }

  /**
   * @brief 처리 에러를 로깅합니다.
   *
   * @param action_name 에러 발생 액션 이름.
   * @param ec          에러 코드.
   */
  void on_error(std::string_view action_name,
                std::error_code ec) override {
    std::fprintf(stderr,
                 "[qbuem] error: action=%.*s code=%d msg=%s\n",
                 static_cast<int>(action_name.size()), action_name.data(),
                 ec.value(), ec.message().c_str());
  }

  /**
   * @brief 스케일 이벤트를 로깅합니다.
   *
   * @param action_name 스케일이 변경된 액션 이름.
   * @param old_workers 이전 워커 수.
   * @param new_workers 새 워커 수.
   */
  void on_scale_event(std::string_view action_name,
                      size_t old_workers,
                      size_t new_workers) override {
    std::fprintf(stderr,
                 "[qbuem] scale: action=%.*s %zu -> %zu workers\n",
                 static_cast<int>(action_name.size()), action_name.data(),
                 old_workers, new_workers);
  }

  /**
   * @brief 파이프라인 상태 전환을 로깅합니다.
   *
   * @param pipeline_name 파이프라인 이름.
   * @param old_state     이전 상태.
   * @param new_state     새 상태.
   */
  void on_state_change(std::string_view pipeline_name,
                       std::string_view old_state,
                       std::string_view new_state) override {
    std::fprintf(stderr,
                 "[qbuem] state: pipeline=%.*s %.*s -> %.*s\n",
                 static_cast<int>(pipeline_name.size()), pipeline_name.data(),
                 static_cast<int>(old_state.size()),    old_state.data(),
                 static_cast<int>(new_state.size()),    new_state.data());
  }

  /**
   * @brief 서킷 브레이커 오픈을 로깅합니다.
   *
   * @param action_name 서킷이 열린 액션 이름.
   */
  void on_circuit_open(std::string_view action_name) override {
    std::fprintf(stderr,
                 "[qbuem] circuit_open: action=%.*s\n",
                 static_cast<int>(action_name.size()), action_name.data());
  }

  /**
   * @brief 서킷 브레이커 클로즈를 로깅합니다.
   *
   * @param action_name 서킷이 닫힌 액션 이름.
   */
  void on_circuit_close(std::string_view action_name) override {
    std::fprintf(stderr,
                 "[qbuem] circuit_close: action=%.*s\n",
                 static_cast<int>(action_name.size()), action_name.data());
  }
};

// ─── NoopObserver ─────────────────────────────────────────────────────────────

/**
 * @brief 관찰 가능성이 비활성화됐을 때 사용하는 제로 오버헤드 구현.
 *
 * 모든 메서드가 빈 가상 함수이므로 컴파일러가 완전히 인라인/제거할 수 있습니다.
 * `PipelineObserver*`를 nullptr로 두는 것 대신 이 클래스를 사용하면
 * 널 포인터 검사 없이 안전하게 호출할 수 있습니다.
 */
class NoopObserver : public PipelineObserver {};

} // namespace qbuem

/** @} */ // end of qbuem_observability
