#pragma once

/**
 * @file qbuem/pipeline/checkpoint.hpp
 * @brief DynamicPipeline 체크포인트 / 스냅샷 지원
 * @defgroup qbuem_checkpoint Checkpoint
 * @ingroup qbuem_pipeline
 *
 * 이 헤더는 DynamicPipeline의 오프셋과 메타데이터를 저장·복원하는
 * 체크포인트 인프라를 제공합니다:
 *
 * - `CheckpointData`         : 체크포인트 레코드 (오프셋 + 메타데이터 JSON + 저장 시각)
 * - `ICheckpointStore`       : 저장소 추상 인터페이스
 * - `InMemoryCheckpointStore`: 인메모리 참조 구현
 * - `CheckpointedPipeline<T>`: DynamicPipeline<T>를 래핑하는 체크포인트 지원 파이프라인
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qbuem {

using std::chrono::system_clock;

// ─── CheckpointData ───────────────────────────────────────────────────────────

/**
 * @brief 단일 체크포인트 레코드.
 *
 * 파이프라인이 처리한 아이템 오프셋, 사용자 정의 메타데이터 JSON,
 * 그리고 저장 시각을 담습니다.
 */
struct CheckpointData {
  /** @brief 저장 시점까지 처리된 아이템의 누적 오프셋. */
  uint64_t offset{0};

  /** @brief 사용자 정의 직렬화 메타데이터 (JSON 문자열). */
  std::string metadata_json;

  /** @brief 체크포인트가 저장된 시각. */
  system_clock::time_point saved_at;
};

// ─── ICheckpointStore ─────────────────────────────────────────────────────────

/**
 * @brief 체크포인트 저장소 추상 인터페이스.
 *
 * 파이프라인 이름을 키로 사용해 `CheckpointData`를 저장하고 불러옵니다.
 * 구현체는 인메모리, 파일 시스템, 원격 KV 스토어 등 다양할 수 있습니다.
 *
 * ### 스레드 안전성
 * 구현체는 `save`와 `load`를 동시에 안전하게 호출할 수 있어야 합니다.
 */
class ICheckpointStore {
public:
  virtual ~ICheckpointStore() = default;

  /**
   * @brief 체크포인트를 저장합니다.
   *
   * @param pipeline_name  파이프라인 식별자.
   * @param offset         처리된 아이템 누적 오프셋.
   * @param metadata_json  사용자 정의 메타데이터 JSON 문자열.
   * @returns 저장 성공 시 `Result<void>::ok()`, 실패 시 에러.
   */
  virtual Task<Result<void>> save(std::string_view pipeline_name,
                                   uint64_t offset,
                                   std::string_view metadata_json) = 0;

  /**
   * @brief 저장된 체크포인트를 불러옵니다.
   *
   * @param pipeline_name 파이프라인 식별자.
   * @returns 성공 시 `CheckpointData`, 데이터가 없거나 실패 시 에러.
   */
  virtual Task<Result<CheckpointData>> load(std::string_view pipeline_name) = 0;
};

// ─── InMemoryCheckpointStore ──────────────────────────────────────────────────

/**
 * @brief 인메모리 체크포인트 저장소.
 *
 * 프로세스 재시작 시 데이터는 소실됩니다.
 * 테스트 및 단위 개발 목적에 적합합니다.
 *
 * 내부적으로 `std::mutex`로 보호되는 해시맵을 사용하므로 스레드 안전합니다.
 */
class InMemoryCheckpointStore : public ICheckpointStore {
public:
  /**
   * @brief 체크포인트를 메모리에 저장합니다.
   *
   * @param pipeline_name  파이프라인 식별자.
   * @param offset         처리된 아이템 누적 오프셋.
   * @param metadata_json  사용자 정의 메타데이터 JSON 문자열.
   * @returns 항상 `Result<void>::ok()`.
   */
  Task<Result<void>> save(std::string_view pipeline_name,
                           uint64_t offset,
                           std::string_view metadata_json) override {
    CheckpointData data;
    data.offset        = offset;
    data.metadata_json = std::string(metadata_json);
    data.saved_at      = system_clock::now();

    {
      std::lock_guard lock(mutex_);
      store_[std::string(pipeline_name)] = std::move(data);
    }
    co_return Result<void>::ok();
  }

  /**
   * @brief 메모리에서 체크포인트를 불러옵니다.
   *
   * @param pipeline_name 파이프라인 식별자.
   * @returns 저장된 `CheckpointData`, 없으면 `errc::no_such_file_or_directory` 에러.
   */
  Task<Result<CheckpointData>> load(std::string_view pipeline_name) override {
    std::lock_guard lock(mutex_);
    auto it = store_.find(std::string(pipeline_name));
    if (it == store_.end()) {
      co_return unexpected(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }
    co_return it->second;
  }

  /**
   * @brief 저장된 체크포인트 수를 반환합니다.
   * @returns 저장소 내 항목 수.
   */
  [[nodiscard]] size_t size() const {
    std::lock_guard lock(mutex_);
    return store_.size();
  }

  /**
   * @brief 모든 체크포인트를 삭제합니다.
   */
  void clear() {
    std::lock_guard lock(mutex_);
    store_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CheckpointData> store_;
};

// ─── CheckpointedPipeline<T> ──────────────────────────────────────────────────

/**
 * @brief 체크포인트 기능을 갖춘 DynamicPipeline 래퍼.
 *
 * `DynamicPipeline<T>`를 내부에 보유하며, 아이템 처리 수를 추적하고
 * 주기적 또는 수동 체크포인트 저장·복원을 지원합니다.
 *
 * ### 사용 예시
 * ```cpp
 * auto store = std::make_shared<InMemoryCheckpointStore>();
 * CheckpointedPipeline<int> cp("my_pipeline", store);
 * cp.pipeline().add_stage("double", [](int x, ActionEnv) -> Task<Result<int>> {
 *     co_return x * 2;
 * });
 * cp.enable_checkpoint(std::chrono::seconds{30}, 500);
 * cp.pipeline().start(dispatcher);
 *
 * // 체크포인트 수동 저장
 * co_await cp.save_checkpoint();
 *
 * // 이전 체크포인트에서 재개 (오프셋을 복원해 외부 소스에서 재전송 시작점 결정)
 * co_await cp.resume_from_checkpoint();
 * ```
 *
 * @tparam T 파이프라인을 흐르는 메시지 타입.
 */
template <typename T>
class CheckpointedPipeline {
public:
  /**
   * @brief CheckpointedPipeline을 생성합니다.
   *
   * @param name  파이프라인 이름 (체크포인트 저장소 키로 사용됨).
   * @param store 체크포인트 저장소 (공유 소유권).
   * @param cfg   내부 DynamicPipeline 설정 (선택적).
   */
  CheckpointedPipeline(std::string name,
                        std::shared_ptr<ICheckpointStore> store,
                        typename DynamicPipeline<T>::Config cfg = {})
      : name_(std::move(name)),
        store_(std::move(store)),
        pipeline_(std::move(cfg)) {}

  CheckpointedPipeline(const CheckpointedPipeline&) = delete;
  CheckpointedPipeline& operator=(const CheckpointedPipeline&) = delete;
  CheckpointedPipeline(CheckpointedPipeline&&) = default;
  CheckpointedPipeline& operator=(CheckpointedPipeline&&) = default;

  // ─── 내부 파이프라인 접근 ───────────────────────────────────────────────────

  /**
   * @brief 내부 `DynamicPipeline<T>`에 대한 참조를 반환합니다.
   *
   * 스테이지 추가, 시작/정지 등 파이프라인 조작에 사용합니다.
   * @returns 내부 파이프라인 참조.
   */
  [[nodiscard]] DynamicPipeline<T>& pipeline() noexcept {
    return pipeline_;
  }

  /**
   * @brief 내부 `DynamicPipeline<T>`에 대한 const 참조를 반환합니다.
   * @returns 내부 파이프라인 const 참조.
   */
  [[nodiscard]] const DynamicPipeline<T>& pipeline() const noexcept {
    return pipeline_;
  }

  // ─── 체크포인트 설정 ────────────────────────────────────────────────────────

  /**
   * @brief 자동 체크포인트 정책을 설정합니다.
   *
   * 시간 간격(`every_t`)과 아이템 수(`every_n`) 중 먼저 충족되는 조건에서
   * 체크포인트를 저장합니다. `push_counted()`를 통해 아이템을 전달할 때
   * 자동 트리거 여부를 확인합니다.
   *
   * @param every_t 체크포인트 저장 주기 (기본 60초).
   * @param every_n 체크포인트 저장 아이템 수 간격 (기본 1000개).
   */
  void enable_checkpoint(std::chrono::seconds every_t = std::chrono::seconds{60},
                          size_t every_n = 1000) {
    checkpoint_interval_t_ = every_t;
    checkpoint_interval_n_ = every_n;
    checkpoint_enabled_    = true;
    last_checkpoint_time_  = system_clock::now();
  }

  // ─── 아이템 전송 (카운터 포함) ──────────────────────────────────────────────

  /**
   * @brief 아이템을 파이프라인에 전달하고 처리 카운터를 증가시킵니다.
   *
   * 체크포인트가 활성화된 경우, 아이템 수 또는 시간 조건이 충족되면
   * 자동으로 `save_checkpoint()`를 호출합니다.
   *
   * @param item 전달할 아이템.
   * @param ctx  아이템 컨텍스트 (기본: 빈 Context).
   * @param metadata_json 체크포인트 메타데이터 JSON (기본: 빈 문자열).
   * @returns `Result<void>::ok()` 또는 push/checkpoint 에러.
   */
  Task<Result<void>> push_counted(T item,
                                   Context ctx = {},
                                   std::string_view metadata_json = "") {
    auto result = co_await pipeline_.push(std::move(item), std::move(ctx));
    if (!result.has_value()) co_return result;

    uint64_t count = items_processed_.fetch_add(1, std::memory_order_relaxed) + 1;

    if (checkpoint_enabled_) {
      bool by_count = (checkpoint_interval_n_ > 0) &&
                      (count % checkpoint_interval_n_ == 0);

      bool by_time = false;
      {
        auto now = system_clock::now();
        std::lock_guard lock(time_mutex_);
        if (now - last_checkpoint_time_ >= checkpoint_interval_t_) {
          by_time = true;
          last_checkpoint_time_ = now;
        }
      }

      if (by_count || by_time) {
        auto cp_result = co_await save_checkpoint(metadata_json);
        if (!cp_result.has_value()) co_return cp_result;
      }
    }

    co_return Result<void>::ok();
  }

  // ─── 수동 체크포인트 조작 ───────────────────────────────────────────────────

  /**
   * @brief 현재 오프셋과 메타데이터를 즉시 저장소에 저장합니다.
   *
   * @param metadata_json 저장할 사용자 메타데이터 JSON 문자열 (기본: 빈 문자열).
   * @returns 저장 성공 시 `Result<void>::ok()`, 실패 시 에러.
   */
  Task<Result<void>> save_checkpoint(std::string_view metadata_json = "") {
    uint64_t offset = items_processed_.load(std::memory_order_acquire);
    co_return co_await store_->save(name_, offset, metadata_json);
  }

  /**
   * @brief 저장소에서 최근 체크포인트를 불러와 오프셋을 복원합니다.
   *
   * 복원된 오프셋은 `items_processed()`로 조회할 수 있으며,
   * 외부 소스(예: Kafka, 파일)에서 해당 오프셋부터 재전송을 시작하는
   * 기준점으로 활용합니다.
   *
   * @returns 복원 성공 시 `Result<void>::ok()`, 저장된 데이터 없음 또는 실패 시 에러.
   */
  Task<Result<void>> resume_from_checkpoint() {
    auto result = co_await store_->load(name_);
    if (!result.has_value()) {
      co_return unexpected(result.error());
    }
    items_processed_.store(result->offset, std::memory_order_release);
    co_return Result<void>::ok();
  }

  // ─── 조회 ──────────────────────────────────────────────────────────────────

  /**
   * @brief `push_counted()`를 통해 처리된 총 아이템 수를 반환합니다.
   * @returns 누적 처리 아이템 수.
   */
  [[nodiscard]] uint64_t items_processed() const noexcept {
    return items_processed_.load(std::memory_order_relaxed);
  }

  /**
   * @brief 파이프라인 이름을 반환합니다.
   * @returns 파이프라인 이름 문자열 참조.
   */
  [[nodiscard]] const std::string& name() const noexcept {
    return name_;
  }

  /**
   * @brief 자동 체크포인트가 활성화되어 있는지 확인합니다.
   * @returns 활성화 여부.
   */
  [[nodiscard]] bool checkpoint_enabled() const noexcept {
    return checkpoint_enabled_;
  }

private:
  std::string                      name_;
  std::shared_ptr<ICheckpointStore> store_;
  DynamicPipeline<T>               pipeline_;

  std::atomic<uint64_t>            items_processed_{0};

  bool                             checkpoint_enabled_{false};
  std::chrono::seconds             checkpoint_interval_t_{60};
  size_t                           checkpoint_interval_n_{1000};

  mutable std::mutex               time_mutex_;
  system_clock::time_point         last_checkpoint_time_{};
};

} // namespace qbuem

/** @} */ // end of qbuem_checkpoint
