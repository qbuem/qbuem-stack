# CLAUDE.md — qbuem-stack AI Context

This file provides structured context for AI coding assistants working in this repository.

---

## Language Policy

**All code, comments, documentation, and user-facing strings MUST be written in English.**

This is a hard requirement that applies to:
- All source file comments (`//`, `/* */`, `/** */` Doxygen)
- All string literals visible to users (error messages, log output, example output)
- All documentation files (`.md`, `.rst`)
- All new code contributions by AI assistants

Korean or other non-English text in code comments, docs, or strings is a review failure.
Existing Korean comments in legacy files should be translated to English when touched.

---

## Project Identity

**qbuem-stack v2.2.0** — Zero Latency · Zero Allocation · Zero Dependency
C++23 high-performance infrastructure library for WAS (Web Application Servers), IPC, and data pipelines.

- **Language**: C++23 (concepts, coroutines `co_await`/`co_return`, `std::expected`, `std::span`, `std::format`, `std::print`/`std::println`, `std::unreachable()`, `if consteval`, `std::to_underlying`)
- **Platform**: POSIX only — Linux (`io_uring`) and macOS (`kqueue`). Windows is explicitly unsupported.
- **Dependencies**: Zero in core headers. Optional `qbuem-json` fetched via CMake `FetchContent` for examples.
- **Build system**: CMake ≥ 3.20, `C++23` required, no extensions.

---

## Build Commands

```bash
# Configure (from repo root) — requires a C++23-capable compiler (GCC 13+, Clang 17+)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build everything
cmake --build build --parallel

# Build examples only
cmake --build build --target all -j$(nproc)

# Run tests
cd build && ctest --output-on-failure

# Build with optional qbuem-json (enables JSON-dependent examples)
cmake -B build -DQBUEM_JSON_TAG=main
cmake --build build --parallel
```

CMake options:
| Option | Default | Description |
|---|---|---|
| `QBUEM_BUILD_TESTS` | ON | Build unit tests under `tests/` |
| `QBUEM_BUILD_EXAMPLES` | ON | Build all 44 examples under `examples/` |
| `QBUEM_BUILD_BENCH` | ON | Build benchmarks under `bench/` |
| `QBUEM_JSON_TAG` | `"main"` | qbuem-json git tag for FetchContent |

---

## Repository Layout

```
include/qbuem/          ← ALL public headers (header-centric library)
  qbuem_stack.hpp       ← Umbrella include — pulls everything
  pipeline/             ← StaticPipeline, DynamicPipeline, PipelineGraph, actions
  shm/                  ← SHMChannel<T>, SHMBus, SHMSource<T>, SHMSink<T>
  reactor/              ← Dispatcher, Task<T>, coroutine infrastructure
  net/                  ← TCP/UDP/Unix socket async primitives
  http/                 ← HTTP/1.1 SIMD parser, Request, Response
  tracing/              ← W3C TraceContext, SpanExporter, OTLP
  security/             ← JwtAuthAction, kTLS
  buf/                  ← Arena, FixedPoolResource, AsyncLogger
  ...

src/                    ← Non-inline implementations
tests/                  ← Unit tests (mirrors include/ structure)
examples/               ← 44 examples in 11 category subdirectories
  01-foundation/        → hello_world, async_timer
  02-network/           → tcp_echo_server, udp, websocket
  03-memory/            → arena, zero_copy, numa_hugepages
  04-codec-security/    → codec, crypto_url, security_middleware
  05-pipeline/          → fanout, hardware_batching, dynamic_hotswap, sensor_fusion, observer_health, factory, subpipeline_migration, stream_ops, windowed_action
  06-ipc-messaging/     → shm_channel, ipc_pipeline (flagship), message_bus, priority_spsc_channel
  07-resilience/        → canary, checkpoint, resilience, saga, scatter_gather, idempotency_slo
  08-observability/     → tracing, timer_wheel, task_group
  09-database/          → db_session, coro_json
  10-hardware/          → hardware_io, kqueue_sophistication (macOS only)
  11-advanced-apps/     → autonomous_driving, sensor_fusion, io_metrics_dashboard, trading_platform, game_server, middleware
docs/                   ← Design documents (all English Markdown)
bench/                  ← Benchmarks
```

---

## Core Architecture: The 9 Levels

| Level | Module | Key Types |
|---|---|---|
| 1 Foundation | `result`, `arena`, `crypto` | `Result<T>`, `Arena`, `FixedPoolResource` |
| 2 Async Core | `task`, `reactor`, `dispatcher` | `Task<T>`, `Dispatcher`, `Reactor` |
| 3 IO Primitives | `net`, `buf`, `file`, `shm` | `TcpSocket`, `SHMChannel<T>`, `AsyncFile` |
| 4 Transport | `transport`, `codec`, `server` | `LengthPrefixCodec`, `LineCodec` |
| 5 Web/HTTP | `http`, `http-server` | `Request`, `Response`, `App` |
| 6 Pipeline | `context`, `channel`, `pipeline` | `StaticPipeline`, `DynamicPipeline`, `AsyncChannel<T>` |
| 7 Extensions | `graph`, `resilience`, `db-abstraction`, `security-core` | `PipelineGraph`, `CircuitBreaker`, `RetryAction` |
| 8 Protocols | `ws`, `http2`, `grpc`, `db-pg` | `WebSocket`, `GrpcChannel` |
| 9 Umbrella | `qbuem` | `qbuem_stack.hpp` — pulls everything |

---

## Key Concepts for AI Assistants

### Pipeline System

```cpp
// StaticPipeline — compile-time type-checked chain
auto p = PipelineBuilder<InputType, OutputType>{}
    .add<Intermediate1>(stage_fn_1)   // stage_fn: Task<Result<Out>>(In, stop_token)
    .add<Intermediate2>(stage_fn_2)
    .build();                          // returns StaticPipeline<...>
p.start(dispatcher);

// DynamicPipeline — runtime-configurable, hot-swappable stages
DynamicPipeline dp;
dp.add_stage("parse",    parse_fn);
dp.add_stage("validate", validate_fn);
dp.start(dispatcher);
dp.hot_swap("validate", new_validate_fn);  // replace stage live

// PipelineGraph — DAG fan-out/fan-in
PipelineGraph graph;
graph.add_node("source", source_fn);
graph.add_node("branch_a", branch_a_fn);
graph.add_node("branch_b", branch_b_fn);
graph.add_node("merge", merge_fn);
graph.connect("source", {"branch_a", "branch_b"});
graph.connect({"branch_a", "branch_b"}, "merge");
```

### IPC Adapters

```cpp
// SHMChannel → Pipeline via SHMSource
auto pipeline = PipelineBuilder<RawOrder, FinalOrder>{}
    .with_source(SHMSource<RawOrder>("channel_name"))     // reads from shared memory
    .add<ParsedOrder>(stage_parse)
    .with_sink(MessageBusSink<FinalOrder>(bus, "topic"))  // writes to MessageBus
    .build();

// MessageBus — in-process pub/sub
MessageBus bus;
auto sub = bus.subscribe<ValidatedOrder>("validated");
bus.publish("validated", order);

// SHMBus — cross-process topic pub/sub
SHMBus shmbus("my_bus", SHMBus::Scope::LOCAL_ONLY);
shmbus.publish("topic", msg);
auto sub = shmbus.subscribe<Msg>("topic");
```

### Action Patterns

```cpp
// Stage function signature (mandatory)
Task<Result<OutputType>> my_stage(InputType input, std::stop_token st) {
    if (st.stop_requested()) co_return std::unexpected(errc::operation_canceled);
    // ... processing ...
    co_return OutputType{...};
}

// RetryAction — wraps a stage with retry logic
RetryAction retry{ExponentialBackoff{.base_ms=10, .max_ms=5000, .jitter=true}};

// CircuitBreaker
CircuitBreaker cb{.failure_threshold=5, .recovery_timeout=std::chrono::seconds{30}};

// BatchAction — accumulate N items then dispatch
BatchAction<Item, HWResult> batch{.batch_size=64, dispatch_to_hw_fn};
```

### Resilience Triad

```cpp
// RetryAction + CircuitBreaker + DeadLetterQueue
auto stage = RetryAction{backoff_policy}
    .with_circuit_breaker(cb)
    .with_dead_letter_queue(dlq);
```

### Async & Coroutines

```cpp
// All async operations return Task<T>
Task<Result<Response>> handle(Request req, stop_token st) {
    auto data = co_await db.query("SELECT ...", st);
    if (!data) co_return std::unexpected(data.error());
    co_return Response{}.body(data->to_json());
}

// Structured concurrency with TaskGroup
TaskGroup group;
group.spawn(task_a);
group.spawn(task_b);
auto results = co_await group.join_all<int>();
```

### Memory Primitives

```cpp
Arena arena{4096};                       // bump allocator, reset per-request
auto* ptr = arena.allocate<MyStruct>();  // zero-allocation allocation
arena.reset();                           // O(1) reset, no dealloc calls

FixedPoolResource<Entry, 256> pool;      // pool for event entries
auto* e = pool.acquire();
pool.release(e);
```

---

## Review Pass/Fail Criteria

Every code contribution — human or AI — is evaluated against the pillars below.
**A single violation in a hot path is a review failure and must be fixed before merge.**

### Pillar 0 — Extreme Performance Implementation (v2.3.0+)

> [!IMPORTANT]
> **RULE #1**: ALL implementation work MUST strictly follow the technical specifications and research findings found in the `docs/` directory. Any deviation from the established high-performance patterns (e.g., using standard heap allocation instead of `Arena` or `FixedPoolResource`) is a critical review failure.

| # | Rule | Requirement |
|---|------|-------------|
| E1 | **Reference Design Alignment** | Every implementation MUST follow the specific design guide in `docs/` (e.g., `docs/kqueue-optimization-guide.md`). |
| E2 | **Platform Reactor Selection** | Linux: `io_uring` Multishot. macOS: `kqueue` udata-dispatch. Windows: `RIO` (Registered IO). |
| E3 | **Zero-Copy File I/O** | Must use `O_DIRECT` + `io_uring` fixed buffers or Windows RIO registered buffers. |
| E4 | **Hardware Locality** | MSI-X interrupts must be affine to the reactor CPU core. |
| E5 | **Distributed Zero-Copy** | Use `NVMe-oF` via RDMA or `copy_file_range` for data migration. |

---

### Pillar 1 — Zero Latency

The goal is deterministic, bounded latency on every hot path.

| # | Rule | Failure example |
|---|------|-----------------|
| L1 | No blocking syscalls on a reactor thread | `read()`, `write()`, `sleep()`, `nanosleep()` called inside a coroutine without `co_await` |
| L2 | No unbounded spin-waits | `while (!flag.load()) {}` without a yield or backoff |
| L3 | No `std::mutex` on the hot path | Use lock-free atomics or SPSC queues; mutexes only on cold paths |
| L4 | No `std::condition_variable` on the hot path | Replace with reactor `post()` / channel wakeup |
| L5 | No `std::this_thread::sleep_for` on a reactor thread | Use `co_await timer` or schedule via `TimerWheel` |
| L6 | poll/wait timeout ≤ 1 ms for reactors | `epoll_wait` / `io_uring_wait` / `kevent` must not block longer than 1 ms |
| L7 | No `std::format` / string construction on the hot path | Format only in the flush thread (AsyncLogger pattern) |
| L8 | All coroutine frame sizes must be bounded at compile time | No VLA or dynamic-size `co_await` inside a tight loop |

---

### Pillar 2 — Zero Copy

Data must not be copied unless the type system forces it (i.e., `!std::is_trivially_copyable_v<T>`).

| # | Rule | Failure example |
|---|------|-----------------|
| C1 | Pass buffers as `std::span<const std::byte>` or `std::span<T>`, never by value | `void process(std::vector<uint8_t> buf)` — copies the entire buffer |
| C2 | Use `std::string_view` for read-only string arguments, never `std::string` | `void log(std::string msg)` on the hot path |
| C3 | Network I/O uses scatter-gather (`iovec` / `io_uring` fixed buffers) | Single-buffer `send()`/`recv()` on high-throughput paths |
| C4 | SHM types must satisfy `std::is_trivially_copyable_v<T>` — no hidden copies | `SHMChannel<std::string>` — copies via constructor |
| C5 | Pipeline stage functions take `InputType` by value only when ownership is required; otherwise `const InputType&` or moved-in unique ownership | Copying a `ParsedEvent` into every downstream branch |
| C6 | `std::move()` must be used when transferring ownership out of a stage | Returning `result` by copy when the local variable is never used again |
| C7 | No intermediate `std::string` construction for binary data — use `std::span<std::byte>` | `std::string s(reinterpret_cast<const char*>(buf), len)` then passing `s` to the next stage |

---

### Pillar 3 — Zero Allocation

No heap allocation on the hot path. "Hot path" = any code reached per-request or per-message.

| # | Rule | Failure example |
|---|------|-----------------|
| A1 | No `new` / `delete` / `malloc` / `free` on the hot path | `auto* ctx = new RequestContext()` per request |
| A2 | No `std::vector`, `std::string`, `std::map` construction on the hot path | `std::vector<int> results;` inside a stage function body |
| A3 | Use `Arena` for per-request objects; call `arena.reset()` at end of request | Forgetting `reset()` causes unbounded arena growth |
| A4 | Use `FixedPoolResource<T, N>` for fixed-size event pools | Allocating event entries with `std::make_shared<Event>()` |
| A5 | `std::format` returns `std::string` — forbidden on hot path | Calling `std::format(...)` inside a stage function |
| A6 | Coroutine frames must not allocate per-resume — avoid captures that force heap frame elision failure | Capturing `std::string` by value in a coroutine lambda |
| A7 | `std::function` is forbidden on the hot path (heap-allocates closures > ~16 bytes) | Storing `std::function<void()>` in a per-message struct |
| A8 | `std::shared_ptr` is forbidden on the hot path (atomic refcount = allocation + contention) | `std::shared_ptr<Packet>` passed between pipeline stages |
| A9 | Exceptions are forbidden — they trigger heap allocation for the exception object | Any `throw` statement in hot-path code |
| A10 | `co_await` inside a loop must not allocate a new coroutine frame each iteration | Spawning a new `Task<>` per loop iteration instead of reusing a persistent worker |

---

### Pillar 4 — Zero Dependency

Core headers (`include/qbuem/`) must compile with zero third-party includes.

| # | Rule | Failure example |
|---|------|-----------------|
| D1 | No third-party `#include` in any public header | `#include <nlohmann/json.hpp>` in a pipeline header |
| D2 | No Boost headers anywhere in `include/` or `src/` | `#include <boost/asio.hpp>` |
| D3 | Only C++23 standard library headers permitted in `include/` | `#include <fmt/format.h>` — use `<format>` instead |
| D4 | Optional integrations (JSON, logging libs) belong in `examples/` only | JSON serialization logic inside `include/qbuem/pipeline/` |
| D5 | CMake `target_link_libraries` for core targets must list only system libs | Adding `nlohmann_json` to `qbuem_stack` link deps |
| D6 | `FetchContent` / `find_package` for non-test deps is forbidden | Pulling `spdlog` for core logging |

---

### Pillar 5 — C++23 Compliance

All new and modified code must use C++23 features where applicable. Using an older equivalent is a review failure.

| # | Rule | Failure example |
|---|------|-----------------|
| M1 | `std::jthread` instead of `std::thread` | `std::thread t([]{...}); t.join();` |
| M2 | `container.contains(key)` instead of `container.count(key)` or `container.find(key) != end()` | `if (map.count(k))` |
| M3 | `std::format(...)` instead of `snprintf` / `sprintf` / `fprintf` / `ostringstream` | `std::ostringstream ss; ss << x;` |
| M4 | `std::span<T>` instead of raw pointer + size pairs | `void f(const T* data, size_t len)` |
| M5 | Designated initializers for aggregate initialization | `MyConfig c; c.timeout = 5; c.retries = 3;` instead of `MyConfig{.timeout=5, .retries=3}` |
| M6 | `[[nodiscard]]` on all functions returning `Result<T>`, `Task<T>`, or error codes | Unmarked `Result<void> init()` |
| M7 | `std::stop_token` for cooperative cancellation — no manual `std::atomic<bool> running` flags | `std::atomic<bool> stop_flag_` as a thread stop mechanism |
| M8 | `std::bit_cast<T>` instead of `reinterpret_cast` for type-punning of trivially-copyable types | `*reinterpret_cast<float*>(&int_val)` |
| M9 | Concepts (`requires` / `concept`) for template constraints instead of SFINAE | `std::enable_if_t<...>` or `std::void_t<...>` |
| M10 | Three-way comparison `operator<=>` for types that need ordering | Manual `operator<`, `operator>`, `operator<=`, `operator>=` |
| M11 | `std::print`/`std::println` instead of `printf`/`cout` in examples | `printf("value: %d\n", x);` or `std::cout << "value: " << x << "\n";` in example code |
| M12 | `std::unreachable()` instead of `__builtin_unreachable()` | `__builtin_unreachable();` in default branches or postcondition assertions |
| M13 | `std::expected<T, E>` / `std::unexpected<E>` used directly (via `Result<T>` alias) — standard C++23, no polyfill needed | Using a custom `Expected<T,E>` type or a third-party polyfill |
| M14 | `if consteval` instead of `std::is_constant_evaluated()` | `if (std::is_constant_evaluated()) { ... }` in constexpr functions |
| M15 | `std::to_underlying(e)` instead of `static_cast<std::underlying_type_t<E>>(e)` | `static_cast<int>(my_enum_value)` when converting enum to its underlying integer type |

---

## Critical Design Rules

1. **No exceptions** — `Task<T>::unhandled_exception()` calls `std::terminate()`. Always return `Task<Result<T>>` and propagate errors as values via `std::unexpected(errc)`.

2. **No cross-reactor resume** — Never call `handle.resume()` from a different reactor thread. Use `waiter.reactor->post([h]{ h.resume(); })`.

3. **Zero dependencies in headers** — Public headers must not include external library headers. JSON, logging, etc. are application-level concerns.

4. **SHMChannel requires trivially_copyable** — `T` in `SHMChannel<T>` must satisfy `std::is_trivially_copyable_v<T>`.

5. **stop_token at every co_await** — Check `st.stop_requested()` before every suspension point in stage functions.

6. **cache-line alignment** — Use `alignas(64)` on shared mutable data structures to prevent false sharing.

---

## Key Files for Common Tasks

| Task | Files to read |
|---|---|
| Understand pipeline API | `include/qbuem/pipeline/static_pipeline.hpp`, `docs/pipeline-master-guide.md` |
| Add a new pipeline stage | `include/qbuem/pipeline/pipeline_builder.hpp`, examples in `examples/05-pipeline/` |
| IPC / SHM integration | `include/qbuem/shm/shm_channel.hpp`, `docs/shm-messaging.md`, `examples/06-ipc-messaging/ipc_pipeline/` |
| HTTP handler | `include/qbuem/http/`, `examples/02-network/tcp_echo_server/` |
| Resilience patterns | `include/qbuem/pipeline/resilience/`, `examples/07-resilience/resilience/` |
| Distributed tracing | `include/qbuem/tracing/`, `examples/08-observability/tracing/` |
| Memory allocation | `include/qbuem/buf/arena.hpp`, `examples/03-memory/arena/` |

---

## Branch Naming Convention

| Pattern | Purpose |
|---|---|
| `feat/<name>` | New feature |
| `fix/<name>` | Bug fix |
| `docs/<name>` | Documentation change |
| `pipeline/<component>` | Pipeline layer work |
| `perf/<target>` | Performance optimization |

---

## Testing

Tests mirror the `include/` structure under `tests/`. Pipeline components must test:
- Normal flow (single worker, multi-worker)
- Backpressure (channel saturation)
- EOS propagation (correct shutdown after `close()`)
- Scale-in / scale-out (dynamic worker count changes)
- Drain (items in-flight complete before shutdown)
- Cross-reactor scenarios (multi-thread Dispatcher)

---

## Platform Notes

- **Linux**: `io_uring` for async I/O. `QBUEM_HAS_IOURING` preprocessor flag.
- **macOS**: `kqueue` — `EVFILT_TIMER`, `EVFILT_USER`, `EVFILT_READ`, batched `kevent64()`.
- `kqueue_sophistication` example is macOS-only (`if(APPLE)` in CMakeLists.txt).
- `NUMA` and hugepage examples require Linux kernel ≥ 5.x with appropriate permissions.
