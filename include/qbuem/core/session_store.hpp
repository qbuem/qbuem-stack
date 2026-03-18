#pragma once

/**
 * @file qbuem/core/session_store.hpp
 * @brief Abstract session store interface — ISessionStore
 * @defgroup qbuem_session Session Store
 * @ingroup qbuem_core
 *
 * Abstracts storage, retrieval, and expiry of session data.
 * Implementations for Redis, in-memory, database, etc. implement this
 * interface at the service layer.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace qbuem {

/**
 * @brief Abstract session store interface.
 *
 * Manages a session ID → serialized session data (string) mapping with TTL.
 * The serialization format (JSON, MessagePack, etc.) is decided by the service.
 *
 * ### Thread Safety
 * Implementations may be called concurrently from multiple worker threads
 * and must therefore be thread-safe.
 *
 * ### Usage Example
 * @code
 * // Register a Redis implementation in the ServiceRegistry
 * global_registry().register_singleton<ISessionStore>(
 *     std::make_shared<RedisSessionStore>("redis://localhost:6379"));
 *
 * // Use inside a route handler
 * auto store = global_registry().require<ISessionStore>();
 * auto data = co_await store->get(req.cookie("session_id"));
 * @endcode
 */
class ISessionStore {
public:
  virtual ~ISessionStore() = default;

  /**
   * @brief Retrieve session data.
   *
   * @param session_id Session identifier.
   * @returns Session data string. Returns `std::nullopt` if the session does
   *          not exist or has expired.
   */
  virtual Task<Result<std::optional<std::string>>>
  get(std::string_view session_id) = 0;

  /**
   * @brief Store (upsert) session data.
   *
   * Overwrites an existing session and refreshes its TTL.
   *
   * @param session_id Session identifier.
   * @param value      Serialized session data.
   * @param ttl        Session lifetime. Default: 1 hour.
   */
  virtual Task<Result<void>>
  set(std::string_view session_id, std::string value,
      std::chrono::seconds ttl = std::chrono::seconds{3600}) = 0;

  /**
   * @brief Delete a session (e.g., on logout).
   *
   * Deleting a non-existent session does not return an error.
   *
   * @param session_id Identifier of the session to delete.
   */
  virtual Task<Result<void>> del(std::string_view session_id) = 0;

  /**
   * @brief Extend the session TTL (sliding expiry).
   *
   * Refreshes the expiry time without modifying session data.
   * Returns an error if the session has already expired.
   *
   * @param session_id Identifier of the session to refresh.
   * @param ttl        New lifetime. Default: 1 hour.
   */
  virtual Task<Result<void>>
  touch(std::string_view session_id,
        std::chrono::seconds ttl = std::chrono::seconds{3600}) = 0;

  /**
   * @brief Check whether a session exists.
   *
   * Use this when you only need existence without fetching data via `get()`.
   * The default implementation delegates to `get() != nullopt`.
   */
  virtual Task<Result<bool>> exists(std::string_view session_id) {
    auto r = co_await get(session_id);
    if (!r.has_value())
      co_return unexpected(r.error());
    co_return r->has_value();
  }
};

} // namespace qbuem

/** @} */
