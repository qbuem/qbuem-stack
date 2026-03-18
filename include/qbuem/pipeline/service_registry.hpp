#pragma once

/**
 * @file qbuem/pipeline/service_registry.hpp
 * @brief Scope-based dependency injection (DI) container — ServiceRegistry
 * @defgroup qbuem_service_registry ServiceRegistry
 * @ingroup qbuem_pipeline
 *
 * ServiceRegistry is the DI container for the pipeline hierarchy.
 *
 * ## Hierarchy
 * ```
 * GlobalRegistry  (process singleton)
 *     └── PipelineRegistry  (per-pipeline)
 *             └── ActionRegistry  (per-Action, optional)
 * ```
 *
 * ## Dependency strength
 * - `get<T>()` — **weak dependency**: returns nullptr, use for optional features
 * - `require<T>()` — **strong dependency**: calls `std::terminate()` if not found, fail-fast
 *
 * ## Usage example
 * @code
 * // Register at process startup
 * global_registry().register_singleton<ILogger>(make_logger());
 * global_registry().register_singleton<ISessionStore>(make_redis_store());
 *
 * // Pipeline scope
 * ServiceRegistry pipe_reg(&global_registry());
 * pipe_reg.register_singleton<IMetricsExporter>(make_metrics_exporter());
 *
 * // Usage inside an Action
 * auto logger = env.registry->require<ILogger>(); // terminates if not found
 * auto store  = env.registry->get<ISessionStore>(); // returns nullptr if not found
 * @endcode
 * @{
 */

#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>

namespace qbuem {

/**
 * @brief Scope-based DI container.
 *
 * Thread-safe. `register_*()` functions can be called dynamically at runtime.
 *
 * ### Lookup order
 * 1. Search the current registry
 * 2. Recursively search the `parent_` registry
 * 3. If not found: nullptr (`get`) or `std::terminate` (`require`)
 */
class ServiceRegistry {
public:
  /**
   * @brief Constructs a scoped registry with the given parent registry.
   *
   * @param parent Parent scope registry. nullptr makes this the root.
   */
  explicit ServiceRegistry(ServiceRegistry *parent = nullptr)
      : parent_(parent) {}

  ServiceRegistry(const ServiceRegistry &) = delete;
  ServiceRegistry &operator=(const ServiceRegistry &) = delete;

  /**
   * @brief Registers a singleton instance.
   *
   * Re-registering an already-registered type overwrites the previous entry.
   *
   * @tparam T Service type (typically an abstract interface).
   * @param  instance Shared-ownership instance.
   */
  template <typename T>
  void register_singleton(std::shared_ptr<T> instance) {
    std::unique_lock lock(mutex_);
    services_[std::type_index(typeid(T))] =
        std::static_pointer_cast<void>(std::move(instance));
  }

  /**
   * @brief Registers a factory function (lazy creation on demand).
   *
   * The factory is executed on the first call to `get<T>()` / `require<T>()`.
   * The result is cached afterward, behaving like a singleton.
   *
   * @tparam T Service type.
   * @param  factory Function that creates the instance.
   */
  template <typename T>
  void register_factory(std::function<std::shared_ptr<T>()> factory) {
    std::unique_lock lock(mutex_);
    factories_[std::type_index(typeid(T))] = [f = std::move(factory)]() {
      return std::static_pointer_cast<void>(f());
    };
  }

  /**
   * @brief Looks up a service — **weak dependency**.
   *
   * Returns nullptr if not found. Use for optional features.
   *
   * @tparam T Service type to look up.
   * @returns Service pointer, or nullptr if not found.
   */
  template <typename T>
  [[nodiscard]] std::shared_ptr<T> get() const {
    return std::static_pointer_cast<T>(lookup(std::type_index(typeid(T))));
  }

  /**
   * @brief Looks up a service — **strong dependency**.
   *
   * Calls `std::terminate()` if not found. Use for required services.
   * Registration must be guaranteed before the pipeline starts.
   *
   * @tparam T Service type to look up.
   * @returns Service pointer.
   * @note The return value is never nullptr.
   */
  template <typename T>
  [[nodiscard]] std::shared_ptr<T> require() const {
    auto ptr = get<T>();
    if (!ptr) {
      // fail-fast: treated as a programming error; use terminate for a stack trace
      std::terminate();
    }
    return ptr;
  }

  /**
   * @brief Looks up a service, or constructs and registers one with the default constructor if not found.
   *
   * T must be default-constructible. Thread-safe.
   *
   * @tparam T Service type.
   * @returns Service reference (always valid).
   */
  template <typename T>
  [[nodiscard]] T& get_or_create() {
    auto ptr = get<T>();
    if (!ptr) {
      ptr = std::make_shared<T>();
      register_singleton<T>(ptr);
    }
    return *ptr;
  }

  /**
   * @brief Returns the parent registry.
   */
  [[nodiscard]] ServiceRegistry *parent() const noexcept { return parent_; }

private:
  using AnyPtr = std::shared_ptr<void>;
  using Factory = std::function<AnyPtr()>;

  [[nodiscard]] AnyPtr lookup(std::type_index key) const {
    {
      std::shared_lock lock(mutex_);
      auto it = services_.find(key);
      if (it != services_.end())
        return it->second;
    }
    // Check factory (may need exclusive lock to cache result)
    {
      std::unique_lock lock(mutex_);
      // Re-check after acquiring exclusive lock (TOCTOU prevention)
      auto sit = services_.find(key);
      if (sit != services_.end())
        return sit->second;

      auto fit = factories_.find(key);
      if (fit != factories_.end()) {
        auto instance = fit->second();
        services_[key] = instance;
        factories_.erase(fit);
        return instance;
      }
    }
    // Delegate to parent scope
    if (parent_)
      return parent_->lookup(key);
    return nullptr;
  }

  mutable std::shared_mutex mutex_;
  mutable std::unordered_map<std::type_index, AnyPtr> services_;
  mutable std::unordered_map<std::type_index, Factory> factories_;
  ServiceRegistry *parent_ = nullptr;
};

/**
 * @brief 프로세스 싱글톤 전역 레지스트리를 반환합니다.
 *
 * 모든 파이프라인의 루트 레지스트리입니다.
 * 프로세스 시작 시 한 번 초기화하고 이후 읽기만 합니다.
 *
 * @code
 * global_registry().register_singleton<ILogger>(make_logger());
 * @endcode
 */
inline ServiceRegistry &global_registry() {
  static ServiceRegistry instance;
  return instance;
}

} // namespace qbuem

/** @} */
