#pragma once

/**
 * @file qbuem/pipeline/context.hpp
 * @brief 파이프라인 아이템 컨텍스트 — 불변 persistent linked-list
 * @defgroup qbuem_pipeline_context Context
 * @ingroup qbuem_pipeline
 *
 * Context는 파이프라인 아이템의 메타데이터를 전달하는 불변 타입입니다.
 * 코루틴 프레임에 저장되어 co_await 경계에서도 안전하게 유지됩니다.
 *
 * ## 핵심 특성
 * - **불변(Immutable)**: `put()`은 새 Context를 반환, 원본 불변
 * - **O(1) 복사**: shared_ptr head 복사만 발생
 * - **O(1) put()**: 새 노드를 head에 추가 (linked-list prepend)
 * - **타입 인덱싱**: `std::type_index` 기반 슬롯 조회
 * - **코루틴 안전**: thread_local과 달리 프레임에 저장되므로 스레드 전환 후에도 유효
 *
 * ## 사용 예시
 * @code
 * Context ctx;
 * ctx = ctx.put(TraceCtx{...});
 * ctx = ctx.put(RequestId{"abc-123"});
 *
 * auto trace = ctx.get<TraceCtx>();  // std::optional<TraceCtx>
 * auto rid   = ctx.get_ptr<RequestId>(); // const RequestId* (복사 없음)
 * @endcode
 * @{
 */

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

namespace qbuem {

// ---------------------------------------------------------------------------
// 내장 Context 슬롯 타입 선언 (pipeline/trace_context.hpp 에서 정의)
// ---------------------------------------------------------------------------
struct TraceCtx;      ///< W3C TraceContext (trace_id, span_id, flags)
struct RequestId;     ///< HTTP 요청 고유 ID
struct AuthSubject;   ///< 인증된 사용자 ID
struct AuthRoles;     ///< 인증된 사용자 역할 목록
struct Deadline;      ///< 요청 처리 마감 시각
struct ActiveSpan;    ///< 현재 활성 추적 Span
struct EventTime;     ///< 이벤트 원본 발생 시각 (windowing용)
struct SagaId;        ///< Saga 분산 트랜잭션 ID
struct IdempotencyKey; ///< 멱등성 키

/**
 * @brief 파이프라인 아이템 메타데이터 컨테이너 (불변 persistent linked-list).
 *
 * ### 수명 규칙
 * Context는 값 타입입니다. `put()`으로 파생된 Context는 원본과 노드를 공유합니다.
 * fan-out 시 여러 Action이 동일 Context를 소유해도 안전합니다.
 *
 * ### 저장 패턴
 * ```
 * [head] → TraceCtx → RequestId → AuthSubject → nullptr
 * ```
 * `get<T>()` 호출 시 linked-list를 선형 탐색합니다.
 * 슬롯이 적을수록 탐색이 빠릅니다 (보통 5개 미만).
 */
class Context {
public:
  Context() noexcept = default;

  /**
   * @brief 새 슬롯을 추가한 Context를 반환합니다.
   *
   * 동일 타입 슬롯이 이미 있으면 head에 새로운 값으로 가려집니다.
   * (shadowing — 원본 노드는 유지됨)
   *
   * @tparam T 저장할 값의 타입.
   * @param  value 복사 또는 이동할 값.
   * @returns 새 슬롯이 추가된 Context.
   */
  template <typename T>
  [[nodiscard]] Context put(T value) const {
    auto node = std::make_shared<Node>();
    node->type_key = std::type_index(typeid(T));
    node->value    = std::make_shared<T>(std::move(value));
    node->next     = head_;
    Context result;
    result.head_   = std::move(node);
    return result;
  }

  /**
   * @brief 슬롯 값을 복사해서 반환합니다.
   *
   * @tparam T 조회할 타입.
   * @returns 값이 있으면 `std::optional<T>`, 없으면 `std::nullopt`.
   */
  template <typename T>
  [[nodiscard]] std::optional<T> get() const noexcept {
    const T *ptr = get_ptr<T>();
    if (!ptr)
      return std::nullopt;
    return *ptr;
  }

  /**
   * @brief 슬롯 값의 포인터를 반환합니다 (복사 없음).
   *
   * @tparam T 조회할 타입.
   * @returns 슬롯이 있으면 `const T*`, 없으면 `nullptr`.
   */
  template <typename T>
  [[nodiscard]] const T *get_ptr() const noexcept {
    const std::type_index key(typeid(T));
    for (const Node *n = head_.get(); n; n = n->next.get()) {
      if (n->type_key == key)
        return static_cast<const T *>(n->value.get());
    }
    return nullptr;
  }

  /**
   * @brief Context가 비어 있는지 확인합니다.
   */
  [[nodiscard]] bool empty() const noexcept { return !head_; }

private:
  struct Node {
    std::type_index              type_key{typeid(void)};
    std::shared_ptr<void>        value;
    std::shared_ptr<const Node>  next;
  };

  std::shared_ptr<const Node> head_;
};

// ---------------------------------------------------------------------------
// 내장 슬롯 정의
// ---------------------------------------------------------------------------

/** @brief W3C Trace Context (W3C traceparent 표준). */
struct TraceCtx {
  uint8_t trace_id[16]{};  ///< 128-bit trace identifier
  uint8_t span_id[8]{};    ///< 64-bit span identifier
  uint8_t flags = 0;       ///< trace-flags (bit 0 = sampled)
};

/** @brief HTTP 요청 고유 ID (UUID v4 형식 권장). */
struct RequestId {
  std::string value;
};

/** @brief 인증된 사용자 식별자. */
struct AuthSubject {
  std::string value;
};

/** @brief 인증된 사용자 역할 목록. */
struct AuthRoles {
  std::vector<std::string> values;
};

/** @brief 처리 데드라인. 초과 시 stop_token을 통해 취소 신호 전달. */
struct Deadline {
  std::chrono::steady_clock::time_point at;
};

/** @brief 활성 추적 Span ID (child span 생성용). */
struct ActiveSpan {
  uint8_t span_id[8]{};
};

/** @brief 이벤트 원본 발생 시각 (windowing, watermark에 사용). */
struct EventTime {
  std::chrono::system_clock::time_point at;
};

/** @brief Saga 분산 트랜잭션 식별자. */
struct SagaId {
  std::string value;
};

/** @brief 멱등성 키 — 중복 처리 방지용. */
struct IdempotencyKey {
  std::string value;
};

} // namespace qbuem

/** @} */
