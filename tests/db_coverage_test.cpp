/**
 * @file tests/db_coverage_test.cpp
 * @brief Deep coverage for the qbuem::db layer beyond db_value_test.cpp.
 *
 * Adds NEW coverage (not duplicating db_value_test / enhancement_test):
 *  - db::Value: blob equality identity, float/int/bool extraction edges,
 *    is<T>() negative paths for every type, BlobView default/empty,
 *    BoundParams.bind to-the-brim and span() over partially-filled storage.
 *  - driver.hpp interfaces: a tiny in-test mock IConnection / IResultSet /
 *    IRow / IStatement implementing the pure-virtual contract; exercises
 *    state(), column lookup by index and name, get(idx) value extraction,
 *    affected_rows / last_insert_id, and the std::expected (Result<T>)
 *    value AND error paths of query()/prepare().
 *  - DriverRegistry: register_driver + find() match / no-match / nullptr.
 *  - connection_pool.hpp: LockFreeConnectionPool driven in-process via a mock
 *    factory — acquire() from empty pool (factory path), warmup() pre-create,
 *    acquire() returns idle conn, max_size backpressure (error path),
 *    return_connection round-trip, idle/active counters, drain() then acquire
 *    (operation_canceled error path), PooledConnection RAII return.
 *  - smart_cache.hpp: SmartCache<T,N> put/get hit, miss, overwrite, TTL expiry
 *    miss, invalidate / invalidate_all, size(), hit_rate(), stats counters,
 *    eviction when full, key longer than KeyLen truncation.
 *
 * All tests are single-process and deterministic. The pool/driver coroutines
 * never perform real I/O — the mock factory is a synchronous coroutine that
 * co_returns immediately, so a single .resume() drives each Task to done.
 */

#include <gtest/gtest.h>

#include <qbuem/db/value.hpp>
#include <qbuem/db/driver.hpp>
#include <qbuem/db/connection_pool.hpp>
#include <qbuem/db/smart_cache.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace qbuem::db;

// ─── Synchronous Task driver ──────────────────────────────────────────────────
// The db mock coroutines never suspend on real I/O: initial_suspend is
// suspend_always, so a single resume() runs the body to completion. We then
// read the produced value directly out of the promise.
template <typename T>
static T sync_await(Task<T>&& t) {
    t.handle.resume();
    // After one resume the coroutine has run to its co_return (no real suspend).
    return std::move(*t.handle.promise().value);
}

static void sync_await_void(Task<void>&& t) {
    t.handle.resume();
}

// ══════════════════════════════════════════════════════════════════════════════
//  db::Value — additional coverage
// ══════════════════════════════════════════════════════════════════════════════

TEST(DbValueDeep, IsNegativePathsForEveryType) {
    Value vi = int64_t{1};
    EXPECT_TRUE(vi.is<int64_t>());
    EXPECT_FALSE(vi.is<double>());
    EXPECT_FALSE(vi.is<bool>());
    EXPECT_FALSE(vi.is<std::string_view>());
    EXPECT_FALSE(vi.is<BufferView>());
    EXPECT_FALSE(vi.is<Null>());

    Value vb = true;
    EXPECT_TRUE(vb.is<bool>());
    EXPECT_FALSE(vb.is<int64_t>());

    Value vd = 2.5;
    EXPECT_TRUE(vd.is<double>());
    EXPECT_FALSE(vd.is<int64_t>());
}

TEST(DbValueDeep, BoolExtractionTruthiness) {
    EXPECT_TRUE(Value{true}.get<bool>());
    EXPECT_FALSE(Value{false}.get<bool>());
    // is<bool>() must NOT match an Int64-tagged value even though both use i64_.
    EXPECT_FALSE(Value{int64_t{1}}.is<bool>());
}

TEST(DbValueDeep, FloatBoundaryValues) {
    Value vneg = -0.0;
    EXPECT_EQ(vneg.type(), Value::Type::Float64);
    EXPECT_DOUBLE_EQ(vneg.get<double>(), 0.0);

    Value vbig = 1.7e308;
    EXPECT_DOUBLE_EQ(vbig.get<double>(), 1.7e308);
}

TEST(DbValueDeep, Int64Boundaries) {
    Value vmin = int64_t{INT64_MIN};
    Value vmax = int64_t{INT64_MAX};
    EXPECT_EQ(vmin.get<int64_t>(), INT64_MIN);
    EXPECT_EQ(vmax.get<int64_t>(), INT64_MAX);
}

TEST(DbValueDeep, EmptyTextAndBlob) {
    Value vt = std::string_view{};
    EXPECT_EQ(vt.type(), Value::Type::Text);
    EXPECT_TRUE(vt.get<std::string_view>().empty());

    Value vbl = BufferView{};
    EXPECT_EQ(vbl.type(), Value::Type::Blob);
    EXPECT_EQ(vbl.get<BufferView>().size(), 0u);
}

TEST(DbValueDeep, BlobEqualityIsIdentityBased) {
    static const uint8_t a[] = {1, 2, 3};
    static const uint8_t b[] = {1, 2, 3};
    Value va = BufferView{a, sizeof(a)};
    Value va2 = BufferView{a, sizeof(a)};
    Value vb = BufferView{b, sizeof(b)};
    // Same pointer + size → equal.
    EXPECT_EQ(va, va2);
    // Same bytes but different pointer → NOT equal (identity comparison).
    EXPECT_NE(va, vb);
}

TEST(DbValueDeep, FloatEqualityAndTypeTagSeparation) {
    EXPECT_EQ(Value{1.25}, Value{1.25});
    EXPECT_NE(Value{1.25}, Value{1.26});
    // Float vs Int with the "same" numeric content are different types.
    EXPECT_NE(Value{1.0}, Value{int64_t{1}});
}

TEST(DbValueDeep, BoundParamsFilledToBrim) {
    BoundParams<3> p;
    p.bind(int64_t{10});
    p.bind(2.0);
    p.bind(std::string_view{"x"});
    auto sp = p.span();
    EXPECT_EQ(sp.size(), 3u);
    EXPECT_EQ(sp[0].get<int64_t>(), 10);
    EXPECT_DOUBLE_EQ(sp[1].get<double>(), 2.0);
    EXPECT_EQ(sp[2].get<std::string_view>(), "x");
}

TEST(DbValueDeep, BoundParamsSpanReflectsPartialFill) {
    BoundParams<8> p;
    EXPECT_EQ(p.span().size(), 0u);  // nothing bound yet
    p.bind(int64_t{7});
    EXPECT_EQ(p.span().size(), 1u);
}

// ══════════════════════════════════════════════════════════════════════════════
//  driver.hpp — in-test mock implementations of the abstract interfaces
// ══════════════════════════════════════════════════════════════════════════════

namespace {

// A row backed by a fixed column set. Names + values are owned by the fixture.
struct MockRow final : IRow {
    std::vector<std::string> names_;
    std::vector<Value>       values_;

    [[nodiscard]] uint16_t column_count() const noexcept override {
        return static_cast<uint16_t>(values_.size());
    }
    [[nodiscard]] std::string_view column_name(uint16_t idx) const noexcept override {
        return (idx < names_.size()) ? std::string_view{names_[idx]} : std::string_view{};
    }
    [[nodiscard]] Value get(uint16_t idx) const noexcept override {
        return (idx < values_.size()) ? values_[idx] : Value{};
    }
    [[nodiscard]] Value get(std::string_view name) const noexcept override {
        for (size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == name) return values_[i];
        return Value{};  // not found → NULL
    }
};

struct MockResultSet final : IResultSet {
    std::vector<MockRow> rows_;
    size_t               cursor_{0};
    uint64_t             affected_{0};
    uint64_t             last_id_{0};

    Task<const IRow*> next() override {
        if (cursor_ < rows_.size()) {
            const IRow* r = &rows_[cursor_++];
            co_return r;
        }
        co_return nullptr;
    }
    [[nodiscard]] uint64_t affected_rows() const noexcept override { return affected_; }
    [[nodiscard]] uint64_t last_insert_id() const noexcept override { return last_id_; }
};

struct MockStatement final : IStatement {
    bool fail_{false};
    Task<Result<std::unique_ptr<IResultSet>>>
    execute(std::span<const Value> /*params*/ = {}) override {
        if (fail_)
            co_return std::unexpected(std::make_error_code(std::errc::io_error));
        auto rs = std::make_unique<MockResultSet>();
        rs->affected_ = 0;
        co_return std::unique_ptr<IResultSet>(std::move(rs));
    }
    Task<Result<uint64_t>>
    execute_dml(std::span<const Value> params = {}) override {
        if (fail_)
            co_return std::unexpected(std::make_error_code(std::errc::io_error));
        co_return static_cast<uint64_t>(params.size());
    }
};

// A connection that hands back canned data and can be told to fail prepare().
struct MockConnection final : IConnection {
    ConnectionState state_{ConnectionState::Idle};
    bool            fail_prepare_{false};
    bool            fail_query_{false};

    [[nodiscard]] ConnectionState state() const noexcept override { return state_; }

    Task<Result<std::unique_ptr<IStatement>>>
    prepare(std::string_view /*sql*/) override {
        if (fail_prepare_)
            co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        co_return std::unique_ptr<IStatement>(std::make_unique<MockStatement>());
    }

    Task<Result<std::unique_ptr<IResultSet>>>
    query(std::string_view /*sql*/, std::span<const Value> /*params*/ = {}) override {
        if (fail_query_)
            co_return std::unexpected(std::make_error_code(std::errc::connection_refused));
        auto rs = std::make_unique<MockResultSet>();
        MockRow row;
        row.names_  = {"id", "name", "score"};
        row.values_ = {Value{int64_t{42}},
                       Value{std::string_view{"alice"}},
                       Value{3.5}};
        rs->rows_.push_back(std::move(row));
        rs->affected_ = 1;
        rs->last_id_  = 99;
        co_return std::unique_ptr<IResultSet>(std::move(rs));
    }

    Task<Result<std::unique_ptr<ITransaction>>>
    begin(IsolationLevel /*level*/ = IsolationLevel::ReadCommitted) override {
        // Transactions are out of scope for this mock — report unsupported cleanly.
        co_return std::unexpected(std::make_error_code(std::errc::not_supported));
    }

    Task<Result<void>> close() override {
        state_ = ConnectionState::Closed;
        co_return Result<void>{};
    }

    Task<bool> ping() override { co_return state_ != ConnectionState::Closed; }
};

// Minimal driver implementing IDBDriver for DriverRegistry tests.
struct MockDriver final : IDBDriver {
    std::string_view name_{"mockdb"};
    [[nodiscard]] std::string_view driver_name() const noexcept override { return name_; }
    Task<Result<std::unique_ptr<IConnectionPool>>>
    pool(std::string_view /*dsn*/, PoolConfig /*config*/ = {}) override {
        co_return std::unexpected(std::make_error_code(std::errc::not_supported));
    }
    [[nodiscard]] bool accepts(std::string_view dsn) const noexcept override {
        return dsn.starts_with("mockdb://");
    }
};

}  // namespace

TEST(DbDriverMock, EnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(IsolationLevel::ReadUncommitted), 0u);
    EXPECT_EQ(static_cast<uint8_t>(IsolationLevel::ReadCommitted),   1u);
    EXPECT_EQ(static_cast<uint8_t>(IsolationLevel::RepeatableRead),  2u);
    EXPECT_EQ(static_cast<uint8_t>(IsolationLevel::Serializable),    3u);

    EXPECT_EQ(static_cast<uint8_t>(ConnectionState::Idle),        0u);
    EXPECT_EQ(static_cast<uint8_t>(ConnectionState::Active),      1u);
    EXPECT_EQ(static_cast<uint8_t>(ConnectionState::Transaction), 2u);
    EXPECT_EQ(static_cast<uint8_t>(ConnectionState::Closed),      3u);
}

TEST(DbDriverMock, ConnectionStateAndPing) {
    MockConnection conn;
    EXPECT_EQ(conn.state(), ConnectionState::Idle);
    EXPECT_TRUE(sync_await(conn.ping()));

    auto cr = sync_await(conn.close());
    EXPECT_TRUE(cr.has_value());
    EXPECT_EQ(conn.state(), ConnectionState::Closed);
    EXPECT_FALSE(sync_await(conn.ping()));
}

TEST(DbDriverMock, QueryValuePathRowExtraction) {
    MockConnection conn;
    auto r = sync_await(conn.query("SELECT * FROM users"));
    ASSERT_TRUE(r.has_value());
    auto& rs = *r;
    EXPECT_EQ(rs->affected_rows(), 1u);
    EXPECT_EQ(rs->last_insert_id(), 99u);

    const IRow* row = sync_await(rs->next());
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->column_count(), 3u);
    EXPECT_EQ(row->column_name(0), "id");
    EXPECT_EQ(row->column_name(1), "name");
    EXPECT_EQ(row->get(0).get<int64_t>(), 42);
    EXPECT_EQ(row->get(1).get<std::string_view>(), "alice");
    EXPECT_DOUBLE_EQ(row->get(2).get<double>(), 3.5);

    // Lookup by column name.
    EXPECT_EQ(row->get(std::string_view{"name"}).get<std::string_view>(), "alice");
    // Unknown column → NULL value.
    EXPECT_TRUE(row->get(std::string_view{"missing"}).is_null());

    // Cursor exhaustion → nullptr.
    EXPECT_EQ(sync_await(rs->next()), nullptr);
}

TEST(DbDriverMock, QueryErrorPath) {
    MockConnection conn;
    conn.fail_query_ = true;
    auto r = sync_await(conn.query("BAD"));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::connection_refused));
}

TEST(DbDriverMock, PrepareValueAndErrorPaths) {
    MockConnection conn;
    auto ok = sync_await(conn.prepare("SELECT 1"));
    ASSERT_TRUE(ok.has_value());
    ASSERT_NE(ok->get(), nullptr);

    // Statement execute_dml value path returns param count.
    BoundParams<2> p;
    p.bind(int64_t{1});
    p.bind(int64_t{2});
    auto dml = sync_await((*ok)->execute_dml(p.span()));
    ASSERT_TRUE(dml.has_value());
    EXPECT_EQ(*dml, 2u);

    // Statement execute value path returns a result set.
    auto rs = sync_await((*ok)->execute());
    ASSERT_TRUE(rs.has_value());
    EXPECT_EQ((*rs)->affected_rows(), 0u);

    conn.fail_prepare_ = true;
    auto bad = sync_await(conn.prepare("SELECT 1"));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(DbDriverMock, BeginReportsUnsupportedCleanly) {
    MockConnection conn;
    auto tx = sync_await(conn.begin(IsolationLevel::Serializable));
    ASSERT_FALSE(tx.has_value());
    EXPECT_EQ(tx.error(), std::make_error_code(std::errc::not_supported));
}

TEST(DbDriverMock, StatementExecuteErrorPath) {
    auto stmt = std::make_unique<MockStatement>();
    stmt->fail_ = true;
    auto rs = sync_await(stmt->execute());
    ASSERT_FALSE(rs.has_value());
    EXPECT_EQ(rs.error(), std::make_error_code(std::errc::io_error));
    auto dml = sync_await(stmt->execute_dml());
    ASSERT_FALSE(dml.has_value());
}

// ─── DriverRegistry ───────────────────────────────────────────────────────────

TEST(DbDriverRegistry, RegisterAndFind) {
    static MockDriver driver;  // static lifetime — registry stores raw pointer
    EXPECT_EQ(driver.driver_name(), "mockdb");
    EXPECT_TRUE(driver.accepts("mockdb://localhost/db"));
    EXPECT_FALSE(driver.accepts("postgresql://x"));

    ASSERT_TRUE(DriverRegistry::register_driver(&driver));

    IDBDriver* found = DriverRegistry::find("mockdb://host/db");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->driver_name(), "mockdb");

    // No driver accepts an unknown scheme.
    EXPECT_EQ(DriverRegistry::find("unknownscheme://x"), nullptr);
}

TEST(DbDriverRegistry, PoolFactoryUnsupportedPath) {
    MockDriver driver;
    auto pr = sync_await(driver.pool("mockdb://x"));
    ASSERT_FALSE(pr.has_value());
    EXPECT_EQ(pr.error(), std::make_error_code(std::errc::not_supported));
}

// ══════════════════════════════════════════════════════════════════════════════
//  connection_pool.hpp — LockFreeConnectionPool driven in-process
// ══════════════════════════════════════════════════════════════════════════════

namespace {

// A factory that fabricates MockConnections and counts how many it created.
struct CountingFactory {
    int* counter;
    Task<Result<std::unique_ptr<IConnection>>> operator()() const {
        ++(*counter);
        co_return std::unique_ptr<IConnection>(std::make_unique<MockConnection>());
    }
};

}  // namespace

TEST(DbConnectionPool, AcquireFromEmptyCreatesViaFactory) {
    int created = 0;
    PoolConfig cfg;
    cfg.min_size = 0;
    cfg.max_size = 2;
    LockFreeConnectionPool pool(CountingFactory{&created}, cfg);

    EXPECT_EQ(pool.max_size(), 2u);
    EXPECT_EQ(pool.idle_count(), 0u);
    EXPECT_EQ(pool.active_count(), 0u);

    auto r = sync_await(pool.acquire());
    ASSERT_TRUE(r.has_value());
    ASSERT_NE(r->get(), nullptr);
    EXPECT_EQ(created, 1);
    EXPECT_EQ(pool.active_count(), 1u);
}

TEST(DbConnectionPool, WarmupPreCreatesMinSize) {
    int created = 0;
    PoolConfig cfg;
    cfg.min_size = 3;
    cfg.max_size = 8;
    LockFreeConnectionPool pool(CountingFactory{&created}, cfg);

    auto wr = sync_await(pool.warmup());
    ASSERT_TRUE(wr.has_value());
    EXPECT_EQ(created, 3);
    EXPECT_EQ(pool.idle_count(), 3u);

    // Acquire now reuses an idle connection (no new factory call).
    auto r = sync_await(pool.acquire());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(created, 3);             // unchanged — pulled from idle
    EXPECT_EQ(pool.idle_count(), 2u);
    EXPECT_EQ(pool.active_count(), 1u);
}

TEST(DbConnectionPool, MaxSizeBackpressureErrorPath) {
    int created = 0;
    PoolConfig cfg;
    cfg.min_size = 0;
    cfg.max_size = 1;
    LockFreeConnectionPool pool(CountingFactory{&created}, cfg);

    auto first = sync_await(pool.acquire());
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(pool.active_count(), 1u);

    // Pool at max, none idle → resource_unavailable_try_again error path.
    auto second = sync_await(pool.acquire());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(),
              std::make_error_code(std::errc::resource_unavailable_try_again));
}

TEST(DbConnectionPool, ReturnConnectionRoundTrip) {
    int created = 0;
    PoolConfig cfg;
    cfg.min_size = 0;
    cfg.max_size = 2;
    LockFreeConnectionPool pool(CountingFactory{&created}, cfg);

    auto r = sync_await(pool.acquire());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(pool.active_count(), 1u);
    EXPECT_EQ(pool.idle_count(), 0u);

    pool.return_connection(std::move(*r));
    EXPECT_EQ(pool.active_count(), 0u);
    EXPECT_EQ(pool.idle_count(), 1u);

    // Returning a null pointer is a no-op (must not crash / not change counts).
    pool.return_connection(nullptr);
    EXPECT_EQ(pool.idle_count(), 1u);
}

TEST(DbConnectionPool, DrainThenAcquireIsCancelled) {
    int created = 0;
    PoolConfig cfg;
    cfg.min_size = 2;
    cfg.max_size = 4;
    LockFreeConnectionPool pool(CountingFactory{&created}, cfg);

    sync_await(pool.warmup());
    EXPECT_EQ(pool.idle_count(), 2u);

    sync_await_void(pool.drain());
    EXPECT_EQ(pool.idle_count(), 0u);

    // After drain, acquire reports operation_canceled.
    auto r = sync_await(pool.acquire());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::operation_canceled));

    // return_connection after drain is dropped silently.
    pool.return_connection(std::make_unique<MockConnection>());
    EXPECT_EQ(pool.idle_count(), 0u);
}

TEST(DbConnectionPool, PooledConnectionRaiiReturns) {
    int created = 0;
    PoolConfig cfg;
    cfg.min_size = 0;
    cfg.max_size = 2;
    LockFreeConnectionPool pool(CountingFactory{&created}, cfg);

    {
        auto guard = sync_await(PooledConnection::acquire(pool));
        ASSERT_TRUE(guard.has_value());
        EXPECT_TRUE(guard->valid());
        EXPECT_EQ(pool.active_count(), 1u);
        // Use the connection through the guard arrow operator.
        EXPECT_EQ(guard->get().state(), ConnectionState::Idle);
    }  // guard destroyed → connection returned to pool

    EXPECT_EQ(pool.active_count(), 0u);
    EXPECT_EQ(pool.idle_count(), 1u);
}

TEST(DbConnectionPool, IsConnectionPoolSubtype) {
    int created = 0;
    LockFreeConnectionPool pool(CountingFactory{&created}, PoolConfig{});
    IConnectionPool& base = pool;  // upcast must be valid
    EXPECT_GE(base.max_size(), 1u);
}

// ══════════════════════════════════════════════════════════════════════════════
//  smart_cache.hpp — SmartCache<T,N>
// ══════════════════════════════════════════════════════════════════════════════

namespace {
struct Quote {
    double bid;
    double ask;
    int64_t volume;
    bool operator==(const Quote&) const = default;
};
static_assert(std::is_trivially_copyable_v<Quote>);
}  // namespace

TEST(SmartCacheTest, PutGetHit) {
    SmartCache<Quote, 64> cache("unused-name");
    Quote q{100.0, 100.5, 1000};
    cache.put("AAPL", q);

    auto got = cache.get("AAPL");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, q);
    EXPECT_EQ(cache.size(), 1u);

    EXPECT_EQ(cache.stats().hits.load(), 1u);
    EXPECT_EQ(cache.stats().writes.load(), 1u);
}

TEST(SmartCacheTest, MissOnUnknownKey) {
    SmartCache<Quote, 64> cache;
    auto got = cache.get("NOPE");
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(cache.stats().misses.load(), 1u);
    EXPECT_EQ(cache.size(), 0u);
}

TEST(SmartCacheTest, OverwriteSameKeyUpdatesValue) {
    SmartCache<Quote, 64> cache;
    cache.put("X", Quote{1, 1, 1});
    cache.put("X", Quote{2, 2, 2});
    auto got = cache.get("X");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bid, 2.0);
    EXPECT_EQ(cache.size(), 1u);  // still one slot
    EXPECT_EQ(cache.stats().writes.load(), 2u);
}

TEST(SmartCacheTest, InvalidateRemovesEntry) {
    SmartCache<Quote, 64> cache;
    cache.put("K", Quote{5, 5, 5});
    EXPECT_TRUE(cache.invalidate("K"));
    EXPECT_FALSE(cache.get("K").has_value());
    EXPECT_EQ(cache.size(), 0u);
    // Invalidating an absent key returns false.
    EXPECT_FALSE(cache.invalidate("absent"));
}

TEST(SmartCacheTest, InvalidateAllFlushes) {
    SmartCache<Quote, 64> cache;
    cache.put("a", Quote{1, 1, 1});
    cache.put("b", Quote{2, 2, 2});
    cache.put("c", Quote{3, 3, 3});
    EXPECT_EQ(cache.size(), 3u);
    cache.invalidate_all();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_FALSE(cache.get("b").has_value());
}

TEST(SmartCacheTest, TtlExpiryReportsMiss) {
    SmartCache<Quote, 64> cache;
    // 1ns TTL → effectively already expired by the time we read.
    cache.put("EXP", Quote{9, 9, 9}, /*ttl_ns=*/1);
    // Busy-loop a touch so monotonic clock advances past 1ns (deterministic:
    // any non-zero elapsed time exceeds a 1ns TTL).
    volatile uint64_t spin = 0;
    for (int i = 0; i < 100000; ++i) spin += static_cast<uint64_t>(i);
    (void)spin;
    auto got = cache.get("EXP");
    EXPECT_FALSE(got.has_value());  // expired → miss
    EXPECT_EQ(cache.size(), 0u);    // invalidated on expiry
}

TEST(SmartCacheTest, NoTtlNeverExpires) {
    SmartCache<Quote, 64> cache;
    cache.put("PERM", Quote{7, 7, 7}, /*ttl_ns=*/0);
    auto got = cache.get("PERM");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->volume, 7);
}

TEST(SmartCacheTest, HitRateComputation) {
    SmartCache<Quote, 64> cache;
    EXPECT_DOUBLE_EQ(cache.hit_rate(), 0.0);  // no lookups yet
    cache.put("h", Quote{1, 1, 1});
    (void)cache.get("h");      // hit
    (void)cache.get("miss1");  // miss
    (void)cache.get("miss2");  // miss
    // 1 hit / 3 lookups.
    EXPECT_NEAR(cache.hit_rate(), 1.0 / 3.0, 1e-9);
}

TEST(SmartCacheTest, EvictionWhenFull) {
    // Tiny capacity forces eviction. Capacity 2 → 3rd distinct key evicts.
    SmartCache<Quote, 2> cache;
    cache.put("k0", Quote{0, 0, 0});
    cache.put("k1", Quote{1, 1, 1});
    EXPECT_EQ(cache.size(), 2u);
    cache.put("k2", Quote{2, 2, 2});  // all probe slots full → eviction
    // Still at capacity (one of k0/k1 was evicted).
    EXPECT_LE(cache.size(), 2u);
    EXPECT_GE(cache.stats().evictions.load(), 1u);
    // The freshly inserted key must be present.
    auto got = cache.get("k2");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bid, 2.0);
}

TEST(SmartCacheTest, LongKeyTruncatedToKeyLen) {
    // KeyLen 8 → put() stores strncpy(key, src, 7) + NUL, i.e. only the first
    // 7 chars survive. Capacity 1 → exactly one slot, so probing always lands
    // on it regardless of which hash the lookup key produces (this isolates the
    // truncation behaviour from open-addressing early-exit on empty slots).
    SmartCache<Quote, 1, 8> cache;
    std::string long_key = "abcdefghIJKLMNOP";  // 16 chars, > KeyLen
    cache.put(long_key, Quote{1, 2, 3});
    // The 7-char truncated prefix matches the stored slot key "abcdefg\0".
    auto got = cache.get("abcdefg");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->volume, 3);
    // The original full key does NOT match: key_matches strncmp over KeyLen=8
    // compares the stored "abcdefg\0" against "abcdefgh..." and differs at
    // index 7 (NUL vs 'h') — truncation is lossy, as documented.
    EXPECT_FALSE(cache.get(long_key).has_value());
}

TEST(SmartCacheTest, EmptyNameConstructorWorks) {
    SmartCache<Quote, 16> cache;  // default empty name → in-process only
    cache.put("ok", Quote{4, 4, 4});
    EXPECT_TRUE(cache.get("ok").has_value());
}

TEST(SmartCacheTest, SupportsTrivialScalarValue) {
    SmartCache<int64_t, 32> cache;
    cache.put("answer", 42);
    auto got = cache.get("answer");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 42);
    cache.put("answer", 43);
    EXPECT_EQ(*cache.get("answer"), 43);
}
