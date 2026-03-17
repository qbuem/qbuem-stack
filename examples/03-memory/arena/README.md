# arena

**Category:** Memory
**File:** `arena_example.cpp`
**Complexity:** Beginner

## Overview

Demonstrates three zero-allocation memory primitives in qbuem-stack:

1. **`Arena`** — bump-pointer allocator for per-request scratch memory.
2. **`FixedPoolResource`** — free-list pool for fixed-size objects (e.g., connection descriptors).
3. **`AsyncLogger`** — lock-free SPSC ring-buffer logger for the hot path.

## Scenario

A high-throughput HTTP server parses request headers into an arena-allocated buffer (`Arena`), manages a pool of reusable connection objects (`FixedPoolResource`), and logs access records without blocking the request thread (`AsyncLogger`).

## Architecture Diagram

```
  Per-Request Lifecycle
  ──────────────────────────────────────────────────────────
  Request arrives
       │
       ▼
  Arena (4 KB bump-pointer)
  ├─ allocate(HttpHeader, align)  → O(1), pointer bump
  ├─ allocate(1024 byte buffer)   → O(1), pointer bump
  └─ reset()                     → O(1), pointer rewind
                                   (memory stays allocated)

  Connection Pool Lifecycle
  ──────────────────────────────────────────────────────────
  New TCP connection
       │
       ▼
  FixedPoolResource<sizeof(Connection), alignof(Connection)>
  ├─ allocate()    → O(1), pops head of free-list
  ├─ ... use ...
  └─ deallocate()  → O(1), pushes back to free-list head

  Access Logging (hot path)
  ──────────────────────────────────────────────────────────
  Request handler
       │  AsyncLogger::log(method, path, status, latency_us)
       ▼
  ┌─────────────────────┐
  │  SPSC Ring (1024)   │ ──► Background flush thread → stdout
  └─────────────────────┘
  Non-blocking; returns immediately
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `Arena(capacity)` | Create a bump-pointer arena with the given byte capacity |
| `Arena::allocate(size, align)` | Allocate `size` bytes with the given alignment |
| `Arena::reset()` | Rewind the bump pointer (O(1), no dealloc) |
| `FixedPoolResource<Size, Align>(n)` | Create a free-list pool with `n` slots |
| `FixedPoolResource::allocate()` | Pop a slot from the free-list; returns `nullptr` if full |
| `FixedPoolResource::deallocate(ptr)` | Push the slot back to the free-list |
| `AsyncLogger(ring_size)` | Create a lock-free SPSC logger |
| `AsyncLogger::start()` | Start the background flush thread |
| `AsyncLogger::log(method, path, status, us)` | Non-blocking log enqueue |
| `AsyncLogger::make_callback()` | Return a callable compatible with `App::set_access_logger` |
| `AsyncLogger::stop()` | Flush remaining entries and join the background thread |

## Input / Output

### Arena

| Operation | Input | Output |
|-----------|-------|--------|
| `allocate(HttpHeader)` | sizeof=192 bytes | Pointer valid; name/value set |
| `reset()` | — | Bump pointer reset to 0 |
| `allocate(HttpHeader)` after reset | sizeof=192 bytes | Same memory reused |

### FixedPoolResource

| Operation | Input | Output |
|-----------|-------|--------|
| `allocate()` | — | Non-null `Connection*` (fd=10) |
| `deallocate(c1)` | c1 pointer | Slot returned to free-list |
| `allocate()` after dealloc | — | Same slot reused (fd=12) |
| `allocate()` (pool full) | — | `nullptr` |

### AsyncLogger

| Operation | Input | Output |
|-----------|-------|--------|
| `log(...)` × 5 | 5 log records | Queued non-blocking |
| `stop()` | — | Background thread flushes all, prints to stdout |

## How to Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target arena_example

# Run
./build/examples/03-memory/arena/arena_example
```

## Expected Output

```
=== Arena (bump-pointer) ===
[arena] h1: Content-Type: application/json
[arena] h2: Authorization: Bearer tok123
[arena] 1024-byte buffer allocated, buf[0]=0xab
[arena] reset() done — pointers cleared
[arena] Recycled: X-Request-ID

=== FixedPoolResource (free-list pool) ===
[pool] c1: fd=10 peer=192.168.1.1:54321
[pool] c2: fd=11 peer=10.0.0.5:12345
[pool] c1 released
[pool] c3 (reused slot): fd=12 peer=172.16.0.1:9999
[pool] Overflow allocate: nullptr (expected)

=== AsyncLogger (lock-free SPSC ring) ===
[logger] 5 log entries queued (async flush)
[logger] Flushed and stopped
```

## Performance Characteristics

| Primitive | Allocation Cost | Deallocation Cost |
|-----------|----------------|-------------------|
| `Arena::allocate` | ~4.5 ns (pointer bump) | N/A (bulk reset) |
| `FixedPoolResource::allocate` | ~4.5 ns (list pop) | ~4.5 ns (list push) |
| `AsyncLogger::log` | ~few ns (ring enqueue) | N/A (async flush) |

## Notes

- `Arena` does **not** call destructors; only use it for trivial types or manage lifetimes manually.
- `FixedPoolResource::allocate()` returns `nullptr` when the pool is exhausted; always check the return value.
- `AsyncLogger` uses a SPSC ring — one producer (request thread) and one consumer (flush thread). For multi-threaded producers, use `MultiLogger` instead.
