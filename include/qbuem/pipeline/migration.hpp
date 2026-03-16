#pragma once

/**
 * @file qbuem/pipeline/migration.hpp
 * @brief 파이프라인 타입 마이그레이션 — MigrationFn, DlqReprocessor
 * @defgroup qbuem_migration Pipeline Migration
 * @ingroup qbuem_pipeline
 *
 * 파이프라인 스키마(메시지 타입)를 점진적으로 변경할 때 사용합니다.
 *
 * ## 점진적 타입 변경 가이드
 * ```
 * 1. MigrationAction<OldT, NewT> 삽입
 *    → 기존 old-format 메시지를 new-format으로 변환하며 병렬 운영
 * 2. 프로덕션 트래픽이 전부 new-format으로 전환되면 MigrationAction 제거
 * 3. DLQ 내 old-format 메시지는 DlqReprocessor로 재처리
 * ```
 *
 * ## 사용 예시
 * ```cpp
 * // OldMsg → NewMsg 변환 함수 등록
 * DlqReprocessor<OldMsg> reprocessor;
 * reprocessor.register_migration<NewMsg>(
 *   "v1→v2",
 *   [](OldMsg old) -> NewMsg { return NewMsg{old.id, old.value * 2}; }
 * );
 *
 * // DLQ 재처리
 * co_await reprocessor.reprocess(dlq, new_pipeline);
 * ```
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/dead_letter.hpp>

#include <any>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace qbuem {

// ---------------------------------------------------------------------------
// MigrationFn
// ---------------------------------------------------------------------------

/**
 * @brief 타입 마이그레이션 함수 타입.
 *
 * `OldT` 값을 받아 `NewT` 값을 반환하는 순수 함수입니다.
 * 실패 시 `Result<NewT>` 에러를 통해 전파합니다.
 *
 * @tparam OldT 이전 메시지 타입.
 * @tparam NewT 새 메시지 타입.
 */
template <typename OldT, typename NewT>
using MigrationFn = std::function<Result<NewT>(OldT)>;

// ---------------------------------------------------------------------------
// MigrationAction<OldT, NewT>
// ---------------------------------------------------------------------------

/**
 * @brief 파이프라인 내 인라인 타입 변환 액션.
 *
 * 스테이지 사이에 삽입하여 OldT → NewT 변환을 수행합니다.
 * 마이그레이션 완료 후 제거합니다.
 *
 * @tparam OldT 입력 메시지 타입 (이전 형식).
 * @tparam NewT 출력 메시지 타입 (새 형식).
 */
template <typename OldT, typename NewT>
class MigrationAction {
public:
  /**
   * @brief 마이그레이션 함수로 액션을 생성합니다.
   * @param name     마이그레이션 이름 (예: "v1→v2").
   * @param migrate  변환 함수.
   */
  explicit MigrationAction(std::string name, MigrationFn<OldT, NewT> migrate)
      : name_(std::move(name)), migrate_(std::move(migrate)) {}

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  /**
   * @brief 단일 아이템을 변환합니다.
   * @param old_item 이전 형식 아이템.
   * @returns 변환된 새 형식 아이템 또는 에러.
   */
  [[nodiscard]] Task<Result<NewT>> process(OldT old_item) {
    co_return migrate_(std::move(old_item));
  }

private:
  std::string            name_;
  MigrationFn<OldT, NewT> migrate_;
};

// ---------------------------------------------------------------------------
// DlqReprocessor<T>
// ---------------------------------------------------------------------------

/**
 * @brief DLQ(Dead Letter Queue) 내 메시지 재처리기.
 *
 * DLQ에 쌓인 old-format 메시지를 마이그레이션 함수로 변환하여
 * 새 파이프라인으로 재주입합니다.
 *
 * ### 워크플로우
 * ```
 * DLQ[OldT] → MigrationFn<OldT,NewT> → new pipeline.push(NewT)
 * ```
 *
 * @tparam T DLQ에 저장된 메시지 타입 (이전 형식).
 */
template <typename T>
class DlqReprocessor {
public:
  /// @brief 재처리 결과 요약.
  struct ReprocessResult {
    size_t migrated{0};   ///< 성공적으로 마이그레이션된 수
    size_t failed{0};     ///< 마이그레이션 실패 수
    size_t skipped{0};    ///< 대상 마이그레이션 없어서 스킵된 수
  };

  // -------------------------------------------------------------------------
  // 마이그레이션 등록
  // -------------------------------------------------------------------------

  /**
   * @brief DLQ 재처리 시 사용할 마이그레이션 함수를 등록합니다.
   *
   * @tparam NewT   변환 목표 타입.
   * @param  name   마이그레이션 이름 (예: "v1→v2").
   * @param  fn     변환 함수 `T → NewT`.
   * @param  push   변환된 아이템을 새 파이프라인에 주입하는 함수.
   * @returns *this (체이닝 지원).
   */
  template <typename NewT>
  DlqReprocessor &register_migration(
      std::string name,
      MigrationFn<T, NewT> fn,
      std::function<bool(NewT)> push_to_new_pipeline) {
    entries_.push_back(Entry{
        .name = std::move(name),
        .migrate_and_push = [fn = std::move(fn),
                             push = std::move(push_to_new_pipeline)](T item)
            -> Result<bool> {
          auto result = fn(item);
          if (!result) return unexpected(result.error());
          bool ok = push(std::move(*result));
          return ok;
        }});
    return *this;
  }

  // -------------------------------------------------------------------------
  // 재처리 실행
  // -------------------------------------------------------------------------

  /**
   * @brief DeadLetterQueue의 모든 항목을 재처리합니다.
   *
   * 각 아이템에 대해 등록된 마이그레이션을 첫 번째 것부터 시도합니다.
   * 성공하면 다음 아이템으로 넘어갑니다.
   *
   * @param dlq 재처리할 DeadLetterQueue<T>.
   * @returns 재처리 결과 요약.
   */
  template <typename DlqT>
  [[nodiscard]] Task<ReprocessResult> reprocess(DlqT &dlq) {
    ReprocessResult result;

    auto items = dlq.drain();
    for (auto &item : items) {
      bool handled = false;

      for (auto &entry : entries_) {
        auto r = entry.migrate_and_push(item.item);
        if (r && *r) {
          ++result.migrated;
          handled = true;
          break;
        }
        if (!r) {
          // 변환 실패 — 다음 migration 시도
        }
      }

      if (!handled) {
        if (entries_.empty())
          ++result.skipped;
        else
          ++result.failed;
      }
    }

    co_return result;
  }

  /// @brief 등록된 마이그레이션 수를 반환합니다.
  [[nodiscard]] size_t migration_count() const noexcept { return entries_.size(); }

private:
  struct Entry {
    std::string                          name;
    std::function<Result<bool>(T)>       migrate_and_push;
  };

  std::vector<Entry> entries_;
};

/** @} */

} // namespace qbuem
