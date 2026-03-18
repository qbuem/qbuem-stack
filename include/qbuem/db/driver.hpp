#pragma once

/**
 * @file qbuem/db/driver.hpp
 * @brief IDBDriver — Unified DB driver interface.
 * @defgroup qbuem_db_driver IDBDriver
 * @ingroup qbuem_db
 *
 * `IDBDriver` is the top-level interface of the unified DB abstraction layer in qbuem-stack.
 *
 * ## Design Principles
 * - **O(1) Connection Handover**: Connection acquisition uses lock-free ring buffer indexing.
 * - **Stateless Protocol Parsing**: Parser processes only the buffer without external state.
 * - **Zero Allocation**: No heap allocation during query preparation or result streaming.
 * - **Reactor Alignment**: All I/O is based on `IReactor` / `io_uring`.
 *
 * ## Implementation Layers
 * ```
 * IDBDriver (interface)
 *   └─ ConnectionPool  (connection pool management)
 *        └─ Connection (single session, begin/prepare/close)
 *             └─ Statement (prepared query + parameter binding)
 *                  └─ ResultSet (async result stream)
 * ```
 *
 * @code
 * auto pool = driver->pool("postgresql://localhost:5432/mydb");
 * auto conn = co_await pool->acquire();
 * auto tx   = co_await conn->begin(db::IsolationLevel::ReadCommitted);
 * auto stmt = co_await conn->prepare("SELECT id, name FROM users WHERE id = $1");
 * auto rs   = co_await stmt->execute(db::BoundParams<1>{{db::Value{int64_t{42}}, 1}}.span());
 * while (auto row = co_await rs->next()) {
 *     auto id   = row->get(0).get<int64_t>();
 *     auto name = row->get(1).get<std::string_view>();
 * }
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/db/value.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace qbuem::db {

// ─── Enumerations ─────────────────────────────────────────────────────────────

/** @brief Transaction isolation levels. Follows the SQL:1999 standard. */
enum class IsolationLevel : uint8_t {
    ReadUncommitted = 0,
    ReadCommitted   = 1,
    RepeatableRead  = 2,
    Serializable    = 3,
};

/** @brief Connection state. */
enum class ConnectionState : uint8_t {
    Idle        = 0,  ///< Waiting in the pool
    Active      = 1,  ///< Executing a query
    Transaction = 2,  ///< Inside a transaction
    Closed      = 3,  ///< Connection closed
};

// ─── Row ─────────────────────────────────────────────────────────────────────

/**
 * @brief Interface for a single query result row.
 *
 * Column values are returned as `db::Value`, referencing the buffer directly as a view
 * with no heap allocation.
 */
class IRow {
public:
    virtual ~IRow() = default;

    /** @brief Returns the number of columns. */
    [[nodiscard]] virtual uint16_t column_count() const noexcept = 0;

    /** @brief Returns the column name (zero-copy). */
    [[nodiscard]] virtual std::string_view column_name(uint16_t idx) const noexcept = 0;

    /** @brief Returns the column value as `db::Value` (zero-copy). */
    [[nodiscard]] virtual Value get(uint16_t idx) const noexcept = 0;

    /** @brief Looks up a value by column name. */
    [[nodiscard]] virtual Value get(std::string_view name) const noexcept = 0;
};

// ─── ResultSet ────────────────────────────────────────────────────────────────

/**
 * @brief Async result stream interface.
 *
 * `next()` co_awaits until the next row is ready.
 * Returns `nullptr` when no more rows are available.
 */
class IResultSet {
public:
    virtual ~IResultSet() = default;

    /**
     * @brief Asynchronously fetches the next row.
     * @returns `IRow*` if a row exists (lifetime managed by ResultSet), otherwise `nullptr`.
     */
    virtual Task<const IRow*> next() = 0;

    /** @brief Number of rows affected (INSERT/UPDATE/DELETE). */
    [[nodiscard]] virtual uint64_t affected_rows() const noexcept = 0;

    /** @brief Last inserted ID (only for drivers that support it). */
    [[nodiscard]] virtual uint64_t last_insert_id() const noexcept = 0;
};

// ─── Statement ────────────────────────────────────────────────────────────────

/**
 * @brief Prepared Statement interface.
 *
 * Repeatedly executes a query pre-parsed and optimized on the server side.
 * Parameter binding is passed as a `db::Value` span with no heap allocation.
 */
class IStatement {
public:
    virtual ~IStatement() = default;

    /**
     * @brief Executes the query with bound parameters.
     * @param params Array of `db::Value` (created via BoundParams::span()).
     * @returns Async result stream.
     */
    virtual Task<Result<std::unique_ptr<IResultSet>>>
    execute(std::span<const Value> params = {}) = 0;

    /** @brief Executes without a result set (INSERT/UPDATE/DELETE). */
    virtual Task<Result<uint64_t>>
    execute_dml(std::span<const Value> params = {}) = 0;
};

// ─── Transaction ──────────────────────────────────────────────────────────────

/**
 * @brief Transaction interface.
 *
 * Supports ACID transactions and Savepoints.
 */
class ITransaction {
public:
    virtual ~ITransaction() = default;

    /** @brief Commits the transaction. */
    virtual Task<Result<void>> commit() = 0;

    /** @brief Rolls back the transaction. */
    virtual Task<Result<void>> rollback() = 0;

    /** @brief Creates a savepoint. */
    virtual Task<Result<void>> savepoint(std::string_view name) = 0;

    /** @brief Rolls back to a savepoint. */
    virtual Task<Result<void>> rollback_to(std::string_view name) = 0;

    /** @brief Executes a query directly within this transaction context. */
    virtual Task<Result<uint64_t>>
    execute(std::string_view sql, std::span<const Value> params = {}) = 0;
};

// ─── Connection ───────────────────────────────────────────────────────────────

/**
 * @brief Single DB connection interface.
 *
 * Acquired from a ConnectionPool and automatically returned to the pool on RAII destruction.
 */
class IConnection {
public:
    virtual ~IConnection() = default;

    /** @brief Returns the current connection state. */
    [[nodiscard]] virtual ConnectionState state() const noexcept = 0;

    /**
     * @brief Creates a Prepared Statement.
     * @param sql SQL string. Parameters use `$1`, `$2` ... or `?` placeholders.
     */
    virtual Task<Result<std::unique_ptr<IStatement>>>
    prepare(std::string_view sql) = 0;

    /**
     * @brief Executes a query directly (without prepare).
     * @param sql SQL string.
     * @param params Binding parameters.
     */
    virtual Task<Result<std::unique_ptr<IResultSet>>>
    query(std::string_view sql, std::span<const Value> params = {}) = 0;

    /**
     * @brief Begins a transaction.
     * @param level Isolation level.
     */
    virtual Task<Result<std::unique_ptr<ITransaction>>>
    begin(IsolationLevel level = IsolationLevel::ReadCommitted) = 0;

    /** @brief Closes the connection (returns to pool). */
    virtual Task<Result<void>> close() = 0;

    /** @brief Connection health check (ping). Used for O(1) handover validation. */
    [[nodiscard]] virtual Task<bool> ping() = 0;
};

// ─── ConnectionPool ───────────────────────────────────────────────────────────

/**
 * @brief DB connection pool interface.
 *
 * ## O(1) Connection Handover
 * Uses an internal lock-free ring buffer to guarantee O(1) connection acquisition.
 * If the pool is empty, co_awaits until a connection is returned.
 *
 * ## Pool Size Policy
 * - `min_size`: Number of connections pre-created during warmup.
 * - `max_size`: Maximum number of concurrent connections.
 * - Excess connections are automatically released when usage drops below `min_size`.
 */
class IConnectionPool {
public:
    virtual ~IConnectionPool() = default;

    /**
     * @brief Acquires a connection.
     *
     * Returns immediately if a connection is available (O(1) lock-free).
     * Otherwise co_awaits until a connection is returned.
     *
     * @returns Connection handle (automatically returned to pool on destruction).
     */
    virtual Task<Result<std::unique_ptr<IConnection>>> acquire() = 0;

    /** @brief Current number of active connections. */
    [[nodiscard]] virtual size_t active_count() const noexcept = 0;

    /** @brief Current number of idle connections. */
    [[nodiscard]] virtual size_t idle_count() const noexcept = 0;

    /** @brief Maximum pool size. */
    [[nodiscard]] virtual size_t max_size() const noexcept = 0;

    /** @brief Drains the pool (closes all connections). */
    virtual Task<void> drain() = 0;

    /** @brief Returns a connection to the pool. Called from the PooledConnection destructor. */
    virtual void return_connection(std::unique_ptr<IConnection>) noexcept {}
};

// ─── PoolConfig ───────────────────────────────────────────────────────────────

/** @brief ConnectionPool configuration struct. */
struct PoolConfig {
    size_t   min_size{2};          ///< Minimum number of connections to maintain
    size_t   max_size{16};         ///< Maximum number of connections
    uint32_t connect_timeout_ms{5000};  ///< Connection timeout (ms)
    uint32_t idle_timeout_ms{60000};    ///< Idle connection release time (ms)
    uint32_t query_timeout_ms{30000};   ///< Query timeout (ms)
    bool     tls{false};                ///< Whether TLS is enabled
};

// ─── IDBDriver ────────────────────────────────────────────────────────────────

/**
 * @brief Unified DB driver interface.
 *
 * Concrete drivers (PostgreSQL, Redis, ScyllaDB, etc.) implement this interface.
 * `IDBDriver` acts as a factory for `ConnectionPool`.
 */
class IDBDriver {
public:
    virtual ~IDBDriver() = default;

    /**
     * @brief Returns the driver identifier.
     * @returns e.g. "postgresql", "redis", "mysql"
     */
    [[nodiscard]] virtual std::string_view driver_name() const noexcept = 0;

    /**
     * @brief Creates a ConnectionPool from a DSN.
     * @param dsn Data Source Name (e.g. "postgresql://user:pass@host:5432/db").
     * @param config Pool configuration.
     */
    virtual Task<Result<std::unique_ptr<IConnectionPool>>>
    pool(std::string_view dsn, PoolConfig config = {}) = 0;

    /**
     * @brief Checks whether this driver can handle the given DSN.
     * @param dsn Data Source Name.
     * @returns true if this driver accepts the DSN.
     */
    [[nodiscard]] virtual bool accepts(std::string_view dsn) const noexcept = 0;
};

// ─── DriverRegistry ───────────────────────────────────────────────────────────

/**
 * @brief Global driver registry.
 *
 * Drivers are registered via `DriverRegistry::register_driver()` and
 * the appropriate driver is automatically selected based on the DSN.
 *
 * @note Thread-safe. Registration should be performed once at program startup.
 */
class DriverRegistry {
public:
    /** @brief Maximum number of registered drivers (no dynamic allocation). */
    static constexpr size_t kMaxDrivers = 8;

    /** @brief Registers a driver. */
    static bool register_driver(IDBDriver* driver) noexcept {
        auto& reg = instance();
        size_t idx = reg.count_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= kMaxDrivers) {
            reg.count_.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        reg.drivers_[idx].store(driver, std::memory_order_release);
        return true;
    }

    /**
     * @brief Returns the driver matching the given DSN.
     * @param dsn Data Source Name.
     * @returns Driver pointer, or nullptr if not found.
     */
    static IDBDriver* find(std::string_view dsn) noexcept {
        auto& reg = instance();
        size_t n = reg.count_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            auto* d = reg.drivers_[i].load(std::memory_order_acquire);
            if (d && d->accepts(dsn))
                return d;
        }
        return nullptr;
    }

private:
    DriverRegistry() = default;
    static DriverRegistry& instance() {
        static DriverRegistry reg;
        return reg;
    }

    std::atomic<IDBDriver*> drivers_[kMaxDrivers]{};
    std::atomic<size_t>     count_{0};
};

} // namespace qbuem::db

/** @} */
