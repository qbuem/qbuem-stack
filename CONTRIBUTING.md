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

## Code Style

- C++23: use concepts, coroutines, `std::span`, `std::format`, `std::print`/`std::println`, `std::expected`, `std::unreachable()`
- Do not add external dependencies (OS syscalls only)
- `alignas(64)` — apply to shared mutable data at cache-line boundaries
- `[[nodiscard]]` — apply to all functions where ignoring the return value is a bug
- `[[likely]]` / `[[unlikely]]` — hot path branches only (do not overuse)
