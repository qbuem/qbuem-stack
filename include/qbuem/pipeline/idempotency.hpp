#pragma once

/**
 * @file qbuem/pipeline/idempotency.hpp
 * @brief Idempotency guarantee — IIdempotencyStore, InMemoryIdempotencyStore, IdempotencyFilter
 * @defgroup qbuem_idempotency Idempotency
 * @ingroup qbuem_pipeline
 *
 * In message-queue environments the same message may be delivered more than once (at-least-once).
 * This module detects and skips duplicate messages based on `IdempotencyKey`.
 *
 * ## Behavior overview
 * 1. `IdempotencyFilter::process(item, env)` reads the `IdempotencyKey` from `env.ctx`.
 * 2. If the key is absent or appears in the store for the first time, the item is returned as-is.
 * 3. If the key has already been processed (duplicate), `std::nullopt` is returned to skip further processing.
 *
 * ## Usage example
 * @code
 * auto store = std::make_shared<InMemoryIdempotencyStore>();
 * IdempotencyFilter<Order> filter(store);
 *
 * // Inside an Action function:
 * auto result = co_await filter.process(order, env);
 * if (!result || !result->has_value()) co_return ...; // skip if duplicate
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
 * @brief Pure virtual interface for an idempotency key store.
 *
 * In distributed environments, implementations backed by external stores such as
 * Redis or a database can be provided.
 * For single-process environments, use `InMemoryIdempotencyStore`.
 */
class IIdempotencyStore {
public:
    virtual ~IIdempotencyStore() = default;

    /**
     * @brief Inserts a key only if absent and returns the result.
     *
     * If the key is not in the store, inserts it and returns `true` (new key = first processing).
     * If the key already exists, does not insert and returns `false` (duplicate = already processed).
     *
     * @param key Idempotency key to insert.
     * @param ttl Key validity period. The same key may be processed again after expiry.
     * @returns `true` = new key (first processing), `false` = duplicate key.
     */
    virtual Task<bool> set_if_absent(std::string_view key, std::chrono::seconds ttl) = 0;

    /**
     * @brief Checks whether a key exists.
     *
     * Keys whose TTL has expired are treated as non-existent.
     *
     * @param key Idempotency key to look up.
     * @returns `true` = key exists and is valid, `false` = absent or expired.
     */
    virtual Task<bool> get(std::string_view key) = 0;
};

/**
 * @brief In-memory implementation of `IIdempotencyStore`.
 *
 * Uses `std::unordered_map` to store keys and their expiry timestamps.
 * Expired entries are removed immediately on `get()` (lazy expiry).
 *
 * ### Thread safety
 * Uses `std::mutex` internally.
 * Safe to call from multiple workers concurrently.
 *
 * ### Note
 * All stored keys are lost on process restart.
 * Use an external-store-backed implementation if persistence is required.
 */
class InMemoryIdempotencyStore : public IIdempotencyStore {
public:
    /**
     * @brief Inserts a key only if absent and returns the result.
     *
     * Overwrites expired entries (allows reprocessing after expiry).
     *
     * @param key Idempotency key to insert.
     * @param ttl Key validity period.
     * @returns `true` = newly inserted (first processing), `false` = duplicate (already processed).
     */
    Task<bool> set_if_absent(std::string_view key, std::chrono::seconds ttl) override {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        const auto key_str = std::string(key);

        auto it = map_.find(key_str);
        if (it != map_.end()) {
            // A non-expired key exists — treat as duplicate.
            if (it->second > now) {
                co_return false;
            }
            // Overwrite the expired entry.
        }

        map_[key_str] = now + ttl;
        co_return true;
    }

    /**
     * @brief Checks whether a key exists.
     *
     * Removes expired keys from the map before returning `false` (lazy expiry).
     *
     * @param key Idempotency key to look up.
     * @returns `true` = key exists and is valid, `false` = absent or expired.
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
