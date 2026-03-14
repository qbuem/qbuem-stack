#pragma once

/**
 * @file qbuem/pipeline/action_env.hpp
 * @brief Action 실행 환경 — ActionEnv, ContextualItem, WorkerLocal
 * @defgroup qbuem_action_env ActionEnv
 * @ingroup qbuem_pipeline
 *
 * Action 함수에 전달되는 실행 컨텍스트와 관련 타입들을 정의합니다.
 * @{
 */

#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/service_registry.hpp>

#include <cassert>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <vector>

namespace qbuem {

/**
 * @brief Action 함수에 전달되는 실행 환경.
 *
 * `Action::Fn` 서명:
 * ```cpp
 * Task<Result<Out>>(In item, ActionEnv env)
 * ```
 *
 * ### 필드 설명
 * - `ctx`        : 아이템 메타데이터 (TraceCtx, RequestId 등)
 * - `stop`       : 파이프라인/액션 취소 신호
 * - `worker_idx` : 0-based 워커 인덱스 (`WorkerLocal<T>` 접근에 사용)
 * - `registry`   : 파이프라인 스코프 ServiceRegistry
 *
 * ### ⚠️ thread_local 금지
 * 코루틴은 `co_await` 후 다른 스레드에서 재개될 수 있습니다.
 * 아이템별 상태는 반드시 `ctx` 슬롯을 통해 전달하세요.
 */
struct ActionEnv {
  Context          ctx;        ///< 아이템 컨텍스트 (불변)
  std::stop_token  stop;       ///< 취소 신호 (stop() 시 요청됨)
  size_t           worker_idx; ///< 현재 워커 인덱스 (0-based)
  ServiceRegistry *registry;   ///< 파이프라인 스코프 레지스트리 (non-null)
};

/**
 * @brief 채널 전송 단위 — 아이템 + Context 쌍.
 *
 * 채널 내부에서는 `ContextualItem<T>`가 전송됩니다.
 * Action 함수는 값(`T`)만 받고 `ActionEnv`를 통해 컨텍스트에 접근합니다.
 *
 * fan-out 시 `ctx`는 `fork()`로 복사됩니다 (O(1) shared_ptr copy).
 *
 * @tparam T 아이템 값의 타입.
 */
template <typename T>
struct ContextualItem {
  T       value;  ///< 아이템 값
  Context ctx;    ///< 아이템 메타데이터
};

/**
 * @brief 워커별 상태 저장소 — 락 없는 워커 로컬 데이터.
 *
 * `alignas(64)` 슬롯으로 false sharing을 방지합니다.
 * `env.worker_idx`로 접근하므로 뮤텍스가 필요 없습니다.
 *
 * ## 적합한 사용 사례
 * - 워커별 RNG (예: std::mt19937)
 * - 워커별 임시 버퍼 (예: 직렬화용 scratch buffer)
 * - 워커별 연결 풀 / 캐시
 *
 * ## ⚠️ 부적합한 사례
 * - 아이템별 상태 → `Context` 슬롯 사용
 * - 파이프라인 전역 상태 → `ServiceRegistry` 사용
 *
 * ## 사용 예시
 * @code
 * WorkerLocal<std::mt19937> rngs(dispatcher.thread_count());
 *
 * // Action 함수 내:
 * auto &rng = rngs[env.worker_idx];
 * auto val = std::uniform_int_distribution<>(0, 100)(rng);
 * @endcode
 *
 * @tparam T 워커별 저장 타입.
 */
template <typename T>
class WorkerLocal {
public:
  /**
   * @brief 지정한 워커 수로 초기화합니다.
   *
   * @param worker_count 워커 수 (보통 `Dispatcher::thread_count()`).
   */
  explicit WorkerLocal(size_t worker_count) {
    slots_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i)
      slots_.emplace_back();
  }

  /**
   * @brief 특정 워커의 슬롯에 접근합니다.
   *
   * @param worker_idx `env.worker_idx` 값.
   * @returns 해당 워커의 `T&` 참조.
   */
  T &operator[](size_t worker_idx) {
    assert(worker_idx < slots_.size());
    return slots_[worker_idx].value;
  }

  const T &operator[](size_t worker_idx) const {
    assert(worker_idx < slots_.size());
    return slots_[worker_idx].value;
  }

  size_t size() const noexcept { return slots_.size(); }

private:
  // alignas(64)로 false sharing 방지.
  // 컴파일러가 sizeof(Slot) % 64 == 0을 보장하도록 패딩을 자동 추가합니다.
  struct alignas(64) Slot {
    T value{};
  };

  std::vector<Slot> slots_;
};

} // namespace qbuem

/** @} */
