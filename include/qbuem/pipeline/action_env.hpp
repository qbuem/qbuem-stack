#pragma once

/**
 * @file qbuem/pipeline/action_env.hpp
 * @brief Action execution environment — ActionEnv, ContextualItem, WorkerLocal
 * @defgroup qbuem_action_env ActionEnv
 * @ingroup qbuem_pipeline
 *
 * Defines the execution context and related types passed to Action functions.
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
 * @brief Execution environment passed to Action functions.
 *
 * `Action::Fn` signature:
 * ```cpp
 * Task<Result<Out>>(In item, ActionEnv env)
 * ```
 *
 * ### Field descriptions
 * - `ctx`        : Item metadata (TraceCtx, RequestId, etc.)
 * - `stop`       : Pipeline/action cancellation signal
 * - `worker_idx` : 0-based worker index (used for WorkerLocal<T> access)
 * - `registry`   : Pipeline-scoped ServiceRegistry
 *
 * ### ⚠️ thread_local is forbidden
 * Coroutines may resume on a different thread after `co_await`.
 * Per-item state must be passed through `ctx` slots.
 */
struct ActionEnv {
  Context          ctx;        ///< Item context (immutable)
  std::stop_token  stop;       ///< Cancellation signal (requested on stop())
  size_t           worker_idx; ///< Current worker index (0-based)
  ServiceRegistry *registry;   ///< Pipeline-scoped registry (non-null)
};

/**
 * @brief Channel transmission unit — item + Context pair.
 *
 * `ContextualItem<T>` is the type transmitted inside channels.
 * Action functions receive the value (`T`) only and access context via `ActionEnv`.
 *
 * On fan-out, `ctx` is copied via `fork()` (O(1) shared_ptr copy).
 *
 * @tparam T Type of the item value.
 */
template <typename T>
struct ContextualItem {
  T       value;  ///< Item value
  Context ctx;    ///< Item metadata
};

/**
 * @brief Per-worker state storage — lock-free worker-local data.
 *
 * `alignas(64)` slots prevent false sharing.
 * Accessed via `env.worker_idx`, so no mutex is required.
 *
 * ## Appropriate use cases
 * - Per-worker RNG (e.g. std::mt19937)
 * - Per-worker scratch buffers (e.g. for serialization)
 * - Per-worker connection pools / caches
 *
 * ## ⚠️ Inappropriate use cases
 * - Per-item state → use `Context` slots
 * - Pipeline-global state → use `ServiceRegistry`
 *
 * ## Usage example
 * @code
 * WorkerLocal<std::mt19937> rngs(dispatcher.thread_count());
 *
 * // Inside an Action function:
 * auto &rng = rngs[env.worker_idx];
 * auto val = std::uniform_int_distribution<>(0, 100)(rng);
 * @endcode
 *
 * @tparam T Per-worker storage type.
 */
template <typename T>
class WorkerLocal {
public:
  /**
   * @brief Initialize with the given worker count.
   *
   * @param worker_count Number of workers (typically `Dispatcher::thread_count()`).
   */
  explicit WorkerLocal(size_t worker_count) {
    slots_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i)
      slots_.emplace_back();
  }

  /**
   * @brief Access the slot for a specific worker.
   *
   * @param worker_idx Value of `env.worker_idx`.
   * @returns `T&` reference to that worker's slot.
   */
  T &operator[](size_t worker_idx) {
    assert(worker_idx < slots_.size());
    return slots_[worker_idx].value;
  }

  const T &operator[](size_t worker_idx) const {
    assert(worker_idx < slots_.size());
    return slots_[worker_idx].value;
  }

  [[nodiscard]] size_t size() const noexcept { return slots_.size(); }

private:
  // alignas(64) prevents false sharing.
  // The compiler automatically adds padding to ensure sizeof(Slot) % 64 == 0.
  struct alignas(64) Slot {
    T value{};
  };

  std::vector<Slot> slots_;
};

} // namespace qbuem

/** @} */
