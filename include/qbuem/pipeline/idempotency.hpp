#pragma once

/**
 * @file qbuem/pipeline/idempotency.hpp
 * @brief 멱등성 보장 — IIdempotencyStore, InMemoryIdempotencyStore, IdempotencyFilter
 * @defgroup qbuem_idempotency Idempotency
 * @ingroup qbuem_pipeline
 *
 * 메시지 큐 환경에서는 동일한 메시지가 두 번 이상 전달될 수 있습니다(at-least-once).
 * 이 모듈은 `IdempotencyKey`를 기반으로 중복 메시지를 감지하고 건너뜁니다.
 *
 * ## 동작 개요
 * 1. `IdempotencyFilter::process(item, env)`는 `env.ctx`에서 `IdempotencyKey`를 읽습니다.
 * 2. 키가 없거나 스토어에 처음 등장하는 키이면 아이템을 그대로 반환합니다.
 * 3. 이미 처리된 키(중복)이면 `std::nullopt`를 반환하여 이후 처리를 건너뜁니다.
 *
 * ## 사용 예시
 * @code
 * auto store = std::make_shared<InMemoryIdempotencyStore>();
 * IdempotencyFilter<Order> filter(store);
 *
 * // Action 함수 내:
 * auto result = co_await filter.process(order, env);
 * if (!result || !result->has_value()) co_return ...; // 중복이면 건너뜀
 * auto item = std::move(**result);
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/context.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qbuem {

/**
 * @brief 멱등성 키 저장소의 순수 가상 인터페이스.
 *
 * 분산 환경에서는 Redis, 데이터베이스 등 외부 저장소를 사용하는
 * 구현체를 제공할 수 있습니다.
 * 단일 프로세스 환경에서는 `InMemoryIdempotencyStore`를 사용합니다.
 */
class IIdempotencyStore {
public:
    virtual ~IIdempotencyStore() = default;

    /**
     * @brief 키가 없을 때만 삽입하고 결과를 반환합니다.
     *
     * 키가 스토어에 없으면 삽입 후 `true`를 반환합니다 (새 키 = 처음 처리).
     * 키가 이미 존재하면 삽입하지 않고 `false`를 반환합니다 (중복 = 이미 처리됨).
     *
     * @param key 삽입할 멱등성 키.
     * @param ttl 키 유효 기간. 만료 후에는 동일 키를 다시 처리합니다.
     * @returns `true` = 새 키(처음 처리), `false` = 중복 키.
     */
    virtual Task<bool> set_if_absent(std::string_view key, std::chrono::seconds ttl) = 0;

    /**
     * @brief 키의 존재 여부를 조회합니다.
     *
     * TTL이 만료된 키는 존재하지 않는 것으로 간주합니다.
     *
     * @param key 조회할 멱등성 키.
     * @returns `true` = 키가 유효하게 존재함, `false` = 없거나 만료됨.
     */
    virtual Task<bool> get(std::string_view key) = 0;
};

/**
 * @brief `IIdempotencyStore`의 인메모리 구현체.
 *
 * `std::unordered_map`을 사용해 키와 만료 시각을 저장합니다.
 * `get()` 호출 시 만료된 항목을 즉시 제거합니다(lazy expiry).
 *
 * ### 스레드 안전성
 * 내부적으로 `std::mutex`를 사용합니다.
 * 다수의 워커가 동시에 호출해도 안전합니다.
 *
 * ### 주의 사항
 * 프로세스 재시작 시 저장된 키가 모두 소멸됩니다.
 * 영속성이 필요하면 외부 저장소 기반 구현체를 사용하세요.
 */
class InMemoryIdempotencyStore : public IIdempotencyStore {
public:
    /**
     * @brief 키가 없을 때만 삽입하고 결과를 반환합니다.
     *
     * 만료된 기존 항목은 덮어씁니다 (재처리 허용).
     *
     * @param key 삽입할 멱등성 키.
     * @param ttl 키 유효 기간.
     * @returns `true` = 새로 삽입됨(처음 처리), `false` = 중복(이미 처리됨).
     */
    Task<bool> set_if_absent(std::string_view key, std::chrono::seconds ttl) override {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        const auto key_str = std::string(key);

        auto it = map_.find(key_str);
        if (it != map_.end()) {
            // 만료되지 않은 키가 존재하면 중복으로 판정합니다.
            if (it->second > now) {
                co_return false;
            }
            // 만료된 항목은 덮어씁니다.
        }

        map_[key_str] = now + ttl;
        co_return true;
    }

    /**
     * @brief 키의 존재 여부를 조회합니다.
     *
     * TTL이 만료된 키는 맵에서 제거 후 `false`를 반환합니다(lazy expiry).
     *
     * @param key 조회할 멱등성 키.
     * @returns `true` = 키가 유효하게 존재함, `false` = 없거나 만료됨.
     */
    Task<bool> get(std::string_view key) override {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        const auto key_str = std::string(key);

        auto it = map_.find(key_str);
        if (it == map_.end()) {
            co_return false;
        }

        if (it->second <= now) {
            // 만료된 항목을 즉시 제거합니다(lazy expiry).
            map_.erase(it);
            co_return false;
        }

        co_return true;
    }

private:
    /** @brief 멱등성 키 → 만료 시각 맵. */
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> map_;

    /** @brief 맵 접근 보호를 위한 뮤텍스. */
    std::mutex mutex_;
};

/**
 * @brief 중복 아이템을 걸러내는 멱등성 필터.
 *
 * `env.ctx`에서 `IdempotencyKey`를 읽어 스토어에 조회합니다.
 * - 처음 등장하는 키: 아이템을 `optional<T>`에 담아 반환합니다.
 * - 중복 키: `std::nullopt`를 반환하여 이후 처리를 건너뜁니다.
 * - 키가 없는 경우: 멱등성 검사를 수행하지 않고 아이템을 그대로 통과시킵니다.
 *
 * @tparam T 처리할 아이템 타입.
 */
template <typename T>
class IdempotencyFilter {
public:
    /**
     * @brief IdempotencyFilter를 생성합니다.
     *
     * @param store 멱등성 키 저장소. nullptr이면 안 됩니다.
     * @param ttl   키 유효 기간 (기본값: 24시간).
     */
    explicit IdempotencyFilter(
        std::shared_ptr<IIdempotencyStore> store,
        std::chrono::seconds ttl = std::chrono::hours{24}
    )
        : store_(std::move(store)), ttl_(ttl) {}

    /**
     * @brief 아이템의 멱등성을 검사하고 중복이면 건너뜁니다.
     *
     * `env.ctx`에서 `IdempotencyKey`를 읽어 스토어를 조회합니다.
     * - `IdempotencyKey` 슬롯이 없으면 검사를 생략하고 아이템을 그대로 반환합니다.
     * - 키가 처음 등장하면 스토어에 등록 후 아이템을 반환합니다.
     * - 키가 이미 존재하면(중복) `std::nullopt`를 반환합니다.
     *
     * @param item 처리할 아이템.
     * @param env  실행 환경 (`env.ctx`에서 `IdempotencyKey`를 읽습니다).
     * @returns 중복이 아니면 `Result<optional<T>>`에 아이템을,
     *          중복이면 `Result<optional<T>>`에 `nullopt`를 담아 반환합니다.
     */
    Task<Result<std::optional<T>>> process(T item, ActionEnv env) {
        const auto* key_slot = env.ctx.get_ptr<IdempotencyKey>();

        // IdempotencyKey 슬롯이 없으면 멱등성 검사를 건너뜁니다.
        if (!key_slot || key_slot->value.empty()) {
            co_return std::optional<T>{std::move(item)};
        }

        const bool is_new = co_await store_->set_if_absent(key_slot->value, ttl_);

        if (!is_new) {
            // 중복 키 — 아이템 처리를 건너뜁니다.
            co_return std::optional<T>{std::nullopt};
        }

        co_return std::optional<T>{std::move(item)};
    }

private:
    /** @brief 멱등성 키 저장소. */
    std::shared_ptr<IIdempotencyStore> store_;

    /** @brief 키 유효 기간. */
    std::chrono::seconds ttl_;
};

} // namespace qbuem

/** @} */
