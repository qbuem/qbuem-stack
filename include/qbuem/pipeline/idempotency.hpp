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
            // Remove the expired entry immediately (lazy expiry).
            map_.erase(it);
            co_return false;
        }

        co_return true;
    }

private:
    /** @brief Idempotency key → expiry timestamp map. */
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> map_;

    /** @brief Mutex protecting map access. */
    std::mutex mutex_;
};

/**
 * @brief Idempotency filter that discards duplicate items.
 *
 * Reads the `IdempotencyKey` from `env.ctx` and looks it up in the store.
 * - First-seen key: wraps the item in `optional<T>` and returns it.
 * - Duplicate key: returns `std::nullopt` to skip further processing.
 * - No key present: passes the item through without performing an idempotency check.
 *
 * @tparam T Type of item to process.
 */
template <typename T>
class IdempotencyFilter {
public:
    /**
     * @brief Creates an IdempotencyFilter.
     *
     * @param store Idempotency key store. Must not be nullptr.
     * @param ttl   Key validity period (default: 24 hours).
     */
    explicit IdempotencyFilter(
        std::shared_ptr<IIdempotencyStore> store,
        std::chrono::seconds ttl = std::chrono::hours{24}
    )
        : store_(std::move(store)), ttl_(ttl) {}

    /**
     * @brief Checks the idempotency of an item and skips it if it is a duplicate.
     *
     * Reads the `IdempotencyKey` from `env.ctx` and queries the store.
     * - If no `IdempotencyKey` slot is present, skips the check and returns the item as-is.
     * - If the key is seen for the first time, registers it in the store and returns the item.
     * - If the key already exists (duplicate), returns `std::nullopt`.
     *
     * @param item Item to process.
     * @param env  Execution environment (`IdempotencyKey` is read from `env.ctx`).
     * @returns `Result<optional<T>>` containing the item if not a duplicate,
     *          or `Result<optional<T>>` containing `nullopt` if it is a duplicate.
     */
    Task<Result<std::optional<T>>> process(T item, ActionEnv env) {
        const auto* key_slot = env.ctx.get_ptr<IdempotencyKey>();

        // If no IdempotencyKey slot is present, skip the idempotency check.
        if (!key_slot || key_slot->value.empty()) {
            co_return std::optional<T>{std::move(item)};
        }

        const bool is_new = co_await store_->set_if_absent(key_slot->value, ttl_);

        if (!is_new) {
            // Duplicate key — skip item processing.
            co_return std::optional<T>{std::nullopt};
        }

        co_return std::optional<T>{std::move(item)};
    }

private:
    /** @brief Idempotency key store. */
    std::shared_ptr<IIdempotencyStore> store_;

    /** @brief Key validity period. */
    std::chrono::seconds ttl_;
};

} // namespace qbuem

/** @} */
