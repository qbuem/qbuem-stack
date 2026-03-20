# qbuem-stack — Contributing Guide

## Branch Strategy

| Pattern | Purpose |
|---|---|
| `feat/<name>` | New feature |
| `fix/<name>` | Bug fix |
| `docs/<name>` | Documentation change |
| `pipeline/<component>` | Pipeline layer work (e.g. `pipeline/async-channel`) |
| `perf/<target>` | Performance optimization |

Standard GitHub Flow applies for all PRs.

---

## Core Design Principles

### No Exceptions

`Task<T>::unhandled_exception()` calls **`std::terminate()`**.
All processing functions must return `Task<Result<T>>` and propagate errors as values.

```cpp
// Correct: return error as Result
Task<Result<ParsedBody>> parse(HttpRequest req, std::stop_token st) {
    if (req.body().empty()) co_return std::unexpected(errc::invalid_argument);
    co_return ParsedBody{req.body()};
}

// Wrong: throw terminates the process
Task<ParsedBody> parse(HttpRequest req) {
    if (req.body().empty()) throw std::invalid_argument("empty"); // FATAL
    co_return ParsedBody{req.body()};
}
```

### JSON Library Policy

qbuem-stack **core has no JSON dependency**.

| Layer | JSON dependency |
|---|---|
| `qbuem::core` | None |
| `qbuem::http` | None — `body()` returns `std::string_view` raw bytes |
| `qbuem::pipeline` | None — item type is defined by the application |
| `qbuem::qbuem` | None |

Applications are free to use `qbuem-json`, `simdjson`, `nlohmann/json`, `glaze`, or any other library to parse `req.body()`.

### Cross-Reactor Coroutine Resume

Directly calling `resume()` on a coroutine handle from a different reactor thread causes a data race.
Always dispatch through the **waiter's reactor `post()`**.

```cpp
// Wrong: data race
waiter.handle.resume();

// Correct: post to the waiter's reactor
waiter.reactor->post([h = waiter.handle]() { h.resume(); });
```

---

## Pipeline Layer Contribution Guide

### File Layout

```
include/qbuem/pipeline/   ← headers (interface + inline implementation)
src/pipeline/             ← non-inline implementations
tests/pipeline/           ← unit tests
```

### Action Implementation Rules

1. Processing function signature: `Task<Result<Out>>(In, std::stop_token)`
2. Check `stop_token` at every `co_await` suspension point
3. Scale-in: use `atomic<size_t> target_workers` + worker index comparison (no poison pill)
4. Worker spawn: via `Dispatcher::spawn()` or `spawn_on()`

```cpp
// Example action processing function
Task<Result<ProcessedItem>> my_process(RawItem item, std::stop_token st) {
    if (st.stop_requested()) co_return std::unexpected(errc::operation_canceled);

    auto result = co_await some_async_work(item);
    if (!result) co_return std::unexpected(result.error());

    co_return ProcessedItem{std::move(*result)};
}
```

### StaticPipeline vs DynamicPipeline

| Situation | Recommended |
|---|---|
| Types determined at compile time | `StaticPipeline` |
| Configuration from file | `DynamicPipeline` |
| Runtime logic replacement needed | `DynamicPipeline` + `hot_swap()` |
| Maximum performance required | `StaticPipeline` |

### Tracing Rules

- Tracing is **opt-in**: disabled = `NoopExporter` = zero overhead
- `TraceContext` is propagated as thread-local (no item type pollution)
- Do not call `child_span()` for unsampled items — check the Sampler result first
- Access via `Reactor::get_current_trace_context()` inside processing functions

### Test Requirements

Pipeline components must test all of the following:

- Normal flow (single worker, multi-worker)
- Backpressure (blocking send when channel is saturated)
- EOS propagation (correct shutdown after `close()`)
- Scale-in / Scale-out (dynamic worker count changes)
- Drain (in-flight items complete before shutdown)
- Cross-reactor scenarios (multi-thread Dispatcher)

---

---

## Review Pillars

Every PR is evaluated against the six pillars defined in `CLAUDE.md`. A single
violation on a hot path is a **review failure**:

| Pillar | Summary |
|--------|---------|
| **E — Extreme Performance** | Implementations must follow reference designs in `docs/`. |
| **L — Zero Latency** | No blocking syscalls, no `std::mutex`, no `sleep_for` on reactor threads. |
| **C — Zero Copy** | Pass `std::span<const std::byte>` / `std::string_view`; never copy on the hot path. |
| **A — Zero Allocation** | No `new`, `std::vector`, `std::string` construction on the hot path. Use `Arena` / `FixedPoolResource`. |
| **D — Zero Dependency** | No third-party `#include` in public headers (`include/qbuem/`). |
| **H — Hardware Alignment** | SIMD-first (AVX2/NEON parity required), `alignas(64)` on shared mutable structs. |

Full rule tables: `CLAUDE.md` §Review Pass/Fail Criteria.

---

## Spatial Index Contribution Guide

### GridBitset (fixed-size, `include/qbuem/buf/grid_bitset.hpp`)
- Use `detail::scan_row_any()` / `detail::scan_row_count()` for all row-level
  SIMD operations — do not duplicate SIMD logic inline.
- Radius queries must use the per-row sqrt extent pattern (one `sqrt` per row).
- AVX-512 / AVX2 / NEON paths must be kept in parity (Pillar H2).

### TiledBitset (infinite dynamic world, `include/qbuem/buf/tiled_bitset.hpp`)
- Tile lookups must go through the TLS 4-slot cache before acquiring any lock.
- Cross-tile queries must split at `TileW` / `TileH` boundaries and call the
  shared SIMD row helpers.
- `evict_empty_tiles()` requires a `unique_lock` — keep it off the hot path.
- Use `instance_id_` (not `this` pointer) as the TLS cache key to prevent
  use-after-free when the allocator reuses addresses.

---

## Code Style

- **Language**: English only — all comments, strings, and documentation.
- **C++23**: concepts, coroutines, `std::span`, `std::format`,
  `std::print`/`std::println`, `std::expected`, `std::unreachable()`,
  `std::jthread`, `std::to_underlying`.
- No external dependencies in `include/` (OS syscalls only).
- `alignas(64)` on all shared mutable data structures (Pillar H4).
- `[[nodiscard]]` on all functions returning `std::expected`, `Task`, or error codes.
- `[[likely]]` / `[[unlikely]]` on hot-path branches only — do not overuse.
