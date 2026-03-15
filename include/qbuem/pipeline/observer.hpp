#pragma once
/**
 * @file qbuem/pipeline/observer.hpp
 * @brief 파이프라인 관찰 가능성 — ActionMetrics, PipelineMetrics, PipelineObserver
 * @defgroup qbuem_observer Observer
 * @ingroup qbuem_pipeline
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/pipeline/context.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace qbuem {

/**
 * @brief 액션 단위 메트릭 — cache-line 정렬
 */
struct ActionMetrics {
  alignas(64)
  std::atomic<uint64_t> items_processed{0};  ///< 처리된 아이템 수
  std::atomic<uint64_t> errors{0};           ///< 에러 수
  std::atomic<uint64_t> retried{0};          ///< 재시도 수
  std::atomic<uint64_t> dlq{0};             ///< DLQ로 전송된 수

  // 레이턴시 버킷 (4구간: <1ms, <10ms, <100ms, >=100ms)
  std::atomic<uint64_t> latency_lt1ms{0};
  std::atomic<uint64_t> latency_lt10ms{0};
  std::atomic<uint64_t> latency_lt100ms{0};
  std::atomic<uint64_t> latency_ge100ms{0};

  void record_latency(std::chrono::microseconds us) {
    auto ms = us.count() / 1000;
    if (ms < 1) latency_lt1ms.fetch_add(1, std::memory_order_relaxed);
    else if (ms < 10) latency_lt10ms.fetch_add(1, std::memory_order_relaxed);
    else if (ms < 100) latency_lt100ms.fetch_add(1, std::memory_order_relaxed);
    else latency_ge100ms.fetch_add(1, std::memory_order_relaxed);
  }
};

/**
 * @brief 파이프라인 단위 집계 메트릭
 */
struct PipelineMetrics {
  std::string_view name;
  uint64_t items_in  = 0;   ///< 입력 아이템 총 수
  uint64_t items_out = 0;   ///< 출력 아이템 총 수
  uint64_t errors    = 0;   ///< 총 에러 수
  uint64_t dlq       = 0;   ///< DLQ 총 수
};

// Forward declarations for observer hooks
template <typename In, typename Out> class Action;

/**
 * @brief PipelineObserver — 파이프라인 이벤트 훅 인터페이스
 */
class PipelineObserver {
public:
  virtual ~PipelineObserver() = default;

  virtual void on_item_start(std::string_view action_name, const Context& ctx) {}
  virtual void on_item_done(std::string_view action_name, const Context& ctx,
                            std::chrono::microseconds latency) {}
  virtual void on_error(std::string_view action_name, const Context& ctx,
                        std::error_code ec) {}
  virtual void on_scale_event(std::string_view action_name, size_t old_workers,
                              size_t new_workers) {}
  virtual void on_state_change(std::string_view pipeline_name,
                               std::string_view old_state, std::string_view new_state) {}
  virtual void on_dlq_item(std::string_view action_name, const Context& ctx,
                           std::error_code ec) {}
  virtual void on_circuit_open(std::string_view action_name) {}
  virtual void on_circuit_close(std::string_view action_name) {}
};

/**
 * @brief LoggingObserver — 기본 로깅 구현
 */
class LoggingObserver : public PipelineObserver {
public:
  explicit LoggingObserver(bool verbose = false) : verbose_(verbose) {}

  void on_item_done(std::string_view action_name, const Context&,
                    std::chrono::microseconds latency) override {
    if (verbose_) {
      // Log: action_name completed in latency.count() us
    }
  }

  void on_error(std::string_view action_name, const Context&,
                std::error_code ec) override {
    // Log: action_name error: ec.message()
  }

  void on_state_change(std::string_view pipeline_name,
                       std::string_view old_state, std::string_view new_state) override {
    // Log: pipeline_name: old_state -> new_state
  }

  void on_circuit_open(std::string_view action_name) override {
    // Log: WARN CircuitBreaker OPEN for action_name
  }

  void on_circuit_close(std::string_view action_name) override {
    // Log: INFO CircuitBreaker CLOSED for action_name
  }

private:
  bool verbose_;
};

} // namespace qbuem
/** @} */
