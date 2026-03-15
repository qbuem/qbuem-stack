# Architecture: Unified Database Driver Abstraction

This document defines the `qbuem-stack` approach to unified database access. The goal is a **Zero-dependency, Zero-allocation, and Fully Asynchronous** driver interface that supports SQL, NoSQL, and NewSQL backends.

---

## 1. Design Philosophy

`qbuem-stack` treats the database as an asynchronous stream of records or documents. The abstraction layer provides a consistent API for application logic while allowing implementation-specific optimizations (e.g., SIMD-accelerated protocol parsing).

### 1.1 Key Principles
- **Reactor Alignment**: All socket I/O MUST use the `IReactor` and `io_uring`.
- **Zero-copy Results**: Query results should be mapped directly from the network buffer when possible.
- **Provider Pattern**: The core stack provides the `IDBDriver` interface; specific protocol implementations (PostgreSQL, Redis, etc.) are provided as pluggable modules or implementation guides.

---

## 2. Core Interface Specification (The `db` Namespace)

### 2.1 Interface Hierarchy

| Component | Responsibility |
| :--- | :--- |
| **`Driver`** | Registry and Factory for connection pools. |
| **`ConnectionPool`** | Managing a pool of warm TCP/Unix-socket connections. |
| **`Connection`** | A single session. Handles `begin()`, `prepare()`, and `close()`. |
| **`Statement`** | A prepared query with bound parameters. |
| **`ResultSet`** | An asynchronous stream of `Row` objects. |

### 2.2 Transaction Management (ACID)

```cpp
// Transaction Flow Example
auto conn = co_await pool->acquire();
auto tx = co_await conn->begin(IsolationLevel::ReadCommitted);

try {
    co_await tx->execute("UPDATE accounts SET balance = balance - :amount", {{"amount", 100}});
    co_await tx->commit();
} catch (...) {
    co_await tx->rollback();
}
```

- **Nested Transactions**: Supports `Savepoint` API for complex business logic.
- **Two-Phase Commit (2PC)**: Interface hooks for distributed transactions (XA).

---

## 3. Implementation Guide: Building a Zero-dependency Driver

To implement a driver (e.g., PostgreSQL wire protocol) from scratch:

### 3.1 Network Layer
Use `io_uring` directly for non-blocking sends/receives.
```cpp
// Low-level buffer management via qbuem::BufferPool
auto buf = pool.acquire();
co_await reactor.recv(fd, buf);
```

### 3.2 Protocol Parsing
- **Stateless Parsing**: The protocol parser should be a state machine that doesn't allocate.
- **SIMD Acceleration**: Use bit-packing and SIMD instructions to parse large ResultSets (aligned with `io-deep-dive.md`).

### 3.3 Type Mapping
A unified `db::Value` variant (heap-free) is used for parameter binding and result extraction.

---

## 4. Multi-Model Support (SQL vs NoSQL)

The abstraction adapts based on the `Driver` type:

- **Relational**: Standard `execute/query` with `Row` mapping.
- **Key-Value (Redis)**: `get/set` methods or `execute_command` for raw RESP protocol.
- **Document (ScyllaDB/Mongo)**: `find/insert` using `std::string_view` for JSON-like blobs.

---

## 5. Performance Roadmap

| Feature | Target |
| :--- | :--- |
| **Connection Handover** | < 1us |
| **Result Parsing** | 100M+ rows/sec (SIMD) |
| **Memory Overhead** | 0 heap allocations per query (excluding Pool growth) |

---
