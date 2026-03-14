#pragma once

/**
 * @file qbuem/tracing/span.hpp
 * @brief 단일 작업 단위를 나타내는 Span — SpanData, Span (RAII 래퍼).
 * @defgroup qbuem_tracing_span Span
 * @ingroup qbuem_tracing
 *
 * 분산 추적에서 Span은 하나의 작업 단위(operation)를 나타냅니다.
 * `SpanData`는 Span의 메타데이터를 저장하고, `Span`은 RAII 패턴으로
 * 소멸 시 자동으로 exporter에 기록합니다.
 *
 * ## 특징
 * - `SpanData::attributes`는 힙 할당 없이 고정 크기 배열을 사용합니다.
 * - `Span` 소멸자가 `end_time`을 기록하고 tracer에 export합니다.
 * - `Tracer`가 nullptr이면 export를 생략합니다 (noop).
 *
 * ## 사용 예시
 * @code
 * auto span = tracer.start_span("process_message", "ingest", "parse");
 * span.set_attribute("queue", "orders");
 * span.set_attribute("message_id", msg_id);
 * // ... 작업 수행 ...
 * span.set_status(SpanStatus::Ok);
 * // 스코프 종료 시 자동으로 export됨
 * @endcode
 * @{
 */

#include <qbuem/tracing/trace_context.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace qbuem::tracing {

// 전방 선언
class Tracer;

// ─── SpanStatus ───────────────────────────────────────────────────────────────

/**
 * @brief 스팬의 완료 상태.
 *
 * OpenTelemetry SpanStatus 규격을 따릅니다.
 * - `Unset`  : 명시적으로 설정되지 않음 (기본값).
 * - `Ok`     : 작업이 성공적으로 완료됨.
 * - `Error`  : 작업 중 오류 발생 (`error_message`에 상세 내용 저장).
 */
enum class SpanStatus {
  Ok,     ///< 작업 성공
  Error,  ///< 작업 실패 (error_message 참조)
  Unset,  ///< 상태 미설정 (기본값)
};

// ─── SpanData ─────────────────────────────────────────────────────────────────

/**
 * @brief Span 메타데이터 컨테이너.
 *
 * 힙 할당을 최소화하기 위해 attributes는 고정 크기 배열을 사용합니다.
 * `kMaxAttributes`(16개)를 초과하는 속성은 무시됩니다.
 *
 * ### 수명
 * `SpanData`는 값 타입입니다. `Span`이 소멸될 때 `end_time`이 설정되고
 * `Tracer::export_span()`에 복사본이 전달됩니다.
 */
struct SpanData {
  TraceId  trace_id;       ///< 128-bit 전역 추적 식별자
  SpanId   span_id;        ///< 이 스팬의 64-bit 식별자
  SpanId   parent_span_id; ///< 부모 스팬의 64-bit 식별자 (루트이면 무효값)

  std::string name;          ///< 작업 이름 (예: "process_message")
  std::string pipeline_name; ///< 스팬이 생성된 파이프라인 이름
  std::string action_name;   ///< 스팬이 생성된 액션 이름

  /** @brief 스팬 시작 시각 (UTC). */
  std::chrono::system_clock::time_point start_time;
  /** @brief 스팬 종료 시각 (UTC). Span 소멸 시 설정됨. */
  std::chrono::system_clock::time_point end_time;

  SpanStatus  status        = SpanStatus::Unset; ///< 완료 상태
  std::string error_message;                     ///< 오류 메시지 (status == Error일 때)

  // ── 속성 (key-value) ──────────────────────────────────────────────────────

  /** @brief 속성 최대 개수. 초과 시 set_attribute()가 무시됩니다. */
  static constexpr size_t kMaxAttributes = 16;

  /**
   * @brief 단일 key-value 속성.
   *
   * 두 멤버 모두 std::string을 사용합니다.
   * 빈 key는 유효하지 않은 슬롯을 나타냅니다.
   */
  struct Attribute {
    std::string key;   ///< 속성 키
    std::string value; ///< 속성 값
  };

  /** @brief 속성 배열 (힙 없는 고정 크기). */
  Attribute attributes[kMaxAttributes];

  /** @brief 현재 저장된 속성 개수. */
  size_t attribute_count = 0;

  /**
   * @brief 속성을 추가합니다.
   *
   * `kMaxAttributes`에 도달하면 추가 속성은 무시됩니다.
   * 동일한 key가 이미 있으면 값을 덮어씁니다.
   *
   * @param key   속성 키 (비어있으면 무시).
   * @param value 속성 값.
   */
  void set_attribute(std::string_view key, std::string_view value) {
    if (key.empty()) return;

    // 기존 키 검색 — 덮어쓰기
    for (size_t i = 0; i < attribute_count; ++i) {
      if (attributes[i].key == key) {
        attributes[i].value = std::string(value);
        return;
      }
    }

    // 새 슬롯
    if (attribute_count >= kMaxAttributes) return;
    attributes[attribute_count].key   = std::string(key);
    attributes[attribute_count].value = std::string(value);
    ++attribute_count;
  }
};

// ─── Span ─────────────────────────────────────────────────────────────────────

/**
 * @brief 활성 스팬 — RAII 래퍼로 소멸 시 자동으로 export됩니다.
 *
 * `Span`은 이동 전용 타입입니다. 복사 생성자와 복사 대입 연산자는 삭제됩니다.
 * 소멸자가 호출되면 `end_time`을 기록하고 `tracer_->export_span(data_)`를
 * 호출합니다.
 *
 * `tracer_`가 nullptr이면 export를 생략합니다 (noop 동작).
 *
 * ### 이중 export 방지
 * `ended_` 플래그로 소멸자에서의 중복 export를 방지합니다.
 */
class Span {
public:
  /**
   * @brief SpanData와 Tracer 포인터로 Span을 구성합니다.
   *
   * @param data   스팬 메타데이터. 이동됩니다.
   * @param tracer export를 담당하는 Tracer 포인터 (소유권 없음, nullable).
   */
  Span(SpanData data, Tracer* tracer)
      : data_(std::move(data)), tracer_(tracer), ended_(false) {}

  /**
   * @brief 소멸 시 스팬을 종료하고 export합니다.
   *
   * `ended_`가 false이면 `end_time`을 현재 시각으로 설정하고
   * `tracer_->export_span(data_)`를 호출합니다.
   */
  ~Span();

  // 이동 가능, 복사 불가
  Span(Span&& other) noexcept
      : data_(std::move(other.data_)), tracer_(other.tracer_), ended_(other.ended_) {
    other.ended_ = true; // 원본 소멸 시 이중 export 방지
  }
  Span& operator=(Span&& other) noexcept {
    if (this != &other) {
      if (!ended_ && tracer_) {
        data_.end_time = std::chrono::system_clock::now();
        // export는 move 대입 전 명시적 flush가 필요한 경우 호출자 책임
      }
      data_        = std::move(other.data_);
      tracer_      = other.tracer_;
      ended_       = other.ended_;
      other.ended_ = true;
    }
    return *this;
  }

  Span(const Span&)            = delete;
  Span& operator=(const Span&) = delete;

  /**
   * @brief 스팬의 완료 상태를 설정합니다.
   *
   * @param s   완료 상태 (`SpanStatus::Ok` 또는 `SpanStatus::Error`).
   * @param msg 오류 메시지 (status == Error일 때 의미 있음).
   */
  void set_status(SpanStatus s, std::string_view msg = {}) {
    data_.status = s;
    if (!msg.empty())
      data_.error_message = std::string(msg);
  }

  /**
   * @brief 속성을 추가합니다.
   *
   * 내부적으로 `SpanData::set_attribute()`를 호출합니다.
   *
   * @param key   속성 키.
   * @param value 속성 값.
   */
  void set_attribute(std::string_view key, std::string_view value) {
    data_.set_attribute(key, value);
  }

  /**
   * @brief SpanData에 대한 const 참조를 반환합니다.
   * @returns 이 스팬의 메타데이터.
   */
  const SpanData& data() const noexcept { return data_; }

private:
  SpanData data_;    ///< 스팬 메타데이터
  Tracer*  tracer_;  ///< export 담당 Tracer (소유권 없음)
  bool     ended_;   ///< 이중 export 방지 플래그
};

} // namespace qbuem::tracing

/** @} */
