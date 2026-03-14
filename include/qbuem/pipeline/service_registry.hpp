#pragma once

/**
 * @file qbuem/pipeline/service_registry.hpp
 * @brief 스코프 기반 의존성 주입(DI) 컨테이너 — ServiceRegistry
 * @defgroup qbuem_service_registry ServiceRegistry
 * @ingroup qbuem_pipeline
 *
 * ServiceRegistry는 파이프라인 계층 구조의 DI 컨테이너입니다.
 *
 * ## 계층 구조
 * ```
 * GlobalRegistry  (프로세스 싱글톤)
 *     └── PipelineRegistry  (파이프라인별)
 *             └── ActionRegistry  (Action별, 선택적)
 * ```
 *
 * ## 의존성 강도
 * - `get<T>()` — **약한 의존성**: nullptr 반환, 선택적 기능에 사용
 * - `require<T>()` — **강한 의존성**: 없으면 `std::terminate()`, fail-fast
 *
 * ## 사용 예시
 * @code
 * // 프로세스 시작 시 등록
 * global_registry().register_singleton<ILogger>(make_logger());
 * global_registry().register_singleton<ISessionStore>(make_redis_store());
 *
 * // 파이프라인 스코프
 * ServiceRegistry pipe_reg(&global_registry());
 * pipe_reg.register_singleton<IMetricsExporter>(make_metrics_exporter());
 *
 * // Action 내 사용
 * auto logger = env.registry->require<ILogger>(); // 없으면 terminate
 * auto store  = env.registry->get<ISessionStore>(); // 없으면 nullptr
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
 * @brief 스코프 기반 DI 컨테이너.
 *
 * 스레드 안전합니다. `register_*()` 함수는 런타임 중 동적으로 호출 가능합니다.
 *
 * ### 조회 순서
 * 1. 현재 레지스트리에서 검색
 * 2. `parent_` 레지스트리로 재귀 검색
 * 3. 없으면 nullptr(`get`) 또는 `std::terminate`(`require`)
 */
class ServiceRegistry {
public:
  /**
   * @brief 부모 레지스트리를 지정해 스코프 레지스트리를 생성합니다.
   *
   * @param parent 상위 스코프 레지스트리. nullptr이면 루트.
   */
  explicit ServiceRegistry(ServiceRegistry *parent = nullptr)
      : parent_(parent) {}

  ServiceRegistry(const ServiceRegistry &) = delete;
  ServiceRegistry &operator=(const ServiceRegistry &) = delete;

  /**
   * @brief 싱글톤 인스턴스를 등록합니다.
   *
   * 이미 등록된 타입을 재등록하면 덮어씁니다.
   *
   * @tparam T 서비스 타입 (보통 추상 인터페이스).
   * @param  instance 공유 소유권 인스턴스.
   */
  template <typename T>
  void register_singleton(std::shared_ptr<T> instance) {
    std::unique_lock lock(mutex_);
    services_[std::type_index(typeid(T))] =
        std::static_pointer_cast<void>(std::move(instance));
  }

  /**
   * @brief 팩토리 함수를 등록합니다 (요청 시 생성 — lazy).
   *
   * 첫 `get<T>()` / `require<T>()` 호출 시 팩토리가 실행됩니다.
   * 이후 결과가 캐시되어 싱글톤처럼 동작합니다.
   *
   * @tparam T 서비스 타입.
   * @param  factory 인스턴스를 생성하는 함수.
   */
  template <typename T>
  void register_factory(std::function<std::shared_ptr<T>()> factory) {
    std::unique_lock lock(mutex_);
    factories_[std::type_index(typeid(T))] = [f = std::move(factory)]() {
      return std::static_pointer_cast<void>(f());
    };
  }

  /**
   * @brief 서비스를 조회합니다 — **약한 의존성**.
   *
   * 없으면 nullptr을 반환합니다. 선택적 기능(optional feature)에 사용합니다.
   *
   * @tparam T 조회할 서비스 타입.
   * @returns 서비스 포인터, 없으면 nullptr.
   */
  template <typename T>
  [[nodiscard]] std::shared_ptr<T> get() const {
    return std::static_pointer_cast<T>(lookup(std::type_index(typeid(T))));
  }

  /**
   * @brief 서비스를 조회합니다 — **강한 의존성**.
   *
   * 없으면 `std::terminate()`를 호출합니다. 필수 서비스에 사용합니다.
   * 파이프라인 시작 전 등록 여부를 보장해야 합니다.
   *
   * @tparam T 조회할 서비스 타입.
   * @returns 서비스 포인터.
   * @note 반환값은 절대 nullptr이 아닙니다.
   */
  template <typename T>
  [[nodiscard]] std::shared_ptr<T> require() const {
    auto ptr = get<T>();
    if (!ptr) {
      // fail-fast: 프로그래밍 오류로 간주, 스택 트레이스를 위해 terminate 사용
      std::terminate();
    }
    return ptr;
  }

  /**
   * @brief 부모 레지스트리를 반환합니다.
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
