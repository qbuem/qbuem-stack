# qbuem-stack — Usage Guide

A practical, task-oriented guide to building on **qbuem-stack**, the C++23
zero-copy / zero-latency / zero-allocation / zero-dependency infrastructure SDK.

> **Scope.** This guide covers the **production-ready subset** that is built and
> tested in CI on Linux (`io_uring`) and macOS (`kqueue`). Subsystems that are
> experimental, platform-gated, or not yet implemented are listed honestly in
> [§12 Experimental & Not-Yet-Implemented](#12-experimental--not-yet-implemented)
> — do not build on those without reading that section first.

> Every code example below has a corresponding, compiled, runnable program under
> `examples/`. When in doubt, the example file is the source of truth.

---

## Table of contents

1. [The four pillars (what the API guarantees)](#1-the-four-pillars)
2. [Build & integrate](#2-build--integrate)
3. [Core types: `Result<T>`, `Task<T>`, the reactor](#3-core-types)
4. [Memory: `Arena` & `FixedPoolResource`](#4-memory)
5. [Networking: TCP / UDP / Unix sockets](#5-networking)
6. [HTTP server & client](#6-http)
7. [WebSocket](#7-websocket)
8. [Pipelines: Static, Dynamic, Graph](#8-pipelines)
9. [Resilience: Retry, CircuitBreaker, DLQ](#9-resilience)
10. [IPC: SHM channels, MessageBus, scatter-gather](#10-ipc)
11. [Crypto, tracing, middleware](#11-crypto-tracing-middleware)
12. [Experimental & Not-Yet-Implemented](#12-experimental--not-yet-implemented)
13. [Platform notes](#13-platform-notes)

---

## 1. The four pillars

Every hot-path API is designed around four guarantees. When you write code on
top of qbuem-stack, your stage/handler functions are expected to honor them too:

| Pillar | What it means for your code |
|--------|-----------------------------|
| **Zero Latency** | No blocking syscalls, `std::mutex`, or `std::format` on a reactor thread. Use `co_await`; mutexes only on cold/setup paths. |
| **Zero Copy** | Pass buffers as `std::span` / `std::string_view`, never by value. Move ownership with `std::move`. |
| **Zero Allocation** | No `new`/`vector`/`string`/`std::function` per request/message. Use `Arena` + `FixedPoolResource`. |
| **Zero Dependency** | Public headers include only the C++23 standard library. JSON/logging are application concerns (kept in `examples/`). |

Errors are values, never exceptions: every fallible call returns
`Result<T>` = `std::expected<T, std::error_code>`. Throwing inside a coroutine
calls `std::terminate()` by design.

---

## 2. Build & integrate

**Requirements:** a C++23 compiler (GCC 13+, Clang 17+, Apple Clang 15+), CMake ≥ 3.20, POSIX (Linux or macOS; Windows is unsupported).

### As a subproject (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(qbuem-stack
    GIT_REPOSITORY https://github.com/qbuem/qbuem-stack.git
    GIT_TAG        main)
FetchContent_MakeAvailable(qbuem-stack)

target_link_libraries(my_app PRIVATE qbuem-stack::qbuem)   # umbrella target
```

### Against an installed copy (find_package)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix /opt/qbuem
```
```cmake
find_package(qbuem-stack REQUIRED)
target_link_libraries(my_app PRIVATE qbuem-stack::qbuem)
```

You can also link finer-grained targets (`qbuem-stack::result`,
`qbuem-stack::reactor`, `qbuem-stack::http`, …) — see the top-level `CMakeLists.txt`.

### Include everything, or à la carte

```cpp
#include <qbuem/qbuem_stack.hpp>     // umbrella — pulls the whole SDK
// — or just what you need —
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/net/tcp_listener.hpp>
```

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `QBUEM_BUILD_TESTS` | ON | Unit tests under `tests/` |
| `QBUEM_BUILD_EXAMPLES` | ON | All examples under `examples/` |
| `QBUEM_BUILD_BENCH` | ON | Benchmarks under `bench/` |
| `QBUEM_ENABLE_LTO` | OFF | Interprocedural optimization (LTO) for library targets |
| `QBUEM_ENABLE_NATIVE_CRYPTO` | OFF | Host hardware SHA-2/AES for the crypto module (~11× SHA-256; host-targeting) |
| `QBUEM_SANITIZE` | "" | `asan` \| `tsan` \| `msan` |
| `QBUEM_XDP` | OFF | AF_XDP support (Linux 4.18+) |

---

## 3. Core types

### `Result<T>`

```cpp
#include <qbuem/common.hpp>
using qbuem::Result;

Result<int> parse_port(std::string_view s) {
    int v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{}) return std::unexpected(std::make_error_code(ec));
    return v;                          // implicit success
}

auto r = parse_port("8080")
    .transform([](int p){ return p + 1; })          // map success
    .value_or(0);                                    // fallback on error
```

`Result<void>` signals success/failure with no value; `return {};` is success.

### `Task<T>` and the reactor

All async operations return `Task<T>`. A `Dispatcher` owns one reactor per
worker thread (`io_uring` on Linux, `kqueue` on macOS) and runs your coroutines.

```cpp
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>

qbuem::Task<void> worker(std::stop_token st) {
    co_await qbuem::AsyncSleep{10};    // 10 ms, reactor timer (not a blocking sleep)
    co_return;
}

int main() {
    qbuem::Dispatcher dispatcher{/*threads=*/4};
    dispatcher.spawn(worker(/*…*/));   // spawn is thread-safe
    dispatcher.run();                  // blocks until stop()
}
```

**Rules of the road**
- Check `st.stop_requested()` before each suspension point in long-running loops.
- Never `handle.resume()` from a different reactor thread — post to the owner:
  `reactor->post([h]{ h.resume(); })`. `Dispatcher::spawn` already does this.

> 📁 Canonical example: `examples/01-foundation/async_timer/`,
> `examples/08-observability/task_group/`.

---

## 4. Memory

`Arena` is a bump allocator: allocate per-request objects, then reset in O(1).

```cpp
#include <qbuem/buf/arena.hpp>

qbuem::Arena arena{4096};
auto* ctx = arena.allocate<RequestContext>();   // no malloc
// … handle request …
arena.reset();                                   // O(1), reuses memory
```

`FixedPoolResource<T, N>` is a fixed-size pool for event entries / fixed objects:

```cpp
#include <qbuem/buf/fixed_pool.hpp>
qbuem::FixedPoolResource<Event, 256> pool;
auto* e = pool.acquire();
pool.release(e);
```

> 📁 `examples/03-memory/arena/`, `examples/03-memory/zero_copy_arena_channel/`.

---

## 5. Networking

Move-only RAII socket types with coroutine I/O. Non-blocking + close-on-exec is
handled portably (`net::make_socket` / `accept_nonblock_cloexec` —
see `net/socket_compat.hpp`).

```cpp
#include <qbuem/net/tcp_listener.hpp>

qbuem::Task<void> echo_server(std::stop_token st) {
    auto listener = qbuem::TcpListener::bind(
        *qbuem::SocketAddr::from_ipv4("0.0.0.0", 9000));
    if (!listener) co_return;

    while (!st.stop_requested()) {
        auto client = co_await listener->accept();
        if (!client) continue;
        // spawn a handler coroutine per connection …
    }
}
```

`TcpStream` supports scatter-gather (`readv`/`writev`); `UdpSocket`,
`UnixSocket`, and `UdpMmsgSocket` (batched `recvmmsg`/`sendmmsg` on Linux,
emulated on macOS) are also available.

> 📁 `examples/02-network/tcp_echo_server/`, `examples/02-network/udp_unix_socket/`.

---

## 6. HTTP

### Server (`App`)

```cpp
#include <qbuem/qbuem_stack.hpp>

qbuem::App app;
app.get("/health", [](const Request& req, Response& res) {
    res.status(200).body("ok");
});
app.use(qbuem::middleware::request_id());   // adds X-Request-ID (CSPRNG)
app.listen(8080);
```

The HTTP/1.1 parser is SIMD-accelerated; responses use `IOVec<2>` + a single
`writev` for the header+body. `Content-Length` parsing is non-throwing
(malformed input → `400`, never a crash).

> 📁 `examples/01-foundation/hello_world/`, `examples/11-advanced-apps/middleware/`.

### Fetch client

```cpp
#include <qbuem/http/fetch.hpp>

// Builder + co_await .send(st) pattern:
auto resp = co_await qbuem::fetch("https://api.example.com/data").send(st);
if (resp && resp->status() == 200)
    process(resp->body());
```

Retry/circuit-breaker wrappers: `<qbuem/http/fetch_pipeline.hpp>`; backoff
strategies: `<qbuem/http/backoff.hpp>`.

> 📁 `examples/02-network/http_fetch/` (builds and runs on macOS).

---

## 7. WebSocket

```cpp
#include <qbuem/server/websocket_handler.hpp>
// Upgrade an HTTP connection, then per-frame:
//   co_await ws.send(frame);
//   auto frame = co_await ws.recv();
```

XOR masking is SIMD-accelerated. Inbound frames are capped (16 MiB default) to
reject the unbounded-allocation DoS in the 64-bit length field.

> 📁 `examples/02-network/websocket/`.

---

## 8. Pipelines

Three flavors, three trade-offs.

### StaticPipeline — compile-time type-checked (the workhorse)

```cpp
#include <qbuem/pipeline/pipeline_builder.hpp>

// stage signature: Task<Result<Out>>(In, std::stop_token)
auto p = qbuem::PipelineBuilder<RawOrder, FinalOrder>{}
    .add<ParsedOrder>(parse_stage)
    .add<ValidatedOrder>(validate_stage)
    .add<FinalOrder>(finalize_stage)
    .build();
p.start(dispatcher);
```

Pass each item by `const In&` or move it; only take by value when you need
ownership. `std::move` results out of a stage.

> 📁 `examples/05-pipeline/sensor_fusion/`.

### DynamicPipeline — runtime-configurable

```cpp
#include <qbuem/pipeline/dynamic_pipeline.hpp>
qbuem::DynamicPipeline dp;
dp.add_stage("parse",    parse_fn);
dp.add_stage("validate", validate_fn);
dp.start(dispatcher);
```

> ⚠ Configure stages **before** `start()`. Runtime `hot_swap`/`add_stage` on a
> running pipeline has known limitations — see §12.

> 📁 `examples/05-pipeline/dynamic_hotswap/`.

### PipelineGraph — DAG fan-out / fan-in

```cpp
#include <qbuem/pipeline/pipeline_graph.hpp>
qbuem::PipelineGraph graph;
graph.add_node("source", source_fn);
graph.add_node("a", a_fn);
graph.add_node("b", b_fn);
graph.add_node("merge", merge_fn);
graph.connect("source", {"a", "b"});
graph.connect({"a", "b"}, "merge");
```

Stream operators (map/filter/window/throttle/debounce):
`<qbuem/pipeline/stream_ops.hpp>`.

> 📁 `examples/05-pipeline/fanout/`, `examples/05-pipeline/stream_ops/`.

---

## 9. Resilience

Compose the triad around any stage:

```cpp
#include <qbuem/pipeline/retry_policy.hpp>
#include <qbuem/pipeline/circuit_breaker.hpp>
#include <qbuem/pipeline/dead_letter.hpp>
using namespace std::chrono_literals;

qbuem::RetryConfig retry_cfg{
    .max_attempts = 3,
    .base_delay   = 10ms,
    .strategy     = qbuem::BackoffStrategy::Exponential,
};

qbuem::CircuitBreaker cb{ {.failure_threshold = 5,
                           .timeout = std::chrono::seconds{30}} };
```

> **CircuitBreaker is a consecutive-failure breaker**: any success resets the
> failure streak, so alternating success/failure traffic will not trip it. This
> is intentional (not a rate-based breaker). `RetryAction::sleep_async` uses a
> reactor timer, never a blocking sleep.

> 📁 `examples/07-resilience/resilience/`, `examples/07-resilience/saga/`.

---

## 10. IPC

### SHMChannel — cross-process ring buffer

`T` must be `std::is_trivially_copyable_v<T>` (enforced by `static_assert`).

```cpp
#include <qbuem/shm/shm_channel.hpp>
qbuem::SHMChannel<Tick> chan{"ticks", /*capacity=*/4096};
chan.try_send(tick);
if (auto v = chan.try_recv()) consume(*v);
```

> Note: the SHM wakeup path currently polls (≈1 ms floor on an empty/full
> channel), not a futex — see §12.

### MessageBus — in-process pub/sub

```cpp
#include <qbuem/shm/message_bus.hpp>
qbuem::MessageBus bus;
auto sub = bus.subscribe<ValidatedOrder>("validated");
bus.publish("validated", order);
```

### Scatter-gather I/O — `IOVec<N>` + `scattered_span`

Zero-copy gather writes (one `writev`/`sendmsg`, no buffer coalescing):

```cpp
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/scattered_span.hpp>

IOVec<2> vec;
vec.push(header.data(), header.size());
vec.push(body.data(),   body.size());
::writev(fd, vec.as_scattered().iov_data(), vec.as_scattered().iov_count());
```

> **Lifetime:** `scattered_span` does **not** own the `IOVec<N>`; keep the
> `IOVec` alive for as long as the span is used.

> 📁 `examples/06-ipc-messaging/shm_channel/`,
> `examples/06-ipc-messaging/ipc_pipeline/` (flagship: SHMSource/Sink ↔ pipeline),
> `examples/06-ipc-messaging/scatter_send/`.

---

## 11. Crypto, tracing, middleware

### Crypto (`qbuem::crypto`)

Self-contained, constant-time, hardware-accelerated (AES-NI/PCLMUL on x86,
AES/PMULL on ARM). AEAD tags are verified before any plaintext is released.

```cpp
#include <qbuem/crypto/aes_gcm.hpp>
auto ctx = qbuem::crypto::AesGcm256::create(key);   // Result — checks HW support
if (!ctx) use_chacha20();                           // fall back if no HW AES

qbuem::crypto::AesGcmTag tag;
ctx->seal(nonce, aad, plaintext, ciphertext_out, tag);
auto ok = ctx->open(nonce, aad, ciphertext, tag, plaintext_out);   // Result<void>
```

> AES-GCM is validated against the NIST GCM known-answer vectors on both x86 and
> ARM (`tests/crypto_primitives_test.cpp`). Also available: ChaCha20-Poly1305
> (constant-time alternative when AES hardware is absent), SHA-256/512, HMAC,
> HKDF, PBKDF2, Base64, and a CSPRNG (`crypto::random_fill`).

### Tracing

W3C TraceContext + span export (OTLP): `<qbuem/tracing/*>`.
> 📁 `examples/08-observability/tracing/`.

### Middleware

`require_json()`, `require_content_type()`, CORS, rate-limit, `request_id()`:
`<qbuem/middleware/*>`.

---

## 12. Experimental & Not-Yet-Implemented

> ⚠ **Read before depending on anything here.** These surfaces compile but are
> incomplete; some have no implementation and will fail to link if used.

| Surface | Status |
|---------|--------|
| `rdma/`, `spdk/`, `ebpf/ebpf_tracer` | **Interface only — no implementation.** Linking against them fails. |
| `db/simd_parser` | **No implementation** (references a non-existent `.cpp`). |
| `shm/futex_sync` | Async `wait`/`wake` **unimplemented**; `SHMChannel` falls back to a ≈1 ms poll. |
| `db/smart_cache` | Works **in-process only** despite "shared memory" docs — each process gets a private cache. |
| `db/connection_pool` | A `std::mutex` + `vector` pool (the "lock-free" path is not wired). |
| AVX2 SIMD: ChaCha20 / Base64 encode / JSON validator | **Scalar fallback active** on x86 (NEON paths are real on ARM). Functionally correct, not yet the advertised throughput. |
| `DynamicPipeline::hot_swap` / runtime `add_stage` after `start()` | Does not safely hot-swap a *running* pipeline — configure before `start()`. |
| `Http2Handler` | No flow control; HPACK Huffman decode not implemented. Experimental. |
| `examples/10-hardware/hardware_io` | Prints the API as strings; does not exercise real hardware. |

The trustworthy core to build on today: **StaticPipeline + AsyncChannel +
RetryAction/DLQ**, the reactor + TCP/UDP/UDS, HTTP/1.1 server + fetch client,
WebSocket, the crypto suite, `SHMChannel`/`MessageBus`, `scattered_span`/`IOVec`,
`Arena`/`FixedPoolResource`, and tracing.

See `docs/audit/2026-06-14_codebase-audit.md` for the full assessment.

---

## 13. Platform notes

| | Linux | macOS |
|---|-------|-------|
| Reactor | `io_uring` (`QBUEM_HAS_IOURING`) | `kqueue` |
| Batched UDP | `recvmmsg`/`sendmmsg` | emulated via `recvmsg`/`sendmsg` loop |
| NUMA / hugepages / AF_XDP | supported (kernel ≥ 5.x) | not available (no-op / `not_supported`) |
| AES-GCM | AES-NI + PCLMUL | ARM AES + PMULL |

Both platforms are first-class and built+tested in CI. Windows is explicitly
unsupported (POSIX only).

---

*Generated 2026-06-14. Pairs with `docs/best-practices.md` (pattern selection)
and `docs/pipeline-master-guide.md` (pipeline deep-dive).*
