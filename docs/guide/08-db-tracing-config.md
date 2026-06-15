# DB Abstraction, Tracing & Config

This group covers three loosely related but commonly co-deployed concerns of a server built on qbuem-stack:

1. **`include/qbuem/db/`** — a zero-allocation SQL value type (`db::Value`), the **interface** layer for async drivers / connections / pools (`IDBDriver`, `IConnection`, `IConnectionPool`, …), a concrete mutex-guarded connection pool (`LockFreeConnectionPool`), and an in-process seqlock query-result cache (`SmartCache`).
2. **`include/qbuem/tracing/`** — W3C Trace Context propagation (`TraceContext`, `TraceId`, `SpanId`), RAII spans (`Span`, `SpanData`), pluggable exporters (`SpanExporter` and friends), samplers, a zero-allocation in-process lifecycle tracer (`LifecycleTracer` + SHM ring), and a trace-correlated async logger (`TraceLogger`).
3. **`include/qbuem/config/config_manager.hpp`** — a redaction-aware secret wrapper (`Secret<T>`), a heap-free tagged config value (`ConfigValue`), and a zero-allocation-at-access layered config loader (`ConfigManager`). Plus the `std::print` polyfill in **`include/qbuem/compat/print.hpp`**.

> **Honesty note on scope (read this first).**
> The DB layer is **interfaces** — `qbuem-stack` ships the abstract contracts (`IDBDriver`, `IConnection`, `IStatement`, `IResultSet`, `ITransaction`, `IConnectionPool`) plus a value type and a generic pool. **Concrete drivers (PostgreSQL, MySQL, Redis wire protocols) are application-level / out of scope for the core library.** You implement `IConnection` for your backend (or use `qbuem-db`). `SmartCache` is **in-process only** despite doc-comments that describe a cross-process / RDMA story — the `name` argument is accepted but unused and the slots live in ordinary process memory. The `LockFreeConnectionPool` is named "lock-free" for API stability but its acquire/release path is a `std::mutex` + free list (the cold path around a DB round-trip does not need a lock-free ring). All of these honest limitations are reflected below.

All APIs follow the four pillars: **errors are returned, never thrown** (`Result<T>` = `std::expected<T, std::error_code>`, async ops return `Task<Result<T>>`); hot-path types avoid heap allocation; views (`std::string_view`, `BufferView`) are zero-copy and the **caller owns the referenced buffer's lifetime**.

**Platforms:** all types here are portable across Linux x86_64, ARM64 boards (Jetson-class), and Mac aarch64. The only platform-specific code is the `__builtin_ia32_pause()` / ARM `yield` spin hint inside `SmartCache` (correctly `#ifdef`-gated per arch), and POSIX `clock_gettime` / `environ` usage.

---

## 1. `db::Value` — heap-free SQL value variant

**Header:** `<qbuem/db/value.hpp>` · **Namespace:** `qbuem::db`

### What it is / role
`db::Value` is a tagged-union variant (no heap, `sizeof <= 32` bytes) used for SQL parameter binding and result-column extraction. It can hold `Null`, `int64_t`, `double`, `bool`, a UTF-8 string (`std::string_view`, zero-copy), or binary blob (`BufferView` = `std::span<const uint8_t>`, zero-copy).

### When to use it
- Binding parameters to a prepared statement (`IStatement::execute(std::span<const Value>)`).
- Reading a column value out of a result row (`IRow::get(idx)` returns a `Value`).
- Any place you need a compact, allocation-free SQL scalar.

**When NOT to:** it is not an owning container — for strings/blobs it stores only a view. If the underlying buffer dies, the `Value` dangles. If you need ownership, copy the bytes into storage you control (e.g. an `Arena`) first.

### Type catalog

| `Value::Type` | Stored as | `is<T>()` / `get<T>()` `T` |
|---|---|---|
| `Null` | tag only | `Null` |
| `Int64` | `int64_t` | `int64_t` (also constructs from `int32_t`) |
| `Float64` | `double` | `double` (also constructs from `float`) |
| `Bool` | `bool` (in the int field) | `bool` |
| `Text` | `std::string_view` | `std::string_view` |
| `Blob` | `BufferView` | `BufferView` |

Key members: `Value()` (= NULL), constructors for each scalar, `type()`, `is_null()`, `template is<T>()`, `template get<T>()`, `operator==` / `operator!=`. There is a `null` constant: `inline constexpr Null null{}`.

### How to use it

```cpp
#include <qbuem/db/value.hpp>

using qbuem::db::Value;

Value v_null = qbuem::db::null;       // SQL NULL
Value v_int  = int64_t{42};
Value v_dbl  = 3.14;
Value v_str  = std::string_view{"hello"};   // zero-copy: "hello" must outlive v_str

// Always type-check before extracting — get<T>() on a wrong type is UB.
if (v_int.is<int64_t>()) {
    int64_t n = v_int.get<int64_t>();      // 42
}
if (v_int.is_null()) { /* ... */ }
```

### Binding parameters — `BoundParams<N>`

`BoundParams<N>` (default `N = 8`) is a stack array of `Value` plus a count. It lets you bind up to `N` params with no heap allocation and hand the driver a `std::span<const Value>` via `.span()`.

```cpp
qbuem::db::BoundParams<2> params;
params.bind(int64_t{42});
params.bind(std::string_view{"admin"});
// pass params.span() to IStatement::execute / IConnection::query
auto rs = co_await stmt->execute(params.span());
```

You can also brace-init it directly (as the driver header shows): `db::BoundParams<1>{{db::Value{int64_t{42}}}, 1}` (the trailing `1` is the `count`).

### Gotchas / constraints
- **Lifetime:** `Text`/`Blob` are non-owning views. `Value v = std::string_view{some_temp};` dangles once `some_temp` dies. This is the price of zero-copy.
- **`get<T>()` is unchecked** — UB if `T` does not match the stored type. Always guard with `is<T>()` or `type()`.
- `bool` is stored in the same integer field as `int64_t`; `is<int64_t>()` and `is<bool>()` are distinct (they check the tag), so a `bool` value will report `is<int64_t>() == false`.
- `BoundParams::bind()` silently drops a value once `count == N` — size `N` to your worst-case parameter count.

---

## 2. DB driver / connection / pool interfaces

**Header:** `<qbuem/db/driver.hpp>` · **Namespace:** `qbuem::db`

### What it is / role
The abstract contract for an async SQL backend, layered as:

```
IDBDriver         factory; selects pool by DSN
  └─ IConnectionPool   acquire() a connection
       └─ IConnection      prepare / query / begin / close / ping
            └─ IStatement       execute / execute_dml
                 └─ IResultSet        next() async row stream
                      └─ IRow              get(idx) / get(name) → db::Value
            └─ ITransaction     commit / rollback / savepoint / execute
```

Every fallible async method returns `Task<Result<...>>`; row iteration returns `Task<const IRow*>` (`nullptr` = end of stream).

### When to use it
- You are integrating a SQL/NoSQL backend and want a uniform async surface that plays with the reactor and `Task<T>`.
- You want connection pooling + prepared statements + transactions behind one interface so the rest of your code is backend-agnostic.

**When NOT to:** for an in-process key/value cache use `SmartCache`. For session storage there is a separate `core/session_store.hpp` (`ISessionStore`). And — importantly — **you must supply the concrete implementation** of these interfaces (or pull in `qbuem-db`). The core ships only the interfaces, the `Value` type, and the generic `LockFreeConnectionPool`.

### Interface catalog

| Interface | Key methods (real signatures) |
|---|---|
| `IRow` | `uint16_t column_count()`, `std::string_view column_name(uint16_t)`, `Value get(uint16_t)`, `Value get(std::string_view)` |
| `IResultSet` | `Task<const IRow*> next()`, `uint64_t affected_rows()`, `uint64_t last_insert_id()` |
| `IStatement` | `Task<Result<std::unique_ptr<IResultSet>>> execute(std::span<const Value> = {})`, `Task<Result<uint64_t>> execute_dml(std::span<const Value> = {})` |
| `ITransaction` | `Task<Result<void>> commit()`, `rollback()`, `savepoint(string_view)`, `rollback_to(string_view)`, `Task<Result<uint64_t>> execute(string_view sql, std::span<const Value> = {})` |
| `IConnection` | `ConnectionState state()`, `Task<Result<std::unique_ptr<IStatement>>> prepare(string_view)`, `Task<Result<std::unique_ptr<IResultSet>>> query(string_view, span = {})`, `Task<Result<std::unique_ptr<ITransaction>>> begin(IsolationLevel = ReadCommitted)`, `Task<Result<void>> close()`, `Task<bool> ping()` |
| `IConnectionPool` | `Task<Result<std::unique_ptr<IConnection>>> acquire()`, `size_t active_count()/idle_count()/max_size()`, `Task<void> drain()`, `void return_connection(std::unique_ptr<IConnection>)` |
| `IDBDriver` | `std::string_view driver_name()`, `Task<Result<std::unique_ptr<IConnectionPool>>> pool(string_view dsn, PoolConfig = {})`, `bool accepts(string_view dsn)` |

Enums: `IsolationLevel { ReadUncommitted, ReadCommitted, RepeatableRead, Serializable }`, `ConnectionState { Idle, Active, Transaction, Closed }`.

`PoolConfig` fields (with defaults): `min_size{2}`, `max_size{16}`, `connect_timeout_ms{5000}`, `idle_timeout_ms{60000}`, `query_timeout_ms{30000}`, `tls{false}`.

### How to use it (consumer side, against any concrete driver)

```cpp
#include <qbuem/db/driver.hpp>
using namespace qbuem::db;

Task<Result<std::string>> fetch_user_name(IConnection& conn, int64_t id) {
    auto stmt_r = co_await conn.prepare("SELECT id, name FROM users WHERE id = $1");
    if (!stmt_r) co_return std::unexpected(stmt_r.error());
    auto& stmt = *stmt_r;

    BoundParams<1> p; p.bind(id);
    auto rs_r = co_await stmt->execute(p.span());
    if (!rs_r) co_return std::unexpected(rs_r.error());
    auto& rs = *rs_r;

    if (const IRow* row = co_await rs->next()) {
        Value name = row->get("name");                 // by column name
        if (name.is<std::string_view>())
            co_return std::string{name.get<std::string_view>()};
    }
    co_return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
}
```

Transaction shape:

```cpp
auto tx_r = co_await conn.begin(IsolationLevel::Serializable);
if (!tx_r) co_return std::unexpected(tx_r.error());
auto& tx = *tx_r;
auto upd = co_await tx->execute("UPDATE accounts SET bal = bal - $1 WHERE id = $2",
                               /* span<const Value> */ params.span());
if (!upd) { co_await tx->rollback(); co_return std::unexpected(upd.error()); }
co_await tx->commit();
```

### `DriverRegistry` — DSN → driver lookup
A lock-free, fixed-capacity (`kMaxDrivers = 8`) global registry. Register concrete drivers at startup; resolve by DSN at runtime:

```cpp
DriverRegistry::register_driver(&my_pg_driver);     // once at startup
if (IDBDriver* d = DriverRegistry::find("postgresql://localhost/db")) {
    auto pool_r = co_await d->pool("postgresql://localhost/db", PoolConfig{.max_size = 32});
}
```

### Gotchas / constraints
- **`IRow*` lifetime is owned by the `IResultSet`.** Don't keep an `IRow*` past the next `next()` call — many implementations reuse one row object (the `db_session` example does exactly that).
- `prepare`/`query` SQL placeholders are driver-dependent (`$1`/`$2` Postgres-style or `?`). The interface does not enforce a dialect.
- Returned `Value`s for `Text`/`Blob` columns are views into the result-set buffer; copy out before the result set is destroyed.
- See the runnable mock implementation in `examples/09-database/db_session/` for a complete `IConnection`/`IStatement`/`IResultSet` that you can model your driver on.

---

## 3. `LockFreeConnectionPool` & `PooledConnection`

**Header:** `<qbuem/db/connection_pool.hpp>` · **Namespace:** `qbuem::db`

### What it is / role
A concrete `IConnectionPool` that takes a **connection factory** (`std::function<Task<Result<std::unique_ptr<IConnection>>>()>`) and hands out connections, returning them on RAII destruction. Despite the name, the current acquire/release path is a `std::mutex` + `std::vector` free list (a DB round-trip is a cold path, so a lock-free ring would buy nothing — the name is kept for API stability).

### When to use it
- You have a concrete `IConnection` factory and want pooling, warmup, backpressure, and RAII return without writing pool logic yourself.

**When NOT to:** if your backend already returns a pool from `IDBDriver::pool()`, use that. This class is the *generic* pool for when you only have a factory.

### Key API

| Member | Signature / behaviour |
|---|---|
| ctor | `LockFreeConnectionPool(ConnFactory factory, PoolConfig config = {})` |
| `warmup()` | `Task<Result<void>>` — pre-creates `min_size` connections to cut first-request latency |
| `acquire()` | `Task<Result<std::unique_ptr<IConnection>>>` — pops an idle conn, else creates a new one if under `max_size`, else returns `errc::resource_unavailable_try_again` |
| `return_connection(...)` | `void` — push a connection back (called by `PooledConnection`'s destructor) |
| `drain()` | `Task<void>` — set draining + clear idle connections |
| `active_count()` / `idle_count()` / `max_size()` | `size_t` counters |

`PooledConnection` is the RAII guard. Use `PooledConnection::acquire(pool)` (returns `Task<Result<PooledConnection>>`); on scope exit the connection is returned to the pool automatically. Access via `guard->method(...)` or `guard.get()`; check `guard.valid()`.

### How to use it

```cpp
#include <qbuem/db/connection_pool.hpp>
using namespace qbuem::db;

LockFreeConnectionPool pool(
    []() -> Task<Result<std::unique_ptr<IConnection>>> {
        co_return std::unique_ptr<IConnection>(std::make_unique<MyConnection>());
    },
    PoolConfig{ .min_size = 2, .max_size = 5, .idle_timeout_ms = 5000 });

co_await pool.warmup();                       // optional: warm min_size conns

{
    auto guard_r = co_await PooledConnection::acquire(pool);
    if (!guard_r) co_return;                  // pool exhausted / factory failed
    PooledConnection guard(std::move(*guard_r));

    auto rs_r = co_await guard->query("SELECT id, name FROM users LIMIT 2");
    if (rs_r) {
        auto& rs = *rs_r;
        while (const IRow* row = co_await rs->next()) { /* use row */ }
    }
}   // <- guard destructor calls pool.return_connection(...)
```

Full runnable demo: `examples/09-database/db_session/`.

### Gotchas / constraints
- **The shipped inline implementation is intentionally simplified.** It uses an `idle_conns_` vector guarded by `waiter_mutex_`; the elaborate `Slot` / `FreeStack` / waiter-queue machinery declared in the header is present for shape but the inline acquire/return path does not exercise the lock-free stack or the waiter wakeup (those are stubbed inline). The slot-based idle-timeout cleanup is also a stub. Treat this as a correct, demo-grade pool — for production-grade reconnection, idle eviction, and waiter fairness you will implement those paths (or use `qbuem-db`).
- `acquire()` does **not** block-and-wait for a returned connection in the inline version — when at `max_size` and none idle, it returns `resource_unavailable_try_again`. Handle that error (retry/backoff) at the call site.
- `acquire()` returns an error after `drain()` (`errc::operation_canceled`).
- The factory runs on the calling coroutine's reactor — keep it non-blocking and `co_await`-friendly.

---

## 4. `SmartCache<V, Capacity, KeyLen>` — in-process seqlock cache

**Header:** `<qbuem/db/smart_cache.hpp>` · **Namespace:** `qbuem` (note: not `qbuem::db`)

### What it is / role
A thread-safe, in-process, fixed-capacity open-addressing cache with **wait-free reads** via a per-slot seqlock (generation counter). Writers bump the generation to odd (dirty), write, then to even (committed); readers snapshot the value and re-check the generation, retrying if it changed. No mutex on the hot path. Optional per-entry TTL.

### When to use it
- Hot read-mostly data shared between **threads in one process**: cached DB query results, order books, computed lookups — anything where you want lock-free reads under a concurrent writer.
- You can tolerate a stale-on-miss semantic (cache miss → query the source → `put`).

**When NOT to:**
- **Cross-process sharing — it does NOT do this.** The doc-comment and the `name` ctor argument describe an SHM/RDMA cross-process design that is **not implemented**: `name` is stored and ignored, and `slots_` is an ordinary `std::array<Slot, Capacity>` in process memory. For genuine cross-process sharing, compose with `qbuem/shm/shm_channel.hpp` yourself.
- Non-trivially-copyable values — the `requires std::is_trivially_copyable_v<V>` constraint forbids them (values are byte-copied under the seqlock).
- Very large value types — each slot embeds a `V` plus padding; `Capacity` slots are allocated up front.

### Type catalog

| Type | Role |
|---|---|
| `SmartCache<V, Capacity = 1024, KeyLen = 64>` | the cache; `requires is_trivially_copyable_v<V>` |
| `CacheSlot<V, KeyLen = 64>` | one cache-line-padded slot (`generation`, `key`, `value`, `expire_ns`); exposed as `SmartCache<...>::Slot` |
| `SmartCacheStats` | atomics: `hits`, `misses`, `evictions`, `invalidations`, `writes`, `seqlock_retries` |

### Key API

| Member | Behaviour |
|---|---|
| `SmartCache(std::string_view name = "")` | construct; `name` accepted for API symmetry but **unused** (no SHM) |
| `void put(string_view key, const V& value, uint64_t ttl_ns = 0)` | seqlock write; `ttl_ns = 0` means no expiry |
| `std::optional<V> get(string_view key)` | wait-free read; returns a **copy** of `V`, or `nullopt` on miss/expiry |
| `bool invalidate(string_view key)` | evict one entry |
| `void invalidate_all()` | flush all |
| `const SmartCacheStats& stats()` | live counters |
| `double hit_rate()` | `hits / (hits + misses)` |
| `size_t size()` | occupied-slot count (scans the table) |

### How to use it

```cpp
#include <qbuem/db/smart_cache.hpp>

struct OrderBook {            // must be trivially copyable
    int64_t bid_ns{0};
    int64_t ask_ns{0};
};
static_assert(std::is_trivially_copyable_v<OrderBook>);

qbuem::SmartCache<OrderBook, 1024> cache;     // 1024 slots, in-process

cache.put("SAMSUNG", OrderBook{.bid_ns = 71'200, .ask_ns = 71'300});

if (auto v = cache.get("SAMSUNG")) {          // hit → std::optional<OrderBook> by copy
    process(*v);
} else {
    // miss → fetch from source, then populate (optionally with a 5s TTL)
    OrderBook fresh = load_from_db("SAMSUNG");
    cache.put("SAMSUNG", fresh, /*ttl_ns=*/5'000'000'000ULL);
}

auto& s = cache.stats();
// s.hits, s.misses, s.writes, s.seqlock_retries  ...  cache.hit_rate()
```

Full runnable demo (including a concurrent-writer/two-reader torn-read test that the seqlock keeps at 0): `examples/09-database/smart_cache/`.

### Gotchas / constraints
- **In-process only** — see "When NOT to" above. Do not size your architecture around the doc-comment's cross-process claims.
- `get()` returns a **copy**, not a reference — safe under concurrency, but for big `V` the copy is the read cost.
- Keys are NUL-terminated, truncated to `KeyLen - 1` chars (default 63). `key_matches` compares up to `KeyLen` bytes.
- **Eviction is "oldest generation" under full open-addressing probing**, not true LRU on read — a hot key that is never re-`put` can still be evicted if the table fills. Size `Capacity` (ideally a prime) above your working set.
- The TTL clock is `CLOCK_MONOTONIC`; expiry is checked lazily on `get()` (an expired entry is invalidated and reported as a miss).
- The seqlock read retries up to 8 times under a concurrent writer (with a short arch-specific `pause`/`yield` spin); persistent inconsistency falls through to a miss. This is portable across x86_64 and ARM64.

---

## 5. Tracing — W3C Trace Context

**Header:** `<qbuem/tracing/trace_context.hpp>` · **Namespace:** `qbuem::tracing`

### What it is / role
W3C Trace Context Level 1 identifiers and the `traceparent` header codec. `TraceId` (128-bit), `SpanId` (64-bit), and `TraceContext` (trace id + parent span id + flags) with `generate()`, `child_span()`, `to_traceparent()`, and `from_traceparent()`.

### When to use it
- Propagating a trace across service boundaries: parse `traceparent` on inbound HTTP, attach a child `traceparent` on outbound calls.
- Generating root contexts for new requests.

### How to use it

```cpp
#include <qbuem/tracing/trace_context.hpp>
using namespace qbuem::tracing;

// Inbound: parse the header (returns Result<TraceContext>)
TraceContext ctx;
if (auto r = TraceContext::from_traceparent(inbound_header)) {
    ctx = *r;                          // continue the existing trace
} else {
    ctx = TraceContext::generate();    // start a fresh root trace
}

// Outbound: propagate a child span
TraceContext child = ctx.child_span();
outbound_req.set_header("traceparent", child.to_traceparent());
// e.g. "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"

bool is_sampled = ctx.is_sampled();    // flags bit 0
```

`TraceId::generate()` / `SpanId::generate()` use `qbuem::random_bytes()` (CSPRNG). `is_valid()` is false iff all bytes are zero (per spec). `to_chars(buf, n)` writes lowercase hex (needs `n >= 33` for trace, `n >= 17` for span).

### Gotchas / constraints
- `from_traceparent` is strict: only version `00`, exact 55-char layout, valid (non-zero) ids — otherwise `std::unexpected(errc::invalid_argument)`. Always check the `Result`.
- `TraceContext::parent_span_id` doubles as "this span's id" when you call `child_span()` (the new child's parent is set to a freshly generated id — read the field doc carefully; the field name is `parent_span_id` for header-serialization symmetry).

---

## 6. Tracing — Spans (`Span`, `SpanData`)

**Header:** `<qbuem/tracing/span.hpp>` · **Namespace:** `qbuem::tracing`

### What it is / role
`SpanData` holds span metadata (ids, name/pipeline/action, start/end times, status, up to `kMaxAttributes = 16` key/value attributes). `Span` is a **move-only RAII wrapper**: on destruction it stamps `end_time` and calls `tracer_->export_span(data_)` (no-op if the tracer pointer is null).

### When to use it
- Timing a unit of work and exporting it automatically at scope exit.
- Attaching structured attributes (`order_id`, `user_id`, …) to a trace.

**When NOT to:** for sub-10ns hot-path tracing into an SHM ring (no `std::string` allocations, no exporter call per span), use `LifecycleTracer` (§9) instead — `SpanData` uses `std::string` members and is meant for the cold/normal path.

### How to use it
You normally get a `Span` from a tracer rather than constructing one (see §7):

```cpp
{
    auto span = tracer.start_span("process_order", "order-pipeline", "validate");
    span.set_attribute("order_id", "ORD-12345");
    span.set_attribute("user_id",  "usr-42");
    // ... do work ...
    span.set_status(SpanStatus::Ok);     // or Error with a message
}   // <- destructor sets end_time and exports
```

`SpanStatus { Ok, Error, Unset }`. `set_status(SpanStatus::Error, "insufficient_funds")` records the message. `data()` returns the const `SpanData&`.

### Gotchas / constraints
- **Move-only.** Moving a `Span` transfers the export responsibility; the moved-from span will not double-export (`ended_` is set).
- Beyond 16 attributes, `set_attribute` is silently ignored; duplicate keys overwrite.
- `SpanData` contains `std::string` members — creating a span allocates. Acceptable on normal paths; for the hot path use `LifecycleTracer`.

---

## 7. Tracing — Exporters & Tracers

**Header:** `<qbuem/tracing/exporter.hpp>` · **Namespace:** `qbuem::tracing`

### What it is / role
The export pipeline and the tracer that drives it:

| Type | Role |
|---|---|
| `SpanExporter` | pure-virtual sink: `void export_span(const SpanData&)`, `flush()`, `shutdown()` |
| `NoopSpanExporter` | discards everything (tests / tracing-off) |
| `LoggingSpanExporter` | prints a human-readable multi-line span to **stderr** (thread-safe) |
| `Tracer` | base tracer: `start_span(...)` builds a `Span`, `export_span(...)` forwards to the exporter; default exporter is Noop |
| `PipelineTracer` | global-singleton wrapper around `Tracer` with `global()` / `set_global_tracer()` / `set_exporter()` |
| `IMetricsExporter` | abstract Prometheus push: `gauge` / `counter` / `histogram` / `flush` |
| `PrometheusTextExporter` | in-memory Prometheus text-format generator; `export_text()` drains accumulated metrics |
| `qbuem::TraceContextSlot` | wrapper to store a `TraceContext` in a `qbuem::Context` |

> **OTLP honesty:** there is **no concrete `OtlpSpanExporter` symbol** in this header. "OTLP" appears only as a *conceptual destination* in the `LifecycleTracer` / `TraceLogger` doc-comments (a sidecar reads the SHM ring and pushes OTLP off the hot path). To emit OTLP you implement `SpanExporter` (or `ILogSink`) yourself. The concrete exporters that ship are `NoopSpanExporter` and `LoggingSpanExporter`, plus the Prometheus text exporter for metrics.

### When to use it
- `PipelineTracer` as the process-wide tracer; swap exporters per environment (Noop in tests, Logging in dev, your custom exporter in prod).
- `PrometheusTextExporter` to expose `/metrics` text without pulling in a metrics library (zero-dependency).

### How to use it

```cpp
#include <qbuem/tracing/exporter.hpp>
using namespace qbuem::tracing;

// 1) Configure the global tracer once at startup.
auto tracer = std::make_unique<PipelineTracer>();
tracer->set_exporter(std::make_shared<LoggingSpanExporter>());   // or your own
PipelineTracer::set_global_tracer(std::move(tracer));

// 2) Create spans anywhere.
auto& pt = PipelineTracer::global();
{
    auto span = pt.start_span("charge_payment", "order-pipeline", "payment");
    span.set_attribute("order_id", "ORD-12345");
    span.set_status(SpanStatus::Ok);
}   // exported here
```

Custom exporter (the idiomatic way to reach OTLP / Jaeger / a file):

```cpp
class MyExporter final : public SpanExporter {
public:
    void export_span(const SpanData& s) override {
        // s.name, s.action_name, s.status, s.start_time/end_time, s.attributes[...]
        // serialize + ship (thread-safe: may be called from many threads)
    }
    void shutdown() override { /* flush/close */ }
};
```

Prometheus metrics:

```cpp
PrometheusTextExporter metrics;
metrics.gauge("queue_depth", 42.0, R"(job="worker")");
metrics.counter("messages_processed", 1.0, R"(env="prod")");
metrics.histogram("request_latency_ms", 12.3);
std::string body = metrics.export_text();   // Prometheus exposition format; buffer is drained
```

### Gotchas / constraints
- **`PipelineTracer` has no sampler hook.** Despite the example in `sampler.hpp` showing `PipelineTracer::global().set_sampler(...)`, **that method does not exist** on `PipelineTracer`. Samplers (§8) are standalone — call `should_sample()` yourself and skip `start_span` on `DROP`.
- `set_global_tracer` should be called once at startup; it `delete`s the previous global. `global()` is safe to call before any set (returns a Noop-backed default).
- `LoggingSpanExporter` writes to **stderr** and formats with `std::format` inside `export_span` — fine for the cold export path, not the request hot path.
- `export_span` may run on any thread; custom exporters must synchronize internally (the contract states this; `export_span` must not be called after `shutdown()`).
- `PrometheusTextExporter::export_text()` **clears** the accumulated counters/histograms/gauges after returning — call it once per scrape.

---

## 8. Tracing — Samplers

**Header:** `<qbuem/tracing/sampler.hpp>` · **Namespace:** `qbuem::tracing`

### What it is / role
Pluggable sampling-decision objects. `Sampler::should_sample(const SamplingContext&)` returns `SamplingDecision { DROP, RECORD_AND_SAMPLE }`.

| Sampler | Behaviour |
|---|---|
| `AlwaysSampler` | always `RECORD_AND_SAMPLE` |
| `NeverSampler` | always `DROP` (zero-overhead off-switch) |
| `ProbabilitySampler(double rate)` | sample with probability `rate ∈ [0,1]` (clamped); per-thread RNG, lock-free |
| `RateLimitingSampler(double max_per_second)` | token bucket; ≤ `max_per_second` samples/s; uses a mutex |
| `ParentBasedSampler(shared_ptr<Sampler> root = nullptr)` | follow parent's sampled flag; delegate to `root` sampler for root spans (defaults to `AlwaysSampler`) |

`SamplingContext { std::string_view pipeline_name, action_name, span_name; const TraceContext* parent; }`.

### When to use it
- Cost-control in production: e.g. `ProbabilitySampler(0.01)` or `RateLimitingSampler(100)`.
- Honor upstream sampling decisions: `ParentBasedSampler`.

### How to use it
Because `PipelineTracer` does not auto-apply a sampler, gate span creation manually:

```cpp
#include <qbuem/tracing/sampler.hpp>
using namespace qbuem::tracing;

ProbabilitySampler sampler{0.1};   // 10%

SamplingContext sctx{"order-pipeline", "validate", "process_order", /*parent*/ nullptr};
if (sampler.should_sample(sctx) == SamplingDecision::RECORD_AND_SAMPLE) {
    auto span = PipelineTracer::global().start_span("process_order", "order-pipeline", "validate");
    span.set_status(SpanStatus::Ok);
}
```

`ParentBasedSampler` reads `ctx.parent->flags & 0x01`:

```cpp
ParentBasedSampler parent{std::make_shared<ProbabilitySampler>(0.1)};
TraceContext upstream = /* parsed from traceparent */;
SamplingContext pctx{"pipe", "action", "span", &upstream};
auto decision = parent.should_sample(pctx);
```

Full demo: `examples/08-observability/tracing/`.

### Gotchas / constraints
- **No tracer integration** — you must call `should_sample` and branch yourself. The "set it on the tracer" pattern in the header comment is aspirational.
- `RateLimitingSampler` takes a mutex per decision — fine at modest rates, avoid on a tight inner loop. `ProbabilitySampler` is lock-free (thread-local RNG) and preferable for hot paths.
- `ParentBasedSampler` only inspects the parent flag bit; it does not re-evaluate for child spans whose parent is sampled.

---

## 9. Tracing — `LifecycleTracer` (zero-allocation, SHM ring)

**Header:** `<qbuem/tracing/lifecycle_tracer.hpp>` · **Namespace:** `qbuem::tracing`

### What it is / role
A zero-allocation tracer that writes fixed-size `SpanRecord`s (exactly 128 bytes, trivially copyable) into a lock-free MPSC ring (`ShmSpanRing<Capacity>`) instead of calling an exporter per span. Spans are RAII (`ActiveSpan`); a separate consumer (sidecar / drain loop) reads the ring and ships records off the hot path. The design *intends* an SHM region read by a collector that pushes OTLP — but the shipped tracer's ring lives in the tracer object (process memory), and you drain it yourself via `drain()`.

### When to use it
- The request hot path where `Span`'s `std::string` allocations and per-span exporter call are too expensive — target costs are < 20 ns to start a lifecycle, < 10 ns per span.
- High-throughput ingress→pipeline→DB→response tracing where you can afford to drop spans under burst (ring-full → dropped, counted).

**When NOT to:** if you want rich string attributes and human-readable export with no throughput pressure, use `Span` + `PipelineTracer` (§6–7). `ActiveSpan` attributes are minimal (a single 15-char field reused as the "service"/attribute slot).

### Key API

| Member | Behaviour |
|---|---|
| `LifecycleTracer<RingCapacity = 65536>(std::string_view service_name)` | construct with a service name (truncated to 15 chars) |
| `ActiveSpan start_lifecycle(string_view op)` | begin a root span for a new request |
| `ActiveSpan start_span(string_view op, const TraceContext& parent)` | begin a child span |
| `ActiveSpan::end(SpanStatus = Ok)` | finalize + push to ring (auto-called by destructor if not ended) |
| `ActiveSpan::context()` | the `TraceContext` to propagate / parent the next span |
| `ActiveSpan::set_attribute(key, val)` | stores `val` (≤15 chars) in the record's service field |
| `template<Fn> void drain(Fn&& fn)` | consume buffered `SpanRecord`s: `fn(const SpanRecord&) -> bool` (false stops) |
| `buffered_spans()` / `total_spans()` / `dropped_spans()` | counters |

### How to use it

```cpp
#include <qbuem/tracing/lifecycle_tracer.hpp>
using namespace qbuem::tracing;

LifecycleTracer<65536> tracer("order-service");

// Hot path: request entry
auto root = tracer.start_lifecycle("process_order");
auto ctx  = root.context();
{
    auto child = tracer.start_span("validate", ctx);
    // ... work ...
    child.end(SpanStatus::Ok);
}
root.end(SpanStatus::Ok);

// Propagate over the wire
outbound.set_header("traceparent", ctx.to_traceparent());

// Drain loop (collector side / background): export off the hot path
tracer.drain([](const SpanRecord& rec) {
    // rec.trace_id_hi/lo, rec.span_id, rec.start_ns/end_ns, rec.status, rec.name.data()
    ship_to_backend(rec);
    return true;       // keep draining
});
```

### Gotchas / constraints
- **The shipped `make_span` uses a single per-instance `scratch_record_`** (the header explicitly notes the production version would use a thread-local pool). As written, this is **not safe for concurrent span creation on one `LifecycleTracer` instance** — concurrent `start_*` calls race on the scratch record. Use one tracer per thread, or treat the multi-thread MPSC ring as the safe surface and serialize span creation, until you swap in a thread-local pool. (The ring's `try_push`/`try_pop` themselves are correct MPSC.)
- Ring capacity must be a power of two (static-asserted). Full ring → span dropped, `dropped_spans()` incremented — size for your burst.
- `ActiveSpan` is move-only; on destruction it auto-`end(Ok)` if you didn't call `end`.
- Times are `CLOCK_MONOTONIC` nanoseconds; ids are a process-local atomic counter, not W3C-random (fine for in-process correlation, map to real ids at export if needed).

---

## 10. Tracing — `TraceLogger` (trace-correlated async log)

**Header:** `<qbuem/tracing/trace_logger.hpp>` · **Namespace:** `qbuem::tracing`

### What it is / role
A trace-aware async logger: every log call embeds the W3C `TraceContext` (trace_id low-64, span_id) into a fixed-size `TraceLogRecord` (trivially copyable, `kMsgLen = 256` message bytes) pushed onto a lock-free MPSC ring. A background `std::jthread` (started by `start()`) drains the ring and writes via an `ILogSink`. Hot-path cost is a CAS + memcpy — formatting and I/O happen on the flush thread.

### When to use it
- You want log lines correlated to trace spans (so a log viewer can place them on the request timeline) without paying formatting/I/O cost on the request thread.

**When NOT to:** for one-off diagnostics or startup logs, plain `std::print`/`std::println` (via the compat shim, §12) is simpler. For metrics use `PrometheusTextExporter` (§7).

### Key API

| Type / member | Role |
|---|---|
| `LogLevel { Trace, Debug, Info, Warn, Error, Fatal }`; `level_str(l)` | severity |
| `ILogSink` | `void write(const TraceLogRecord&)`, `void flush()` — implement for file/stderr/OTLP |
| `StderrLogSink` | default sink; prints ISO-8601 ts + level + traceparent + service + msg |
| `TraceLogger<Cap = 8192>(string_view service, ILogSink* = nullptr, LogLevel min = Info)` | construct |
| `start()` / `stop()` | run / stop the flush `jthread` |
| `log(level, ctx, msg)`, `info/warn/error/debug(ctx, msg)` | hot-path: pre-formatted `string_view`, no allocation |
| `logf(level, ctx, fmt, args...)`, `infof(...)` | formats with `std::format` **on the caller thread** (allocates — non-hot-path) |
| `dropped()` / `ring_size()` | counters |

### How to use it

```cpp
#include <qbuem/tracing/trace_logger.hpp>
using namespace qbuem::tracing;

TraceLogger<8192> logger("order-service");   // default StderrLogSink, min level Info
logger.start();                               // launch flush thread

LifecycleTracer<> tracer("order-service");
auto span = tracer.start_lifecycle("process_order");
TraceContext ctx = span.context();

logger.info(ctx, "received order");                          // hot path: no alloc
logger.infof(ctx, "validated order_id={}", order_id);        // formats on caller thread

logger.stop();   // drains remaining records, then joins
```

Custom sink to reach a file or OTLP backend:

```cpp
class FileLogSink final : public ILogSink {
public:
    void write(const TraceLogRecord& rec) noexcept override {
        // rec.timestamp_ns, rec.trace_id, rec.span_id, rec.level, rec.service.data(), rec.msg.data()
    }
    void flush() noexcept override { /* fsync */ }
};
```

### Gotchas / constraints
- The hot-path `info/warn/...(ctx, std::string_view)` overloads do **not** format — pass an already-built message. Use `infof` only off the hot path (`std::format` allocates; the header says so).
- Ring full → record dropped, `dropped()` incremented. `Cap` must be a power of two.
- `TraceLogger` is non-copyable in effect (owns a `jthread`); construct one per process and share by reference.
- Only the **low 64 bits** of the trace id and the span id are stored (the record is 64-bit-id based); the stderr sink zero-pads to a 128-bit-looking traceparent. Fine for correlation, lossy vs full 128-bit ids.
- `start()` is idempotent; always call `stop()` (or rely on `jthread` RAII) so buffered records flush.

---

## 11. Config — `Secret<T>`, `ConfigValue`, `ConfigManager`

**Header:** `<qbuem/config/config_manager.hpp>` · **Namespace:** `qbuem::config`

### `Secret<T>` — redaction-aware secret wrapper

**What it is:** a move-only wrapper around a string-like `T` (needs `.data()` and `.size()`) that (a) cannot be copied, (b) formats/prints as `[REDACTED]` (a `std::formatter` specialization), and (c) zeroes its bytes on destruction/move via a `volatile` clear (defeats dead-store elimination). `reveal()` is the explicit, auditable opt-in to read the value.

**When to use it:** API keys, DB passwords, tokens — anything you must not accidentally log. **When NOT to:** non-sensitive config (use `ConfigValue`/`get_or`).

```cpp
#include <qbuem/config/config_manager.hpp>
using qbuem::config::Secret;

Secret<std::string> key{"s3cr3t-api-key"};
std::println("{}", key);          // prints: [REDACTED]
make_api_call(key.reveal());      // explicit, auditable read
if (key.has_value()) { /* ... */ }
```

**Gotchas:** non-copyable (pass by move/reference). `reveal()` returns a `const T&` — do not store it past the call; copying the revealed value defeats the protection. The `volatile` wipe assumes contiguous `.data()` storage.

### `ConfigValue` — heap-free tagged config value

A union of `Int (int64_t)`, `Double`, `Bool`, `String` (inline ≤255 bytes, longer is truncated), plus `Unset`. Construct explicitly: `ConfigValue{int64_t{8080}}`, `ConfigValue{3.14}`, `ConfigValue{true}`, `ConfigValue{std::string_view{"text"}}`. Read with `as_int()/as_double()/as_bool()/as_string()` (wrong type → safe default `0/0.0/false/""`), or `type()`/`is_set()`.

**Gotcha:** strings over 255 bytes are silently truncated; `ConfigValue` is for short config scalars, not blobs.

### `ConfigManager` — layered, zero-alloc-at-access loader

**What it is:** a hierarchical config store backed by a fixed-capacity (`ConfigTable<512>`) open-addressing flat table. Load order (low → high priority): `set_default()` → `load_file()` → `load_env()` → `set()`. After init, reads (`get_or`, `get_secret`, `contains`) are O(1), lock-free, and heap-free.

**When to use it:** process configuration with env-var and file overrides, where read-path latency matters and you want no allocation per lookup. **When NOT to:** dynamic, frequently-mutated config — writes are init-time only and **not thread-safe**.

```cpp
namespace cfg = qbuem::config;
cfg::ConfigManager cm;

cm.set_default("server.port",    cfg::ConfigValue{int64_t{8080}});
cm.set_default("server.workers", cfg::ConfigValue{int64_t{4}});

if (auto r = cm.load_env("QBUEM_"); !r)       // QBUEM_SERVER_PORT=9090 → server.port
    std::println("env load failed: {}", r.error().message());
if (auto r = cm.load_file("config.ini"); !r)  // KEY=VALUE; '#' comments
    std::println("file load failed: {}", r.error().message());

int64_t port = cm.get_or("server.port", int64_t{8080});   // O(1), lock-free
if (auto api_key = cm.get_secret("external.api_key"))      // Result<Secret<std::string>>
    connect(api_key->reveal());
```

**Method catalog:** `set_default(key, ConfigValue)` (insert-if-absent), `set(key, ConfigValue)` (overwrite), `load_env(prefix) -> Result<void>` (strips prefix, lowercases, `_`→`.`), `load_file(path) -> Result<void>` (`KEY=VALUE`), `template<T> get_or(key, default)` (T ∈ `int64_t/double/bool/std::string_view`, with string→T coercion), `get_secret(key) -> Result<Secret<std::string>>`, `contains(key)`, `erase(key)`.

**Gotchas / constraints:**
- **`load_*` / `set*` are NOT thread-safe — call only during init**, before serving requests. After init, the read accessors are safe to call concurrently.
- The table holds `ConfigTable<512>` slots; exceeding ~512 keys fails inserts (`insert_or_assign` returns false internally). Keys hash via FNV-1a; collisions probe linearly.
- Env keys are normalized (lowercased, `_`→`.`); file keys are lowercased. Lookups are **case-sensitive** against the normalized form, so query with the dotted lowercase key.
- Env/file values are stored as **strings**; `get_or<int64_t>`/`<double>`/`<bool>` coerce on read (`from_chars` / `strtod` / `"true"/"1"/"yes"/"on"`). A non-numeric string with `get_or<int64_t>` falls back to your default.
- `get_secret` requires the stored value to be a `String` type (env/file values are); a numeric `ConfigValue` returns `errc::invalid_argument`.

---

## 12. `std::print` polyfill — `<qbuem/compat/print.hpp>`

### What it is / role
A compatibility shim that provides `std::print` / `std::println` on toolchains that implement most of C++23 but ship no `<print>` (GCC 13, Clang 17). When `<print>` is available (GCC 14+, Clang 18+), this header simply includes it and is a pass-through.

### When to use it
- **Always**, instead of `#include <print>`, anywhere in qbuem-stack code that prints — this is what keeps the library buildable across the supported compiler matrix (Linux GCC/Clang, ARM64 boards, Mac aarch64/Apple Clang). Pillar 6 (M3) mandates `std::print`/`std::println` over `printf`/`std::cout`.

### How to use it

```cpp
#include <qbuem/compat/print.hpp>

std::println("server on port {}", port);
std::print("no newline {}", x);
std::println(stderr, "error: {}", err.message());   // FILE* overloads
std::println();                                       // bare newline
```

### Gotchas / constraints
- The shim writes via `std::format` + `std::fwrite` — it is **not** locale/encoding-aware the way a full `<print>` is, but matches the common format-and-write behavior used across the library.
- It defines names in `namespace std` (an intentional C++23 polyfill, flagged with a `NOLINT`). This is harmless when the native header is absent and inert when it is present; do not add your own `std` overloads alongside it.
- Because it shadows `<print>` only when missing, code written against it compiles unchanged once you upgrade the compiler.
