#pragma once

/**
 * @file qbuem/tracing/exporter.hpp
 * @brief 스팬 익스포터, 파이프라인 트레이서, 메트릭 익스포터 정의.
 * @defgroup qbuem_tracing_exporter Exporter
 * @ingroup qbuem_tracing
 *
 * 이 헤더는 분산 추적 시스템의 익스포트 파이프라인을 완성합니다:
 *
 * - `SpanExporter`          : 스팬 데이터를 소비하는 순수 가상 인터페이스
 * - `NoopSpanExporter`      : 아무것도 하지 않는 스팬 익스포터
 * - `LoggingSpanExporter`   : stderr에 사람이 읽을 수 있는 형식으로 출력하는 익스포터
 * - `Tracer`                : `Span`이 사용하는 기본 트레이서 (`span.hpp` 전방 선언 구현)
 * - `PipelineTracer`        : `SpanExporter`를 주입할 수 있는 전역 싱글턴 트레이서
 * - `IMetricsExporter`      : Prometheus 푸시 추상화 인터페이스
 * - `PrometheusTextExporter`: 인메모리 Prometheus 텍스트 포맷 생성기
 * - `TraceContextSlot`      : `qbuem::Context`에 `TraceContext`를 저장하기 위한 슬롯 타입
 *
 * ## 사용 예시
 * @code
 * // 글로벌 트레이서에 로깅 익스포터 설정
 * auto tracer = std::make_unique<qbuem::tracing::PipelineTracer>();
 * tracer->set_exporter(std::make_shared<qbuem::tracing::LoggingSpanExporter>());
 * qbuem::tracing::PipelineTracer::set_global_tracer(std::move(tracer));
 *
 * // 스팬 생성
 * auto& pt = qbuem::tracing::PipelineTracer::global();
 * auto span = pt.start_span("process", "ingest", "parse");
 * span.set_status(qbuem::tracing::SpanStatus::Ok);
 * // 스코프 종료 시 자동 export
 * @endcode
 * @{
 */

#include <qbuem/tracing/span.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

// ─── SpanExporter ─────────────────────────────────────────────────────────────

namespace qbuem::tracing {

/**
 * @brief 스팬 데이터를 소비하는 순수 가상 인터페이스.
 *
 * 구현체는 스팬 완료 시 `export_span()`을 통해 데이터를 수신합니다.
 * 기본적으로 `flush()`와 `shutdown()`은 아무 동작도 하지 않습니다.
 *
 * ### 구현 계약
 * - `export_span()`은 여러 스레드에서 동시에 호출될 수 있습니다.
 *   구현체는 내부적으로 동기화해야 합니다.
 * - `shutdown()` 호출 이후에는 `export_span()`이 호출되지 않아야 합니다.
 */
class SpanExporter {
public:
  virtual ~SpanExporter() = default;

  /**
   * @brief 완료된 스팬 데이터를 익스포트합니다.
   *
   * @param span 익스포트할 스팬 메타데이터.
   */
  virtual void export_span(const SpanData& span) = 0;

  /**
   * @brief 버퍼링된 스팬 데이터를 강제로 플러시합니다.
   *
   * 기본 구현은 아무 동작도 하지 않습니다.
   */
  virtual void flush() {}

  /**
   * @brief 익스포터를 종료합니다.
   *
   * 종료 전 남은 데이터를 처리해야 합니다.
   * 기본 구현은 아무 동작도 하지 않습니다.
   */
  virtual void shutdown() {}
};

// ─── NoopSpanExporter ─────────────────────────────────────────────────────────

/**
 * @brief 아무것도 하지 않는 스팬 익스포터.
 *
 * 추적이 필요 없는 환경이나 테스트에서 사용합니다.
 * 모든 스팬 데이터를 즉시 폐기합니다.
 */
class NoopSpanExporter final : public SpanExporter {
public:
  /**
   * @brief 스팬을 조용히 폐기합니다.
   * @param span 무시할 스팬 데이터.
   */
  void export_span(const SpanData& /*span*/) override {}
};

// ─── LoggingSpanExporter ──────────────────────────────────────────────────────

/**
 * @brief 완료된 스팬을 stderr에 사람이 읽을 수 있는 형식으로 출력하는 익스포터.
 *
 * 개발 및 디버깅 목적으로 사용합니다. 각 스팬은 단일 줄로 출력됩니다.
 *
 * ### 출력 형식
 * ```
 * [SPAN] <name> pipeline=<pipeline> action=<action>
 *        trace=<trace_id> span=<span_id> parent=<parent_span_id>
 *        status=<Ok|Error|Unset> duration=<ms>ms
 *        [error: <message>]
 *        [attrs: key=value ...]
 * ```
 *
 * `export_span()`은 스레드 안전합니다 (내부 뮤텍스 사용).
 */
class LoggingSpanExporter final : public SpanExporter {
public:
  /**
   * @brief 스팬 데이터를 stderr에 출력합니다.
   *
   * @param span 출력할 스팬 메타데이터.
   */
  void export_span(const SpanData& span) override {
    // TraceId → hex 문자열
    char trace_buf[33];
    span.trace_id.to_chars(trace_buf, sizeof(trace_buf));

    // SpanId → hex 문자열
    char span_buf[17];
    span.span_id.to_chars(span_buf, sizeof(span_buf));

    // parent SpanId → hex 문자열
    char parent_buf[17];
    span.parent_span_id.to_chars(parent_buf, sizeof(parent_buf));

    // 지속 시간 계산 (밀리초)
    const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
        span.end_time - span.start_time).count();
    const double duration_ms = static_cast<double>(duration_us) / 1000.0;

    // 상태 문자열
    const char* status_str = "Unset";
    switch (span.status) {
      case SpanStatus::Ok:    status_str = "Ok";    break;
      case SpanStatus::Error: status_str = "Error"; break;
      case SpanStatus::Unset: status_str = "Unset"; break;
    }

    // Assemble output string.
    std::string out = std::format(
        "[SPAN] {} pipeline={} action={}\n"
        "       trace={} span={} parent={}\n"
        "       status={} duration={:.3f}ms",
        span.name, span.pipeline_name, span.action_name,
        trace_buf, span_buf, parent_buf,
        status_str, duration_ms);

    if (span.status == SpanStatus::Error && !span.error_message.empty()) {
      out += std::format("\n       error: {}", span.error_message);
    }

    if (span.attribute_count > 0) {
      out += "\n       attrs:";
      for (size_t i = 0; i < span.attribute_count; ++i) {
        out += std::format(" {}={}", span.attributes[i].key, span.attributes[i].value);
      }
    }

    out += '\n';

    {
      std::lock_guard<std::mutex> lk(mtx_);
      std::cerr << out;
    }
  }

private:
  mutable std::mutex mtx_; ///< stderr 출력 동기화 뮤텍스
};

// ─── Tracer ───────────────────────────────────────────────────────────────────

/**
 * @brief `Span`의 RAII 소멸자가 호출하는 기본 트레이서.
 *
 * `span.hpp`에서 전방 선언된 `Tracer` 클래스의 구체적 구현입니다.
 * `SpanExporter`를 통해 완료된 스팬을 외부로 전달합니다.
 *
 * 직접 사용하기보다는 `PipelineTracer`를 사용하는 것을 권장합니다.
 */
class Tracer {
public:
  /**
   * @brief 기본 생성자 — `NoopSpanExporter`를 사용합니다.
   */
  Tracer() : exporter_(std::make_shared<NoopSpanExporter>()) {}

  /**
   * @brief 특정 익스포터를 사용하는 Tracer를 생성합니다.
   *
   * @param exporter 스팬 완료 시 호출할 익스포터. nullptr이면 Noop으로 대체됩니다.
   */
  explicit Tracer(std::shared_ptr<SpanExporter> exporter)
      : exporter_(exporter ? std::move(exporter)
                           : std::make_shared<NoopSpanExporter>()) {}

  /**
   * @brief 완료된 스팬을 익스포터로 전달합니다.
   *
   * `Span` 소멸자에서 자동으로 호출됩니다.
   *
   * @param span 완료된 스팬 데이터.
   */
  void export_span(SpanData span) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (exporter_) {
      exporter_->export_span(span);
    }
  }

  /**
   * @brief 새 스팬을 시작합니다.
   *
   * @param name          작업 이름.
   * @param pipeline_name 파이프라인 이름.
   * @param action_name   액션 이름.
   * @param parent        부모 TraceContext (기본값: 빈 컨텍스트 → 루트 스팬 생성).
   * @returns RAII 스팬 객체. 소멸 시 자동으로 export됩니다.
   */
  Span start_span(std::string_view name,
                  std::string_view pipeline_name,
                  std::string_view action_name,
                  TraceContext parent = {}) {
    SpanData data;

    if (parent.trace_id.is_valid()) {
      // 부모 컨텍스트가 유효하면 child span으로 생성
      data.trace_id       = parent.trace_id;
      data.parent_span_id = parent.parent_span_id;
      data.span_id        = SpanId::generate();
    } else {
      // 루트 스팬: 새 TraceId와 SpanId 생성
      data.trace_id       = TraceId::generate();
      data.span_id        = SpanId::generate();
      // parent_span_id는 기본값(무효)으로 유지
    }

    data.name          = std::string(name);
    data.pipeline_name = std::string(pipeline_name);
    data.action_name   = std::string(action_name);
    data.start_time    = std::chrono::system_clock::now();

    return Span(std::move(data), this);
  }

  /**
   * @brief 현재 익스포터를 교체합니다.
   *
   * @param exporter 새 익스포터. nullptr이면 Noop으로 대체됩니다.
   */
  void set_exporter(std::shared_ptr<SpanExporter> exporter) {
    std::lock_guard<std::mutex> lk(mtx_);
    exporter_ = exporter ? std::move(exporter)
                         : std::make_shared<NoopSpanExporter>();
  }

private:
  std::shared_ptr<SpanExporter> exporter_; ///< 스팬 데이터를 소비하는 익스포터
  std::mutex                    mtx_;      ///< export 동기화 뮤텍스
};

// ─── Span 소멸자 정의 ──────────────────────────────────────────────────────────
// span.hpp에서 Tracer는 전방 선언만 되어 있으므로, Tracer가 완전히 정의된
// 이 지점에서 Span::~Span()을 정의합니다.

/**
 * @brief Span RAII 소멸자 — end_time을 기록하고 Tracer에 export합니다.
 *
 * `ended_` 플래그가 false인 경우에만 실행되어 이중 export를 방지합니다.
 */
inline Span::~Span() {
  if (!ended_ && tracer_) {
    data_.end_time = std::chrono::system_clock::now();
    tracer_->export_span(std::move(data_));
    ended_ = true;
  }
}

// ─── PipelineTracer ───────────────────────────────────────────────────────────

/**
 * @brief `SpanExporter` 주입을 지원하는 전역 싱글턴 트레이서.
 *
 * `PipelineTracer`는 `Tracer`를 래핑하여 전역 싱글턴 패턴을 제공합니다.
 * 애플리케이션 시작 시 `set_global_tracer()`로 구성하고,
 * 이후 `global()`을 통해 어디서든 접근할 수 있습니다.
 *
 * ### 스레드 안전성
 * - `global()` : 스레드 안전 (atomic 초기화 보장).
 * - `set_global_tracer()` : 프로그램 시작 시 단 한 번만 호출해야 합니다.
 * - `start_span()` / `end_span()` : 스레드 안전 (내부 뮤텍스 사용).
 * - `set_exporter()` : 스레드 안전.
 *
 * ### 사용 예시
 * @code
 * // 애플리케이션 초기화
 * auto pt = std::make_unique<PipelineTracer>();
 * pt->set_exporter(std::make_shared<LoggingSpanExporter>());
 * PipelineTracer::set_global_tracer(std::move(pt));
 *
 * // 스팬 생성
 * auto span = PipelineTracer::global().start_span("op", "pipeline", "action");
 * span.set_status(SpanStatus::Ok);
 * @endcode
 */
class PipelineTracer {
public:
  /**
   * @brief 기본 생성자 — `NoopSpanExporter`를 사용합니다.
   */
  PipelineTracer() : tracer_(std::make_shared<NoopSpanExporter>()) {}

  /**
   * @brief 전역 `PipelineTracer` 인스턴스에 접근합니다.
   *
   * 전역 트레이서가 `set_global_tracer()`로 설정되지 않은 경우,
   * 기본 `NoopSpanExporter`를 사용하는 트레이서가 반환됩니다.
   *
   * @returns 전역 PipelineTracer 참조.
   */
  static PipelineTracer& global() {
    static PipelineTracer default_instance;
    PipelineTracer* ptr = s_global_.load(std::memory_order_acquire);
    return ptr ? *ptr : default_instance;
  }

  /**
   * @brief 전역 `PipelineTracer`를 교체합니다.
   *
   * 이전 전역 트레이서는 소멸됩니다.
   * 프로그램 시작 시 단 한 번만 호출하는 것을 권장합니다.
   *
   * @param tracer 새 전역 트레이서. nullptr이면 기본 인스턴스로 복원됩니다.
   */
  static void set_global_tracer(std::unique_ptr<PipelineTracer> tracer) {
    std::lock_guard<std::mutex> lk(s_global_mtx_);
    delete s_global_.exchange(tracer.release(), std::memory_order_acq_rel);
  }

  /**
   * @brief 새 스팬을 시작합니다.
   *
   * 부모 컨텍스트가 유효하면 child span으로, 그렇지 않으면 루트 스팬으로 생성합니다.
   *
   * @param name          작업 이름.
   * @param pipeline_name 파이프라인 이름.
   * @param action_name   액션 이름.
   * @param parent        부모 TraceContext (기본값: 루트 스팬 생성).
   * @returns RAII 스팬 객체. 소멸 시 자동으로 `end_span()`을 호출합니다.
   */
  Span start_span(std::string_view name,
                  std::string_view pipeline_name,
                  std::string_view action_name,
                  TraceContext parent = {}) {
    std::lock_guard<std::mutex> lk(mtx_);
    return tracer_.start_span(name, pipeline_name, action_name, parent);
  }

  /**
   * @brief 완료된 스팬 데이터를 익스포터로 전달합니다.
   *
   * `Span` RAII 소멸자에서 내부적으로 호출됩니다.
   * 직접 호출할 필요는 없습니다.
   *
   * @param span_data 완료된 스팬 메타데이터.
   */
  void end_span(SpanData span_data) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::shared_ptr<SpanExporter> exp = exporter_;
    if (exp) {
      exp->export_span(span_data);
    }
  }

  /**
   * @brief 익스포터를 교체합니다.
   *
   * 새 익스포터는 이후 모든 스팬에 적용됩니다.
   * nullptr이면 `NoopSpanExporter`로 대체됩니다.
   *
   * @param exporter 새 스팬 익스포터.
   */
  void set_exporter(std::shared_ptr<SpanExporter> exporter) {
    auto new_exp = exporter ? std::move(exporter)
                            : std::make_shared<NoopSpanExporter>();
    {
      std::lock_guard<std::mutex> lk(mtx_);
      exporter_ = new_exp;
      tracer_.set_exporter(new_exp);
    }
  }

private:
  Tracer                        tracer_;   ///< 내부 Tracer (스팬 생성 및 export 담당)
  std::shared_ptr<SpanExporter> exporter_; ///< 현재 설정된 익스포터 (참조 보관용)
  std::mutex                    mtx_;      ///< 스레드 안전 접근 뮤텍스

  static std::atomic<PipelineTracer*> s_global_;  ///< 전역 싱글턴 포인터
  static std::mutex                   s_global_mtx_; ///< 전역 포인터 교체 뮤텍스
};

// 정적 멤버 정의
inline std::atomic<PipelineTracer*> PipelineTracer::s_global_{nullptr};
inline std::mutex                   PipelineTracer::s_global_mtx_{};

// ─── IMetricsExporter ─────────────────────────────────────────────────────────

/**
 * @brief Prometheus 푸시 메트릭 추상화 인터페이스.
 *
 * Gauge, Counter, Histogram 세 가지 메트릭 타입을 지원합니다.
 * Prometheus 레이블은 문자열 형식으로 전달됩니다.
 *
 * ### 레이블 형식
 * Prometheus 레이블 형식: `key="value",key2="value2"`
 *
 * ### 구현 계약
 * - 모든 메서드는 스레드 안전해야 합니다.
 * - `flush()` 호출 후 모든 메트릭이 외부로 전달됨을 보장해야 합니다.
 */
class IMetricsExporter {
public:
  virtual ~IMetricsExporter() = default;

  /**
   * @brief Gauge 메트릭을 설정합니다 (절댓값).
   *
   * 현재 상태를 나타내는 메트릭입니다 (예: 현재 큐 크기, 메모리 사용량).
   *
   * @param name   메트릭 이름 (Prometheus 네이밍 규칙: snake_case).
   * @param value  현재 게이지 값.
   * @param labels Prometheus 레이블 문자열 (예: `job="worker",env="prod"`).
   */
  virtual void gauge(std::string_view name,
                     double value,
                     std::string_view labels = "") = 0;

  /**
   * @brief Counter 메트릭을 증가시킵니다 (델타).
   *
   * 단조 증가하는 누적 메트릭입니다 (예: 처리된 메시지 수).
   *
   * @param name   메트릭 이름.
   * @param delta  증가량 (음수를 허용하지 않는 것을 권장).
   * @param labels Prometheus 레이블 문자열.
   */
  virtual void counter(std::string_view name,
                       double delta,
                       std::string_view labels = "") = 0;

  /**
   * @brief Histogram에 관측값을 기록합니다.
   *
   * 분포를 추적하는 메트릭입니다 (예: 요청 지연 시간, 페이로드 크기).
   *
   * @param name   메트릭 이름.
   * @param value  관측값.
   * @param labels Prometheus 레이블 문자열.
   */
  virtual void histogram(std::string_view name,
                         double value,
                         std::string_view labels = "") = 0;

  /**
   * @brief 버퍼링된 메트릭을 강제로 플러시합니다.
   *
   * 기본 구현은 아무 동작도 하지 않습니다.
   */
  virtual void flush() {}
};

// ─── PrometheusTextExporter ───────────────────────────────────────────────────

/**
 * @brief 인메모리 Prometheus 텍스트 포맷 메트릭 생성기.
 *
 * 메트릭 데이터를 메모리에 누적하고, `export_text()`를 통해
 * Prometheus 텍스트 exposition 포맷으로 반환합니다.
 *
 * ### Prometheus 텍스트 포맷 출력 예시
 * ```
 * # TYPE queue_depth gauge
 * queue_depth{job="worker"} 42.000000
 * # TYPE messages_processed counter
 * messages_processed_total{env="prod"} 1024.000000
 * # TYPE request_latency_ms histogram
 * request_latency_ms_sum{} 12345.678000
 * request_latency_ms_count{} 100
 * ```
 *
 * ### 스레드 안전성
 * 모든 메서드는 내부 뮤텍스로 보호됩니다.
 */
class PrometheusTextExporter final : public IMetricsExporter {
public:
  /**
   * @brief Gauge 메트릭을 버퍼에 기록합니다.
   *
   * @param name   메트릭 이름.
   * @param value  현재 게이지 값.
   * @param labels Prometheus 레이블 문자열.
   */
  void gauge(std::string_view name,
             double value,
             std::string_view labels = "") override {
    std::lock_guard<std::mutex> lk(mtx_);
    append_metric("gauge", name, value, labels);
  }

  /**
   * @brief Counter 메트릭을 버퍼에 누적합니다.
   *
   * 동일한 (name, labels) 조합의 counter는 누적됩니다.
   *
   * @param name   메트릭 이름.
   * @param delta  증가량.
   * @param labels Prometheus 레이블 문자열.
   */
  void counter(std::string_view name,
               double delta,
               std::string_view labels = "") override {
    std::lock_guard<std::mutex> lk(mtx_);
    const std::string key = make_key(name, labels);
    counter_values_[key] += delta;
    counter_meta_[key]    = {std::string(name), std::string(labels)};
  }

  /**
   * @brief Histogram에 관측값을 기록합니다.
   *
   * sum과 count를 누적합니다.
   *
   * @param name   메트릭 이름.
   * @param value  관측값.
   * @param labels Prometheus 레이블 문자열.
   */
  void histogram(std::string_view name,
                 double value,
                 std::string_view labels = "") override {
    std::lock_guard<std::mutex> lk(mtx_);
    const std::string key = make_key(name, labels);
    histogram_sum_[key]   += value;
    histogram_count_[key] += 1;
    histogram_meta_[key]   = {std::string(name), std::string(labels)};
  }

  /**
   * @brief 누적된 모든 메트릭을 Prometheus 텍스트 포맷으로 반환합니다.
   *
   * 반환 후 내부 버퍼는 초기화됩니다.
   *
   * @returns Prometheus exposition 텍스트 포맷 문자열.
   */
  [[nodiscard]] std::string export_text() {
    std::lock_guard<std::mutex> lk(mtx_);

    std::string out;
    out.reserve(raw_buffer_.size() + counter_values_.size() * 80
                                   + histogram_sum_.size() * 120);

    // Gauge / raw 버퍼
    out += raw_buffer_;
    raw_buffer_.clear();

    // Counter 출력
    for (const auto& [key, total] : counter_values_) {
      const auto& [metric_name, metric_labels] = counter_meta_.at(key);
      out += "# TYPE ";
      out += metric_name;
      out += "_total counter\n";
      out += metric_name;
      out += "_total";
      append_labels(out, metric_labels);
      out += ' ';
      out += std::to_string(total);
      out += '\n';
    }
    counter_values_.clear();
    counter_meta_.clear();

    // Histogram 출력
    for (const auto& [key, sum] : histogram_sum_) {
      const auto& [metric_name, metric_labels] = histogram_meta_.at(key);
      const double count = histogram_count_.at(key);

      out += "# TYPE ";
      out += metric_name;
      out += " histogram\n";

      out += metric_name;
      out += "_sum";
      append_labels(out, metric_labels);
      out += ' ';
      out += std::to_string(sum);
      out += '\n';

      out += metric_name;
      out += "_count";
      append_labels(out, metric_labels);
      out += ' ';
      out += std::to_string(static_cast<uint64_t>(count));
      out += '\n';
    }
    histogram_sum_.clear();
    histogram_count_.clear();
    histogram_meta_.clear();

    return out;
  }

  /**
   * @brief 내부 버퍼를 비웁니다 (메트릭 초기화).
   *
   * `IMetricsExporter::flush()` 재정의. 버퍼를 초기화하여 메모리를 해제합니다.
   */
  void flush() override {
    std::lock_guard<std::mutex> lk(mtx_);
    raw_buffer_.clear();
    counter_values_.clear();
    counter_meta_.clear();
    histogram_sum_.clear();
    histogram_count_.clear();
    histogram_meta_.clear();
  }

private:
  /**
   * @brief Gauge 메트릭을 raw 버퍼에 즉시 기록합니다 (내부 헬퍼).
   *
   * @param type   Prometheus 메트릭 타입 문자열.
   * @param name   메트릭 이름.
   * @param value  값.
   * @param labels 레이블 문자열.
   */
  void append_metric(std::string_view type,
                     std::string_view name,
                     double value,
                     std::string_view labels) {
    raw_buffer_ += "# TYPE ";
    raw_buffer_ += name;
    raw_buffer_ += ' ';
    raw_buffer_ += type;
    raw_buffer_ += '\n';
    raw_buffer_ += name;
    append_labels(raw_buffer_, labels);
    raw_buffer_ += ' ';
    raw_buffer_ += std::to_string(value);
    raw_buffer_ += '\n';
  }

  /**
   * @brief 레이블 문자열을 Prometheus 형식으로 출력 문자열에 추가합니다.
   *
   * 레이블이 비어 있으면 `{}`를 추가합니다.
   *
   * @param out    대상 문자열.
   * @param labels 레이블 문자열 (예: `job="worker"`).
   */
  static void append_labels(std::string& out, std::string_view labels) {
    out += '{';
    if (!labels.empty()) {
      out += labels;
    }
    out += '}';
  }

  /**
   * @brief (name, labels) 조합을 유일한 키 문자열로 변환합니다.
   *
   * @param name   메트릭 이름.
   * @param labels 레이블 문자열.
   * @returns 복합 키 문자열.
   */
  static std::string make_key(std::string_view name, std::string_view labels) {
    std::string key;
    key.reserve(name.size() + 1 + labels.size());
    key += name;
    key += '\x1F'; // ASCII unit separator
    key += labels;
    return key;
  }

  std::mutex  mtx_;         ///< 스레드 안전 접근 뮤텍스

  /// @brief Gauge 및 즉시 직렬화된 메트릭 버퍼
  std::string raw_buffer_;

  /// @brief Counter 누적값 (key → 합산 값)
  std::unordered_map<std::string, double> counter_values_;

  /// @brief Counter 메타데이터 (key → {name, labels})
  std::unordered_map<std::string, std::pair<std::string, std::string>> counter_meta_;

  /// @brief Histogram sum 누적값 (key → sum)
  std::unordered_map<std::string, double> histogram_sum_;

  /// @brief Histogram count 누적값 (key → count)
  std::unordered_map<std::string, double> histogram_count_;

  /// @brief Histogram 메타데이터 (key → {name, labels})
  std::unordered_map<std::string, std::pair<std::string, std::string>> histogram_meta_;
};

} // namespace qbuem::tracing

// ─── TraceContextSlot ─────────────────────────────────────────────────────────

namespace qbuem {

/**
 * @brief `qbuem::Context`에 `TraceContext`를 저장하기 위한 슬롯 타입.
 *
 * `qbuem::Context::put<TraceContextSlot>()`으로 TraceContext를 컨텍스트에 저장하고,
 * `qbuem::Context::get<TraceContextSlot>()`으로 조회합니다.
 *
 * ### 사용 예시
 * @code
 * qbuem::Context ctx;
 * auto trace_ctx = qbuem::tracing::TraceContext::generate();
 * ctx = ctx.put(qbuem::TraceContextSlot{trace_ctx});
 *
 * if (auto slot = ctx.get<qbuem::TraceContextSlot>()) {
 *     auto child = slot->value.child_span();
 * }
 * @endcode
 *
 * @note `qbuem::TraceCtx`는 원시 바이트 배열을 사용하는 반면,
 *       `TraceContextSlot`은 타입 안전한 `tracing::TraceContext`를 직접 보관합니다.
 */
struct TraceContextSlot {
  tracing::TraceContext value; ///< 추적 컨텍스트 (TraceId, SpanId, flags)
};

} // namespace qbuem

/** @} */
