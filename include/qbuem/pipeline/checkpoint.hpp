#pragma once

/**
 * @file qbuem/pipeline/checkpoint.hpp
 * @brief DynamicPipeline checkpoint / snapshot support
 * @defgroup qbuem_checkpoint Checkpoint
 * @ingroup qbuem_pipeline
 *
 * This header provides the checkpoint infrastructure for saving and restoring
 * DynamicPipeline offsets and metadata:
 *
 * - `CheckpointData`         : Checkpoint record (offset + metadata JSON + saved timestamp)
 * - `ICheckpointStore`       : Abstract storage interface
 * - `InMemoryCheckpointStore`: In-memory reference implementation
 * - `CheckpointedPipeline<T>`: Checkpoint-aware pipeline wrapping DynamicPipeline<T>
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
 * @brief A single checkpoint record.
 *
 * Holds the cumulative offset of items processed by the pipeline,
 * user-defined metadata JSON, and the timestamp at which the checkpoint was saved.
 */
struct CheckpointData {
  /** @brief Cumulative offset of items processed up to the save point. */
  uint64_t offset{0};

  /** @brief User-defined serialized metadata (JSON string). */
  std::string metadata_json;

  /** @brief Timestamp at which the checkpoint was saved. */
  system_clock::time_point saved_at;
};

// ─── ICheckpointStore ─────────────────────────────────────────────────────────

/**
 * @brief Abstract interface for checkpoint storage.
 *
 * Saves and loads `CheckpointData` using the pipeline name as a key.
 * Implementations may target in-memory storage, the filesystem, a remote KV store, etc.
 *
 * ### Thread safety
 * Implementations must support concurrent calls to `save` and `load`.
 */
class ICheckpointStore {
public:
  virtual ~ICheckpointStore() = default;

  /**
   * @brief Save a checkpoint.
   *
   * @param pipeline_name  Pipeline identifier.
   * @param offset         Cumulative offset of processed items.
   * @param metadata_json  User-defined metadata JSON string.
   * @returns `Result<void>::ok()` on success, or an error.
   */
  virtual Task<Result<void>> save(std::string_view pipeline_name,
                                   uint64_t offset,
                                   std::string_view metadata_json) = 0;

  /**
   * @brief Load a saved checkpoint.
   *
   * @param pipeline_name Pipeline identifier.
   * @returns `CheckpointData` on success, or an error if no data exists or loading fails.
   */
  virtual Task<Result<CheckpointData>> load(std::string_view pipeline_name) = 0;
};

// ─── InMemoryCheckpointStore ──────────────────────────────────────────────────

/**
 * @brief In-memory checkpoint store.
 *
 * Data is lost on process restart.
 * Suitable for testing and unit development.
 *
 * Thread-safe; uses a hash map protected by `std::mutex`.
 */
class InMemoryCheckpointStore : public ICheckpointStore {
public:
  /**
   * @brief Save a checkpoint to memory.
   *
   * @param pipeline_name  Pipeline identifier.
   * @param offset         Cumulative offset of processed items.
   * @param metadata_json  User-defined metadata JSON string.
   * @returns Always `Result<void>::ok()`.
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
   * @brief Load a checkpoint from memory.
   *
   * @param pipeline_name Pipeline identifier.
   * @returns The stored `CheckpointData`, or `errc::no_such_file_or_directory` if not found.
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
   * @brief Return the number of stored checkpoints.
   * @returns Number of entries in the store.
   */
  [[nodiscard]] size_t size() const {
    std::lock_guard lock(mutex_);
    return store_.size();
  }

  /**
   * @brief Delete all stored checkpoints.
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
 * @brief DynamicPipeline wrapper with checkpoint support.
 *
 * Holds a `DynamicPipeline<T>` internally, tracks the number of processed items,
 * and supports periodic or manual checkpoint save and restore.
 *
 * ### Usage example
 * ```cpp
 * auto store = std::make_shared<InMemoryCheckpointStore>();
 * CheckpointedPipeline<int> cp("my_pipeline", store);
 * cp.pipeline().add_stage("double", [](int x, ActionEnv) -> Task<Result<int>> {
 *     co_return x * 2;
 * });
 * cp.enable_checkpoint(std::chrono::seconds{30}, 500);
 * cp.pipeline().start(dispatcher);
 *
 * // Manually save checkpoint
 * co_await cp.save_checkpoint();
 *
 * // Resume from previous checkpoint (restore offset to determine retransmit start point from external source)
 * co_await cp.resume_from_checkpoint();
 * ```
 *
 * @tparam T Message type flowing through the pipeline.
 */
template <typename T>
class CheckpointedPipeline {
public:
  /**
   * @brief Construct a CheckpointedPipeline.
   *
   * @param name  Pipeline name (used as the checkpoint store key).
   * @param store Checkpoint store (shared ownership).
   * @param cfg   Internal DynamicPipeline configuration (optional).
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

  // ─── Internal pipeline access ─────────────────────────────────────────────

  /**
   * @brief Return a reference to the internal `DynamicPipeline<T>`.
   *
   * Used for pipeline operations such as adding stages, starting, and stopping.
   * @returns Reference to the internal pipeline.
   */
  [[nodiscard]] DynamicPipeline<T>& pipeline() noexcept {
    return pipeline_;
  }

  /**
   * @brief Return a const reference to the internal `DynamicPipeline<T>`.
   * @returns Const reference to the internal pipeline.
   */
  [[nodiscard]] const DynamicPipeline<T>& pipeline() const noexcept {
    return pipeline_;
  }

  // ─── Checkpoint configuration ─────────────────────────────────────────────

  /**
   * @brief Configure the automatic checkpoint policy.
   *
   * A checkpoint is saved when either the time interval (`every_t`) or
   * the item count (`every_n`) threshold is reached first.
   * The automatic trigger is checked when items are submitted via `push_counted()`.
   *
   * @param every_t Checkpoint save interval (default 60 seconds).
   * @param every_n Item count interval for checkpoint saves (default 1000 items).
   */
  void enable_checkpoint(std::chrono::seconds every_t = std::chrono::seconds{60},
                          size_t every_n = 1000) {
    checkpoint_interval_t_ = every_t;
    checkpoint_interval_n_ = every_n;
    checkpoint_enabled_    = true;
    last_checkpoint_time_  = system_clock::now();
  }

  // ─── Item submission (with counter) ───────────────────────────────────────

  /**
   * @brief Submit an item to the pipeline and increment the processing counter.
   *
   * If checkpointing is enabled and either the item count or time condition is met,
   * `save_checkpoint()` is called automatically.
   *
   * @param item          Item to submit.
   * @param ctx           Item context (default: empty Context).
   * @param metadata_json Checkpoint metadata JSON (default: empty string).
   * @returns `Result<void>::ok()` or a push/checkpoint error.
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

  // ─── Manual checkpoint operations ────────────────────────────────────────

  /**
   * @brief Immediately save the current offset and metadata to the store.
   *
   * @param metadata_json User metadata JSON string to save (default: empty string).
   * @returns `Result<void>::ok()` on success, or an error.
   */
  Task<Result<void>> save_checkpoint(std::string_view metadata_json = "") {
    uint64_t offset = items_processed_.load(std::memory_order_acquire);
    co_return co_await store_->save(name_, offset, metadata_json);
  }

  /**
   * @brief Load the most recent checkpoint from the store and restore the offset.
   *
   * The restored offset can be queried via `items_processed()` and used as the
   * starting point for retransmission from an external source (e.g., Kafka, file).
   *
   * @returns `Result<void>::ok()` on success, or an error if no data exists or loading fails.
   */
  Task<Result<void>> resume_from_checkpoint() {
    auto result = co_await store_->load(name_);
    if (!result.has_value()) {
      co_return unexpected(result.error());
    }
    items_processed_.store(result->offset, std::memory_order_release);
    co_return Result<void>::ok();
  }

  // ─── Queries ──────────────────────────────────────────────────────────────

  /**
   * @brief Return the total number of items processed via `push_counted()`.
   * @returns Cumulative processed item count.
   */
  [[nodiscard]] uint64_t items_processed() const noexcept {
    return items_processed_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Return the pipeline name.
   * @returns Reference to the pipeline name string.
   */
  [[nodiscard]] const std::string& name() const noexcept {
    return name_;
  }

  /**
   * @brief Check whether automatic checkpointing is enabled.
   * @returns true if enabled, false otherwise.
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
