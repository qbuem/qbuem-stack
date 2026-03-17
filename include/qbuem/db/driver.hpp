#pragma once

/**
 * @file qbuem/db/driver.hpp
 * @brief IDBDriver — 통합 DB 드라이버 인터페이스.
 * @defgroup qbuem_db_driver IDBDriver
 * @ingroup qbuem_db
 *
 * `IDBDriver`는 qbuem-stack의 통합 DB 추상화 레이어 최상위 인터페이스입니다.
 *
 * ## 설계 원칙
 * - **O(1) Connection Handover**: 연결 획득은 lock-free 링 버퍼 인덱싱.
 * - **Stateless Protocol Parsing**: 파서는 외부 상태 없이 버퍼만으로 파싱.
 * - **Zero Allocation**: 쿼리 준비 및 결과 스트리밍 시 힙 할당 없음.
 * - **Reactor Alignment**: 모든 I/O는 `IReactor` / `io_uring` 기반.
 *
 * ## 구현 계층
 * ```
 * IDBDriver (인터페이스)
 *   └─ ConnectionPool  (연결 풀 관리)
 *        └─ Connection (단일 세션, begin/prepare/close)
 *             └─ Statement (prepared query + 파라미터 바인딩)
 *                  └─ ResultSet (비동기 결과 스트림)
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

// ─── 열거형 ───────────────────────────────────────────────────────────────────

/** @brief 트랜잭션 격리 수준. SQL:1999 표준을 따릅니다. */
enum class IsolationLevel : uint8_t {
    ReadUncommitted = 0,
    ReadCommitted   = 1,
    RepeatableRead  = 2,
    Serializable    = 3,
};

/** @brief 연결 상태. */
enum class ConnectionState : uint8_t {
    Idle        = 0,  ///< 풀에서 대기 중
    Active      = 1,  ///< 쿼리 실행 중
    Transaction = 2,  ///< 트랜잭션 진행 중
    Closed      = 3,  ///< 연결 종료됨
};

// ─── Row ─────────────────────────────────────────────────────────────────────

/**
 * @brief 쿼리 결과 행(row) 인터페이스.
 *
 * 컬럼값은 `db::Value`로 반환되며, 힙 할당 없이 버퍼를 직접 뷰로 참조합니다.
 */
class IRow {
public:
    virtual ~IRow() = default;

    /** @brief 컬럼 개수를 반환합니다. */
    [[nodiscard]] virtual uint16_t column_count() const noexcept = 0;

    /** @brief 컬럼 이름을 반환합니다 (zero-copy). */
    [[nodiscard]] virtual std::string_view column_name(uint16_t idx) const noexcept = 0;

    /** @brief 컬럼값을 `db::Value`로 반환합니다 (zero-copy). */
    [[nodiscard]] virtual Value get(uint16_t idx) const noexcept = 0;

    /** @brief 컬럼 이름으로 값을 조회합니다. */
    [[nodiscard]] virtual Value get(std::string_view name) const noexcept = 0;
};

// ─── ResultSet ────────────────────────────────────────────────────────────────

/**
 * @brief 비동기 결과 스트림 인터페이스.
 *
 * `next()`는 다음 행이 준비될 때까지 co_await로 대기합니다.
 * 결과가 없으면 `std::nullopt`를 반환합니다.
 */
class IResultSet {
public:
    virtual ~IResultSet() = default;

    /**
     * @brief 다음 행을 비동기적으로 가져옵니다.
     * @returns 행이 있으면 `IRow*` (수명은 ResultSet이 관리), 없으면 `nullptr`.
     */
    virtual Task<const IRow*> next() = 0;

    /** @brief 영향받은 행 수 (INSERT/UPDATE/DELETE). */
    [[nodiscard]] virtual uint64_t affected_rows() const noexcept = 0;

    /** @brief 마지막 삽입 ID (지원하는 드라이버만). */
    [[nodiscard]] virtual uint64_t last_insert_id() const noexcept = 0;
};

// ─── Statement ────────────────────────────────────────────────────────────────

/**
 * @brief Prepared Statement 인터페이스.
 *
 * 서버 측에서 미리 파싱/최적화된 쿼리를 반복 실행합니다.
 * 파라미터 바인딩은 `db::Value` 스팬으로 전달하며 힙 할당이 없습니다.
 */
class IStatement {
public:
    virtual ~IStatement() = default;

    /**
     * @brief 파라미터를 바인딩하여 쿼리를 실행합니다.
     * @param params `db::Value` 배열 (BoundParams::span()으로 생성).
     * @returns 비동기 결과 스트림.
     */
    virtual Task<Result<std::unique_ptr<IResultSet>>>
    execute(std::span<const Value> params = {}) = 0;

    /** @brief 결과 없이 실행 (INSERT/UPDATE/DELETE). */
    virtual Task<Result<uint64_t>>
    execute_dml(std::span<const Value> params = {}) = 0;
};

// ─── Transaction ──────────────────────────────────────────────────────────────

/**
 * @brief 트랜잭션 인터페이스.
 *
 * ACID 트랜잭션과 Savepoint를 지원합니다.
 */
class ITransaction {
public:
    virtual ~ITransaction() = default;

    /** @brief 트랜잭션을 커밋합니다. */
    virtual Task<Result<void>> commit() = 0;

    /** @brief 트랜잭션을 롤백합니다. */
    virtual Task<Result<void>> rollback() = 0;

    /** @brief Savepoint를 생성합니다. */
    virtual Task<Result<void>> savepoint(std::string_view name) = 0;

    /** @brief Savepoint로 롤백합니다. */
    virtual Task<Result<void>> rollback_to(std::string_view name) = 0;

    /** @brief 이 트랜잭션 컨텍스트에서 쿼리를 직접 실행합니다. */
    virtual Task<Result<uint64_t>>
    execute(std::string_view sql, std::span<const Value> params = {}) = 0;
};

// ─── Connection ───────────────────────────────────────────────────────────────

/**
 * @brief 단일 DB 연결 인터페이스.
 *
 * ConnectionPool에서 획득하며, RAII 소멸 시 자동으로 풀에 반환됩니다.
 */
class IConnection {
public:
    virtual ~IConnection() = default;

    /** @brief 현재 연결 상태를 반환합니다. */
    [[nodiscard]] virtual ConnectionState state() const noexcept = 0;

    /**
     * @brief Prepared Statement를 생성합니다.
     * @param sql SQL 문자열. 파라미터는 `$1`, `$2` ... 또는 `?` 형식.
     */
    virtual Task<Result<std::unique_ptr<IStatement>>>
    prepare(std::string_view sql) = 0;

    /**
     * @brief 쿼리를 직접 실행합니다 (prepare 없이).
     * @param sql SQL 문자열.
     * @param params 바인딩 파라미터.
     */
    virtual Task<Result<std::unique_ptr<IResultSet>>>
    query(std::string_view sql, std::span<const Value> params = {}) = 0;

    /**
     * @brief 트랜잭션을 시작합니다.
     * @param level 격리 수준.
     */
    virtual Task<Result<std::unique_ptr<ITransaction>>>
    begin(IsolationLevel level = IsolationLevel::ReadCommitted) = 0;

    /** @brief 연결을 닫습니다 (풀 반환). */
    virtual Task<Result<void>> close() = 0;

    /** @brief 연결 헬스체크 (ping). O(1) 핸드오버 검증용. */
    [[nodiscard]] virtual Task<bool> ping() = 0;
};

// ─── ConnectionPool ───────────────────────────────────────────────────────────

/**
 * @brief DB 연결 풀 인터페이스.
 *
 * ## O(1) Connection Handover
 * 내부적으로 lock-free 링 버퍼를 사용하여 O(1) 연결 획득을 보장합니다.
 * 풀이 비어 있으면 `co_await`로 연결이 반환될 때까지 대기합니다.
 *
 * ## 풀 크기 정책
 * - `min_size`: 초기 워밍업 연결 수.
 * - `max_size`: 최대 동시 연결 수.
 * - 사용량이 `min_size` 이하로 떨어지면 초과 연결은 자동 해제.
 */
class IConnectionPool {
public:
    virtual ~IConnectionPool() = default;

    /**
     * @brief 연결을 획득합니다.
     *
     * 사용 가능한 연결이 있으면 즉시 반환 (O(1) lock-free).
     * 없으면 연결이 반환될 때까지 co_await 대기.
     *
     * @returns 연결 핸들 (소멸 시 자동 풀 반환).
     */
    virtual Task<Result<std::unique_ptr<IConnection>>> acquire() = 0;

    /** @brief 현재 활성 연결 수. */
    [[nodiscard]] virtual size_t active_count() const noexcept = 0;

    /** @brief 현재 유휴 연결 수. */
    [[nodiscard]] virtual size_t idle_count() const noexcept = 0;

    /** @brief 풀 최대 크기. */
    [[nodiscard]] virtual size_t max_size() const noexcept = 0;

    /** @brief 풀을 드레인(모든 연결 종료)합니다. */
    virtual Task<void> drain() = 0;

    /** @brief 연결을 풀에 반환합니다. PooledConnection 소멸자에서 호출됩니다. */
    virtual void return_connection(std::unique_ptr<IConnection>) noexcept {}
};

// ─── PoolConfig ───────────────────────────────────────────────────────────────

/** @brief ConnectionPool 설정 구조체. */
struct PoolConfig {
    size_t   min_size{2};          ///< 최소 유지 연결 수
    size_t   max_size{16};         ///< 최대 연결 수
    uint32_t connect_timeout_ms{5000};  ///< 연결 타임아웃 (ms)
    uint32_t idle_timeout_ms{60000};    ///< 유휴 연결 해제 시간 (ms)
    uint32_t query_timeout_ms{30000};   ///< 쿼리 타임아웃 (ms)
    bool     tls{false};                ///< TLS 활성화 여부
};

// ─── IDBDriver ────────────────────────────────────────────────────────────────

/**
 * @brief 통합 DB 드라이버 인터페이스.
 *
 * 구체적인 드라이버(PostgreSQL, Redis, ScyllaDB 등)는 이 인터페이스를 구현합니다.
 * `IDBDriver`는 `ConnectionPool`의 팩토리 역할을 합니다.
 */
class IDBDriver {
public:
    virtual ~IDBDriver() = default;

    /**
     * @brief 드라이버 식별자를 반환합니다.
     * @returns e.g. "postgresql", "redis", "mysql"
     */
    [[nodiscard]] virtual std::string_view driver_name() const noexcept = 0;

    /**
     * @brief DSN으로 ConnectionPool을 생성합니다.
     * @param dsn Data Source Name (e.g. "postgresql://user:pass@host:5432/db").
     * @param config 풀 설정.
     */
    virtual Task<Result<std::unique_ptr<IConnectionPool>>>
    pool(std::string_view dsn, PoolConfig config = {}) = 0;

    /**
     * @brief DSN이 이 드라이버로 처리 가능한지 확인합니다.
     * @param dsn Data Source Name.
     * @returns 처리 가능하면 true.
     */
    [[nodiscard]] virtual bool accepts(std::string_view dsn) const noexcept = 0;
};

// ─── DriverRegistry ───────────────────────────────────────────────────────────

/**
 * @brief 전역 드라이버 레지스트리.
 *
 * 드라이버는 `DriverRegistry::register_driver()`로 등록하며,
 * DSN 기반으로 자동으로 적합한 드라이버를 선택합니다.
 *
 * @note Thread-safe. 등록은 프로그램 시작 시 once로 수행하세요.
 */
class DriverRegistry {
public:
    /** @brief 최대 등록 드라이버 수 (동적 할당 없음). */
    static constexpr size_t kMaxDrivers = 8;

    /** @brief 드라이버를 등록합니다. */
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
     * @brief DSN에 맞는 드라이버를 반환합니다.
     * @param dsn Data Source Name.
     * @returns 드라이버 포인터 또는 nullptr.
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
