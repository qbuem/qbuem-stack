# Getting Started

qbuem-stack is a **header-centric, zero-dependency C++23 infrastructure library**
for building Web Application Servers (WAS), inter-process communication (IPC),
and high-throughput data pipelines. It gives you an async runtime (coroutines +
a per-core reactor), an HTTP/1.1+2 and WebSocket server, a composable pipeline
engine, shared-memory IPC, crypto primitives, zero-allocation memory pools, a
resilience toolkit, and distributed tracing — all with **no third-party
dependencies in the public headers**.

---

## Supported platforms

| Platform | Reactor backend | Notes |
|---|---|---|
| **Linux x86_64** | io_uring (kernel ≥ 5.x) or epoll | io_uring is auto-detected; epoll is the universal fallback |
| **Linux ARM64** (Jetson-class boards) | io_uring or epoll | same as above |
| **macOS aarch64** (Apple Silicon) | kqueue | `EVFILT_READ/WRITE/USER/TIMER` |

Windows and exotic-hardware backends (RDMA, AF_XDP, NVMe-passthrough, PCIe/VFIO,
eBPF) are **out of scope by design** — they would require third-party libraries
or special hardware, which breaks the zero-dependency contract.

---

## The four pillars

Everything in the library is built to honor these. They are enforced in review
(see the root `CLAUDE.md`) and they shape every API:

| Pillar | What it means for you |
|---|---|
| **Zero Latency** | No blocking syscalls, mutexes, or `std::format` on hot paths. Timeouts ≤ 1 ms. |
| **Zero Copy** | Buffers move as `std::span`/`string_view`; scatter-gather (`IOVec<N>` + `scattered_span`) writes with one `writev`. |
| **Zero Allocation** | Per-request `Arena`, `FixedPoolResource` pools, `inplace_function`. No `new`/`std::function`/`shared_ptr` on hot paths. |
| **Zero Dependency** | C++23 stdlib + OS/arch intrinsics only. No third-party `#include` in `include/`. |

**Errors are values, never exceptions.** Almost every fallible call returns
`Result<T>` (= `std::expected<T, std::error_code>`) or `Task<Result<T>>`.
Throwing inside a coroutine calls `std::terminate()` — always `co_return
unexpected(...)` instead.

---

## Build & install

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cd build && ctest --output-on-failure
```

- **Compiler:** GCC 13+ or Clang 17+ (C++23, no extensions).
- **liburing** is used for the io_uring reactor when present and **falls back to
  epoll automatically** when absent — there is no hard dependency.
- Optional `qbuem-json` (via CMake `FetchContent`) only enables JSON examples.

| CMake option | Default | Description |
|---|---|---|
| `QBUEM_BUILD_TESTS` | ON | Unit tests under `tests/` |
| `QBUEM_BUILD_EXAMPLES` | ON | All examples under `examples/` |
| `QBUEM_BUILD_BENCH` | ON | Benchmarks under `bench/` |
| `QBUEM_ENABLE_LTO` | OFF | Interprocedural optimization (LTO) for library targets |
| `QBUEM_ENABLE_NATIVE_CRYPTO` | OFF | Host hardware SHA-2/AES for the crypto module (~11× SHA-256; host-targeting) |
| `QBUEM_JSON_TAG` | `"main"` | optional `qbuem-json` tag |

Consume from your own CMake project:

```cmake
find_package(qbuem-stack CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE qbuem-stack::qbuem)
```

---

## Hello, server

```cpp
#include <qbuem/qbuem_stack.hpp>

int main() {
    qbuem::App app;

    app.get("/hello", [](const qbuem::Request& req, qbuem::Response& res) {
        res.status(200).body("Zero allocation hello");
    });

    app.get("/user/:id", [](const qbuem::Request& req, qbuem::Response& res)
        -> qbuem::Task<void> {
        res.status(200).body("{\"id\":\"" + std::string(req.param("id")) + "\"}");
        co_return;
    });

    app.health_check();          // GET /health → {"status":"ok"}
    app.metrics_endpoint();      // GET /metrics → Prometheus text
    return app.listen(8080) ? 0 : 1;
}
```

---

## The mental model: reactor + coroutines

qbuem-stack is **shared-nothing, one-thread-per-reactor** (the Nginx/Redis
model). The `Dispatcher` owns one `Reactor` per CPU core. A coroutine
(`Task<T>`) `co_await`s an awaiter (`AsyncRead`/`AsyncWrite`/`AsyncSleep`/
`AsyncAccept`), which registers an event on the current reactor and resumes the
coroutine **on the same thread** when the event fires.

Two invariants follow:
1. A reactor and everything it owns (allocators, `TimerWheel`, `Task` frames)
   live on **one thread** and are not thread-safe. Touch another reactor only
   via `Reactor::post()` / `Dispatcher::post()`.
2. **Never `resume()` a coroutine across threads** — marshal with `post()`.

See **[Core & Async Runtime](./02-core-and-async.md)** for the full treatment.

---

## How to choose — feature map

| You want to… | Look at |
|---|---|
| Run async code, understand `Task`/`Reactor`/`Dispatcher`, use `Arena`/pools | **[02 — Core & Async](./02-core-and-async.md)** |
| Build a processing pipeline (static/dynamic/DAG), add retry/circuit-breaker, batching, windows | **[03 — Pipeline](./03-pipeline.md)** |
| Serve HTTP/1.1, HTTP/2, WebSocket, gRPC; route; middleware; the curl-free fetch client | **[04 — HTTP & Server](./04-http-and-server.md)** |
| Hash, MAC, derive keys, encrypt (AEAD), parse JWTs | **[05 — Crypto & Security](./05-crypto-and-security.md)** |
| TCP/UDP/Unix sockets, UDS FD passing, scatter-gather I/O, shared-memory IPC | **[06 — Networking, I/O & SHM](./06-net-io-shm.md)** |
| Zero-alloc pools, `inplace_function`, codecs, HTTP middleware, spatial bitsets | **[07 — Buffers, Codecs & Middleware](./07-buffers-codecs-middleware.md)** |
| DB connection pool / cache interfaces, tracing, config + `Secret<T>` | **[08 — DB, Tracing & Config](./08-db-tracing-config.md)** |

Every section documents each feature with **what it is · when to use it · how to
use it (real, copy-pasteable code) · gotchas**, and points at a runnable example
under [`examples/`](../../examples/).

---

## Where things live

```
include/qbuem/
  core/        Task, Reactor (epoll/io_uring/kqueue), Dispatcher, Arena, TimerWheel, awaiters, TickLoop/TickScheduler
  pipeline/    Static/Dynamic/Graph pipelines, channels, actions, resilience, windows
  http/        HTTP/1.1 parser, Request/Response, Router, fetch client
  server/      HTTP1/HTTP2/WebSocket/gRPC connection handlers
  crypto/      SHA/HMAC/HKDF/PBKDF2/ChaCha20-Poly1305/AES-GCM/Base64/CSPRNG
  security/    SIMD JWT parser, JwtAuthAction
  net/ io/ shm/  sockets, scatter-gather, buffers, shared memory
  buf/ codec/ middleware/  pools, framing, HTTP middleware, spatial bitsets
  db/ tracing/ config/ compat/  DB interfaces, W3C tracing, config, std::print shim
  qbuem_stack.hpp   umbrella → App / StackController
```
