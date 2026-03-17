/**
 * @file db_session_example.cpp
 * @brief DB 연결 풀 + 세션 저장소 예제.
 *
 * ## 커버리지 — db/connection_pool.hpp
 * - IConnection                — 연결 인터페이스 (query, prepare, ping)
 * - IConnectionPool            — 풀 인터페이스 (acquire, active_count, idle_count)
 * - LockFreeConnectionPool     — Lock-free O(1) 연결 풀
 * - PoolConfig                 — 풀 설정 (min_size/max_size/idle_timeout_ms)
 * - LockFreeConnectionPool::acquire()  — 연결 획득
 * - PooledConnection           — RAII 연결 소유권
 *
 * ## 커버리지 — core/session_store.hpp
 * - ISessionStore              — 세션 저장소 인터페이스
 * - ISessionStore::get()       — 세션 조회
 * - ISessionStore::set()       — 세션 저장 (TTL 포함)
 * - ISessionStore::del()       — 세션 삭제
 * - ISessionStore::touch()     — TTL 갱신
 */

#include <qbuem_json/qbuem_json.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/session_store.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/db/connection_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/// 세션 페이로드 DTO — ISessionStore 에 저장되는 JSON 데이터.
struct SessionData {
    int         user_id = 0;
    std::string role;
};
QBUEM_JSON_FIELDS(SessionData, user_id, role)

using namespace qbuem;
using namespace qbuem::db;
using namespace std::chrono_literals;

// qbuem_json 포함 시 qbuem::Value(JSON)와 qbuem::db::Value 이름 충돌 방지
using DbValue = qbuem::db::Value;

// ─────────────────────────────────────────────────────────────────────────────
// §1  Mock DB 연결 구현
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
        std::printf("  [MockStmt#%d] execute: %s\n", conn_id_, sql_.c_str());
        std::vector<std::vector<std::string>> rows = {
            {"id", "name"},
            {"1",  "Alice"},
        };
        co_return std::unique_ptr<IResultSet>(std::make_unique<MockResultSet>(std::move(rows)));
    }

    Task<Result<uint64_t>>
    execute_dml(std::span<const DbValue> /*params*/ = {}) override {
        std::printf("  [MockStmt#%d] execute_dml: %s\n", conn_id_, sql_.c_str());
        co_return uint64_t{1};
    }

private:
    int         conn_id_;
    std::string sql_;
};

class MockConnection final : public IConnection {
public:
    explicit MockConnection(int id) : id_(id) {
        std::printf("  [MockConn] #%d 연결 생성\n", id_);
    }
    ~MockConnection() override {
        std::printf("  [MockConn] #%d 연결 해제\n", id_);
    }

    ConnectionState state() const noexcept override {
        return ConnectionState::Idle;
    }

    Task<Result<std::unique_ptr<IStatement>>>
    prepare(std::string_view sql) override {
        std::printf("  [MockConn#%d] prepare: %.*s\n",
                    id_, static_cast<int>(sql.size()), sql.data());
        co_return std::unique_ptr<IStatement>(std::make_unique<MockStatement>(id_, std::string(sql)));
    }

    Task<Result<std::unique_ptr<IResultSet>>>
    query(std::string_view sql, std::span<const DbValue> /*params*/ = {}) override {
        std::printf("  [MockConn#%d] query: %.*s\n",
                    id_, static_cast<int>(sql.size()), sql.data());
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

    Task<Result<void>> close() override { co_return Result<void>::ok(); }

    Task<bool> ping() override { co_return true; }

    int id() const noexcept { return id_; }

private:
    int id_;
};

// ─────────────────────────────────────────────────────────────────────────────
// §2  LockFreeConnectionPool 데모
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_pool_done{false};

static Task<void> demo_connection_pool_task() {
    std::printf("── §2  LockFreeConnectionPool ──\n");

    // 연결 팩토리: MockConnection 생성
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

    // 초기 연결 warm-up
    auto init_r = co_await pool.warmup();
    if (!init_r)
        std::printf("  warmup 실패: %s\n", init_r.error().message().c_str());
    else
        std::printf("  풀 초기화 완료: 유휴 %zu개 연결\n", pool.idle_count());

    // 연결 획득 → 쿼리 → 반납 (PooledConnection RAII)
    {
        auto guard_r = co_await PooledConnection::acquire(pool);
        if (!guard_r) {
            std::printf("  연결 획득 실패: %s\n",
                        guard_r.error().message().c_str());
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
                std::printf("   ");
                for (auto& cell : mrow->cells())
                    std::printf(" [%s]", cell.c_str());
                std::printf("\n");
            }
            std::printf("  쿼리 결과 행 수: %zu\n", row_count);
        }
        // guard 소멸 시 자동 반납
    }

    // prepare + execute_dml 테스트
    {
        auto guard_r = co_await PooledConnection::acquire(pool);
        if (guard_r) {
            PooledConnection guard(std::move(*guard_r));
            auto stmt_r = co_await guard->prepare(
                "UPDATE users SET active=1 WHERE id=$1");
            if (stmt_r) {
                auto affected_r = co_await (*stmt_r)->execute_dml();
                if (affected_r)
                    std::printf("  execute_dml: %llu 행 영향\n",
                                static_cast<unsigned long long>(*affected_r));
            }
        }
    }

    std::printf("  풀 활성:%zu  유휴:%zu\n\n",
                pool.active_count(), pool.idle_count());
    g_pool_done.store(true, std::memory_order_release);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  ISessionStore (LocalSessionStore) 데모
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
        co_return Result<void>::ok();
    }

    Task<Result<void>> del(std::string_view session_id) override {
        std::lock_guard lock(mu_);
        data_.erase(std::string(session_id));
        co_return Result<void>::ok();
    }

    Task<Result<void>>
    touch(std::string_view /*session_id*/,
          std::chrono::seconds /*ttl*/ = std::chrono::seconds{3600}) override {
        // No real expiry tracking in this demo store — just succeed
        co_return Result<void>::ok();
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
    std::printf("── §3  LocalSessionStore ──\n");

    LocalSessionStore store;

    // 세션 생성
    auto set_r = co_await store.set(
        "sess-001", qbuem::write(SessionData{42, "admin"}), 3600s);
    std::printf("  set(sess-001): %s\n",
                set_r ? "ok" : set_r.error().message().c_str());

    // 세션 조회
    auto get_r = co_await store.get("sess-001");
    if (get_r && get_r->has_value())
        std::printf("  get(sess-001): %s\n", get_r->value().c_str());
    else
        std::printf("  get(sess-001): 없음\n");

    // 존재하지 않는 세션 조회
    auto miss = co_await store.get("sess-999");
    std::printf("  get(sess-999): %s\n",
                (miss && !miss->has_value()) ? "nullopt (정상)" : "unexpected");

    // TTL 갱신
    auto touch_r = co_await store.touch("sess-001", 7200s);
    std::printf("  touch(sess-001): %s\n", touch_r ? "ok" : "실패");

    // 두 번째 세션 생성
    co_await store.set("sess-002", qbuem::write(SessionData{7, "user"}), 1800s);

    // 세션 수 확인
    std::printf("  세션 수: %zu\n", store.count());

    // 세션 삭제
    auto del_r = co_await store.del("sess-001");
    std::printf("  del(sess-001): %s\n", del_r ? "ok" : "실패");
    std::printf("  세션 수 (삭제 후): %zu\n\n", store.count());

    g_session_done.store(true, std::memory_order_release);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem DB 연결 풀 + 세션 저장소 예제 ===\n\n");

    Dispatcher disp(2);
    std::thread t([&] { disp.run(); });

    auto pool_fn    = []() -> Task<void> { co_await demo_connection_pool_task(); };
    auto session_fn = []() -> Task<void> { co_await demo_session_store_task(); };

    disp.spawn(pool_fn());

    // 풀 데모 완료 후 세션 데모 실행
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!g_pool_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.spawn(session_fn());

    deadline = std::chrono::steady_clock::now() + 3s;
    while (!g_session_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    std::printf("=== 완료 ===\n");
    return 0;
}
