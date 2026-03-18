#pragma once

/**
 * @file qbuem/pipeline/migration.hpp
 * @brief Pipeline type migration — MigrationFn, DlqReprocessor
 * @defgroup qbuem_migration Pipeline Migration
 * @ingroup qbuem_pipeline
 *
 * Used when incrementally changing the pipeline schema (message type).
 *
 * ## Incremental type change guide
 * ```
 * 1. Insert MigrationAction<OldT, NewT>
 *    → Converts existing old-format messages to new-format while running in parallel
 * 2. Once all production traffic has switched to new-format, remove MigrationAction
 * 3. Reprocess old-format messages in the DLQ using DlqReprocessor
 * ```
 *
 * ## Usage example
 * ```cpp
 * // Register OldMsg → NewMsg conversion function
 * DlqReprocessor<OldMsg> reprocessor;
 * reprocessor.register_migration<NewMsg>(
 *   "v1->v2",
 *   [](OldMsg old) -> NewMsg { return NewMsg{old.id, old.value * 2}; }
 * );
 *
 * // Reprocess DLQ
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
 * @brief Type migration function type.
 *
 * A pure function that takes an `OldT` value and returns a `NewT` value.
 * On failure, propagates via `Result<NewT>` error.
 *
 * @tparam OldT Previous message type.
 * @tparam NewT New message type.
 */
template <typename OldT, typename NewT>
using MigrationFn = std::function<Result<NewT>(OldT)>;

// ---------------------------------------------------------------------------
// MigrationAction<OldT, NewT>
// ---------------------------------------------------------------------------

/**
 * @brief Inline type conversion action within a pipeline.
 *
 * Inserted between stages to perform OldT -> NewT conversion.
 * Remove after migration is complete.
 *
 * @tparam OldT Input message type (old format).
 * @tparam NewT Output message type (new format).
 */
template <typename OldT, typename NewT>
class MigrationAction {
public:
  /**
   * @brief Construct an action with a migration function.
   * @param name     Migration name (e.g., "v1->v2").
   * @param migrate  Conversion function.
   */
  explicit MigrationAction(std::string name, MigrationFn<OldT, NewT> migrate)
      : name_(std::move(name)), migrate_(std::move(migrate)) {}

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  /**
   * @brief Convert a single item.
   * @param old_item Item in the old format.
   * @returns Converted item in the new format, or an error.
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
 * @brief Reprocessor for messages stored in the Dead Letter Queue (DLQ).
 *
 * Converts old-format messages accumulated in the DLQ using a migration
 * function, then re-injects them into the new pipeline.
 *
 * ### Workflow
 * ```
 * DLQ[OldT] -> MigrationFn<OldT,NewT> -> new pipeline.push(NewT)
 * ```
 *
 * @tparam T Message type stored in the DLQ (old format).
 */
template <typename T>
class DlqReprocessor {
public:
  /// @brief Summary of reprocessing results.
  struct ReprocessResult {
    size_t migrated{0};   ///< Number of items successfully migrated
    size_t failed{0};     ///< Number of migration failures
    size_t skipped{0};    ///< Number of items skipped (no matching migration)
  };

  // -------------------------------------------------------------------------
  // Migration registration
  // -------------------------------------------------------------------------

  /**
   * @brief Register a migration function to be used during DLQ reprocessing.
   *
   * @tparam NewT   Target conversion type.
   * @param  name   Migration name (e.g., "v1->v2").
   * @param  fn     Conversion function `T -> NewT`.
   * @param  push   Function that injects the converted item into the new pipeline.
   * @returns *this (supports chaining).
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
  // Reprocess execution
  // -------------------------------------------------------------------------

  /**
   * @brief Reprocess all entries in the DeadLetterQueue.
   *
   * For each item, the registered migrations are tried starting from the first.
   * On success, moves on to the next item.
   *
   * @param dlq The DeadLetterQueue<T> to reprocess.
   * @returns Summary of reprocessing results.
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
          // Conversion failed — try the next migration
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

  /// @brief Return the number of registered migrations.
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
