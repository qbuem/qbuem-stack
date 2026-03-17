# db_session

**Category:** Database
**File:** `db_session_example.cpp`
**Complexity:** Intermediate
**Dependencies:** `qbuem-json`

## Overview

Demonstrates `LockFreeConnectionPool` (zero-allocation database connection pool) and `InMemorySessionStore` (fast in-memory HTTP session storage). Both are designed for high-concurrency web application servers.

## Scenario

A REST API server handles thousands of concurrent requests. Each request needs a database connection (from a pool, not per-request allocation) and an HTTP session lookup (O(1) hash map, no lock contention).

## Architecture Diagram

```
  Request arrives
       │
       ├─── Session lookup ────────────────────────────────
       │    SessionStore::get(session_id)
       │    └─ InMemorySessionStore (lock-free hash map)
       │       SessionData { user_id, roles, expires_at }
       │
       └─── DB query ──────────────────────────────────────
            LockFreeConnectionPool::acquire()
            ├─ pops connection from free-list (O(1))
            │  DbConnection { execute_query() }
            └─ returns connection via RAII guard
                 │  query completes
                 ▼
            LockFreeConnectionPool::release(conn)
            └─ pushes back to free-list (O(1))

  LockFreeConnectionPool
  ──────────────────────────────────────────────────────────
  ┌──────────────────────────────────────────────────────┐
  │  Pool (max_size=8 connections)                       │
  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ...           │
  │  │ conn │ │ conn │ │ conn │ │ conn │                │
  │  └──────┘ └──────┘ └──────┘ └──────┘                │
  │           lock-free free-list                        │
  └──────────────────────────────────────────────────────┘
  acquire() → O(1)   release() → O(1)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `LockFreeConnectionPool(config)` | Zero-allocation connection pool |
| `pool.acquire()` | Acquire a connection (blocking if pool exhausted) |
| `pool.release(conn)` | Return connection to pool |
| `pool.available_count()` | Current free connection count |
| `InMemorySessionStore` | Thread-safe O(1) session storage |
| `store.create(data)` | Create session; returns `session_id` |
| `store.get(session_id)` | Lookup session data |
| `store.refresh(session_id)` | Extend session TTL |
| `store.destroy(session_id)` | Delete session |

## Input / Output

### Connection Pool

| Operation | Input | Output |
|-----------|-------|--------|
| `acquire()` | — | `DbConnection*` or blocks |
| `execute_query("SELECT ...")` | SQL string | Result rows |
| `release(conn)` | connection | returned to pool |

### Session Store

| Operation | Input | Output |
|-----------|-------|--------|
| `create({user_id, roles})` | `SessionData` | `"sess-abc123"` |
| `get("sess-abc123")` | session_id | `SessionData` |
| `refresh("sess-abc123")` | session_id | TTL extended |
| `destroy("sess-abc123")` | session_id | removed |

## How to Run

```bash
# Requires qbuem-json
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target db_session_example
./build/examples/09-database/db_session/db_session_example
```

## Notes

- `LockFreeConnectionPool` uses a lock-free CAS-based stack; `acquire()` spins briefly if the pool is momentarily exhausted.
- `InMemorySessionStore` expires sessions based on TTL; call a periodic `cleanup()` to purge expired entries.
- For a real PostgreSQL driver, combine with `qbuem::db::PgDriver`.
- Requires `qbuem-json` for JSON serialization of session data.
