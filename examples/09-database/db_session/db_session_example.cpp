/**
 * @file db_session_example.cpp
 * @brief DB connection pool + session store example.
 *
 * ## Coverage — db/connection_pool.hpp
 * - IConnection                — connection interface (query, prepare, ping)
 * - IConnectionPool            — pool interface (acquire, active_count, idle_count)
 * - LockFreeConnectionPool     — lock-free O(1) connection pool
 * - PoolConfig                 — pool configuration (min_size/max_size/idle_timeout_ms)
 * - LockFreeConnectionPool::acquire()  — connection acquisition
 * - PooledConnection           — RAII connection ownership
 *
 * ## Coverage — core/session_store.hpp
 * - ISessionStore              — session store interface
 * - ISessionStore::get()       — session lookup
 * - ISessionStore::set()       — session storage (with TTL)
 * - ISessionStore::del()       — session deletion
 * - ISessionStore::touch()     — TTL renewal
 */

#include <qbuem_json/qbuem_json.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/session_store.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/db/connection_pool.hpp>
#include <qbuem/compat/print.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/// Session payload DTO — JSON data stored in ISessionStore.
struct SessionData {
    int         user_id = 0;
    std::string role;
};
QBUEM_JSON_FIELDS(SessionData, user_id, role)

using namespace qbuem;
using namespace qbuem::db;
using namespace std::chrono_literals;

// Prevent name collision between qbuem::Value (JSON) and qbuem::db::Value
using DbValue = qbuem::db::Value;

// ─────────────────────────────────────────────────────────────────────────────
// §1  Mock DB connection implementation
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<int> g_conn_id_counter{0};

// Simple mock IResultSet that holds a fixed set of string rows
class MockResultSet final : public IResultSet {
public:
    struct MockRow final : public IRow {
        explicit MockRow(std::vector<std::string> cells)
            : cells_(std::move(cells)) {}

        uint16_t column_count() const noexcept override {
            return static_cast<uint16_t>(cells_.size());
        }
        std::string_view column_name(uint16_t idx) const noexcept override {
            (void)idx; return {};
        }
        DbValue get(uint16_t idx) const noexcept override {
            (void)idx; return DbValue{};
        }
        DbValue get(std::string_view name) const noexcept override {
            (void)name; return DbValue{};
        }

        const std::vector<std::string>& cells() const { return cells_; }
    private:
        std::vector<std::string> cells_;
    };

    explicit MockResultSet(std::vector<std::vector<std::string>> rows)
        : raw_rows_(std::move(rows)), pos_(0) {}

    Task<const IRow*> next() override {
        if (pos_ >= raw_rows_.size()) co_return nullptr;
        current_ = std::make_unique<MockRow>(raw_rows_[pos_++]);
        co_return current_.get();
    }

    uint64_t affected_rows()    const noexcept override { return 0; }
    uint64_t last_insert_id()   const noexcept override { return 0; }

private:
    std::vector<std::vector<std::string>> raw_rows_;
    size_t                                pos_;
    std::unique_ptr<MockRow>              current_;
};

// Simple mock IStatement
class MockStatement final : public IStatement {
public:
    explicit MockStatement(int conn_id, std::string sql)
        : conn_id_(conn_id), sql_(std::move(sql)) {}

    Task<Result<std::unique_ptr<IResultSet>>>
    execute(std::span<const DbValue> /*params*/ = {}) override {
        std::println("  [MockStmt#{}] execute: {}", conn_id_, sql_);
        std::vector<std::vector<std::string>> rows = {
            {"id", "name"},
            {"1",  "Alice"},
        };
        co_return std::unique_ptr<IResultSet>(std::make_unique<MockResultSet>(std::move(rows)));
    }

    Task<Result<uint64_t>>
    execute_dml(std::span<const DbValue> /*params*/ = {}) override {
        std::println("  [MockStmt#{}] execute_dml: {}", conn_id_, sql_);
        co_return uint64_t{1};
    }

private:
    int         conn_id_;
    std::string sql_;
};

class MockConnection final : public IConnection {
public:
    explicit MockConnection(int id) : id_(id) {
        std::println("  [MockConn] #{} connection created", id_);
    }
    ~MockConnection() override {
        std::println("  [MockConn] #{} connection released", id_);
    }

    ConnectionState state() const noexcept override {
        return ConnectionState::Idle;
    }

    Task<Result<std::unique_ptr<IStatement>>>
    prepare(std::string_view sql) override {
        std::println("  [MockConn#{}] prepare: {}", id_, sql);
        co_return std::unique_ptr<IStatement>(std::make_unique<MockStatement>(id_, std::string(sql)));
    }

    Task<Result<std::unique_ptr<IResultSet>>>
    query(std::string_view sql, std::span<const DbValue> /*params*/ = {}) override {
        std::println("  [MockConn#{}] query: {}", id_, sql);
        std::vector<std::vector<std::string>> rows = {
            {"id", "name"},
            {"1",  "Alice"},
        };
        co_return std::unique_ptr<IResultSet>(std::make_unique<MockResultSet>(std::move(rows)));
    }

    Task<Result<std::unique_ptr<ITransaction>>>
    begin(IsolationLevel /*level*/ = IsolationLevel::ReadCommitted) override {
        co_return unexpected(std::make_error_code(std::errc::not_supported));
    }

    Task<Result<void>> close() override { co_return Result<void>{}; }

    Task<bool> ping() override { co_return true; }

    int id() const noexcept { return id_; }

private:
    int id_;
};

// ─────────────────────────────────────────────────────────────────────────────
// §2  LockFreeConnectionPool demo
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_pool_done{false};

static Task<void> demo_connection_pool_task() {
    std::println("── §2  LockFreeConnectionPool ──");

    // Connection factory: creates MockConnection instances
    LockFreeConnectionPool pool(
        []() -> Task<Result<std::unique_ptr<IConnection>>> {
            int id = ++g_conn_id_counter;
            co_return std::unique_ptr<IConnection>(std::make_unique<MockConnection>(id));
        },
        PoolConfig{
            .min_size        = 2,
            .max_size        = 5,
            .idle_timeout_ms = 5000,
        });

    // Initial connection warm-up
    auto init_r = co_await pool.warmup();
    if (!init_r)
        std::println("  warmup failed: {}", init_r.error().message());
    else
        std::println("  pool initialized: {} idle connections", pool.idle_count());

    // Acquire connection → query → return (PooledConnection RAII)
    {
        auto guard_r = co_await PooledConnection::acquire(pool);
        if (!guard_r) {
            std::println("  connection acquire failed: {}",
                        guard_r.error().message());
            co_return;
        }

        PooledConnection guard(std::move(*guard_r));
        auto rs_r = co_await guard->query("SELECT id, name FROM users LIMIT 2");
        if (rs_r) {
            auto& rs = *rs_r;
            size_t row_count = 0;
            while (auto* row = co_await rs->next()) {
                ++row_count;
                auto* mrow = static_cast<const MockResultSet::MockRow*>(row);
                std::print("   ");
                for (auto& cell : mrow->cells())
                    std::print(" [{}]", cell);
                std::println("");
            }
            std::println("  query result row count: {}", row_count);
        }
        // guard destruction automatically returns the connection
    }

    // prepare + execute_dml test
    {
        auto guard_r = co_await PooledConnection::acquire(pool);
        if (guard_r) {
            PooledConnection guard(std::move(*guard_r));
            auto stmt_r = co_await guard->prepare(
                "UPDATE users SET active=1 WHERE id=$1");
            if (stmt_r) {
                auto affected_r = co_await (*stmt_r)->execute_dml();
                if (affected_r)
                    std::println("  execute_dml: {} rows affected",
                                static_cast<unsigned long long>(*affected_r));
            }
        }
    }

    std::println("  pool active:{} idle:{}\n",
                pool.active_count(), pool.idle_count());
    g_pool_done.store(true, std::memory_order_release);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  ISessionStore (LocalSessionStore) demo
// ─────────────────────────────────────────────────────────────────────────────

// Minimal in-process ISessionStore implementation for demonstration
class LocalSessionStore final : public ISessionStore {
public:
    Task<Result<std::optional<std::string>>>
    get(std::string_view session_id) override {
        std::lock_guard lock(mu_);
        auto it = data_.find(std::string(session_id));
        if (it == data_.end())
            co_return std::optional<std::string>{std::nullopt};
        co_return std::optional<std::string>{it->second};
    }

    Task<Result<void>>
    set(std::string_view session_id, std::string value,
        std::chrono::seconds /*ttl*/ = std::chrono::seconds{3600}) override {
        std::lock_guard lock(mu_);
        data_[std::string(session_id)] = std::move(value);
        co_return Result<void>{};
    }

    Task<Result<void>> del(std::string_view session_id) override {
        std::lock_guard lock(mu_);
        data_.erase(std::string(session_id));
        co_return Result<void>{};
    }

    Task<Result<void>>
    touch(std::string_view /*session_id*/,
          std::chrono::seconds /*ttl*/ = std::chrono::seconds{3600}) override {
        // No real expiry tracking in this demo store — just succeed
        co_return Result<void>{};
    }

    size_t count() const {
        std::lock_guard lock(mu_);
        return data_.size();
    }

private:
    mutable std::mutex          mu_;
    std::map<std::string, std::string> data_;
};

static std::atomic<bool> g_session_done{false};

static Task<void> demo_session_store_task() {
    std::println("── §3  LocalSessionStore ──");

    LocalSessionStore store;

    // Create session
    auto set_r = co_await store.set(
        "sess-001", qbuem::write(SessionData{42, "admin"}), 3600s);
    std::println("  set(sess-001): {}",
                set_r ? "ok" : set_r.error().message());

    // Retrieve session
    auto get_r = co_await store.get("sess-001");
    if (get_r && get_r->has_value())
        std::println("  get(sess-001): {}", get_r->value());
    else
        std::println("  get(sess-001): not found");

    // Retrieve non-existent session
    auto miss = co_await store.get("sess-999");
    std::println("  get(sess-999): {}",
                (miss && !miss->has_value()) ? "nullopt (expected)" : "unexpected");

    // Renew TTL
    auto touch_r = co_await store.touch("sess-001", 7200s);
    std::println("  touch(sess-001): {}", touch_r ? "ok" : "failed");

    // Create second session
    co_await store.set("sess-002", qbuem::write(SessionData{7, "user"}), 1800s);

    // Check session count
    std::println("  session count: {}", store.count());

    // Delete session
    auto del_r = co_await store.del("sess-001");
    std::println("  del(sess-001): {}", del_r ? "ok" : "failed");
    std::println("  session count (after delete): {}\n", store.count());

    g_session_done.store(true, std::memory_order_release);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== qbuem DB connection pool + session store example ===\n");

    Dispatcher disp(2);
    std::jthread t([&] { disp.run(); });

    auto pool_fn    = []() -> Task<void> { co_await demo_connection_pool_task(); };
    auto session_fn = []() -> Task<void> { co_await demo_session_store_task(); };

    disp.spawn(pool_fn());

    // Run session demo after pool demo completes
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!g_pool_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.spawn(session_fn());

    deadline = std::chrono::steady_clock::now() + 3s;
    while (!g_session_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    std::println("=== done ===");
    return 0;
}
