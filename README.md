# qbuem-stack

**Zero Latency · Zero Copy · Zero Allocation · Zero Dependency**

> **v3.4.0** — A focused, zero-dependency C++23 infrastructure core for
> Web Application Servers (WAS), Inter-Process Communication (IPC), and data pipelines.
>
> **Supported platforms:** Linux x86_64 · ARM64 boards (Jetson-class) · macOS aarch64.
> No exotic-hardware or third-party dependencies — stdlib + OS/arch intrinsics only.

> 📚 **Full feature guide:** [`docs/guide/`](./docs/guide/) — detailed per-module docs
> (role · when to use · how to use · gotchas), grounded in the real API.

---

## Core Principles

| Principle | Implementation Strategy |
| :--- | :--- |
| **Zero Latency** | SIMD HTTP parser · `io_uring`/`epoll`/`kqueue` batching · `sendfile(2)` · async logger |
| **Zero Copy** | `std::span`/`string_view` buffers · scatter-gather `IOVec<N>` + `scattered_span` → `writev` |
| **Zero Allocation** | Per-request `Arena` · `FixedPoolResource` event pools · lock-free SPSC ring buffers |
| **Zero Dependency** | C++23 stdlib + OS/arch intrinsics only · no third-party headers in `include/` |
| **Shared-Nothing** | Thread-per-core reactor · `SO_REUSEPORT` · NUMA-aware pinning · minimal cross-thread sync |

Errors are returned as values (`std::expected`), never thrown — exceptions are forbidden on hot paths.

---

## Quick Start

```cpp
#include <qbuem/qbuem_stack.hpp>

int main() {
    qbuem::App app;

    // Synchronous handler
    app.get("/hello", [](const qbuem::Request& req, qbuem::Response& res) {
        res.status(200).body("Zero allocation hello");
    });

    // C++23 coroutine async handler
    app.get("/api/v1/user/:id", [](const qbuem::Request& req, qbuem::Response& res)
        -> qbuem::Task<void> {
        auto id = req.param("id");
        res.status(200).body("{\"id\":\"" + std::string(id) + "\"}");
        co_return;
    });

    return app.listen(8080) ? 0 : 1;
}
```

### Pipeline + IPC Integration

```cpp
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/shm/shm_bus.hpp>

using namespace qbuem;
using namespace qbuem::shm;

// SHMChannel → SHMSource → Pipeline → MessageBusSink → MessageBus
auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("trading.raw_orders"))      // SHM input
    .add<ParsedOrder>(stage_parse)
    .add<ValidatedOrder>(stage_validate)
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "validated")) // MessageBus output
    .build();

pipeline.start(dispatcher);
```

See [`examples/06-ipc-messaging/ipc_pipeline`](./examples/06-ipc-messaging/ipc_pipeline/) for the full walkthrough.

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cd build && ctest --output-on-failure
```

Requires a C++23 compiler (GCC 13+, Clang 17+). On Linux, `liburing` is used for the
io_uring reactor when present and **falls back to epoll automatically** when absent —
there is no hard dependency. macOS uses kqueue.

| CMake option | Default | Description |
|---|---|---|
| `QBUEM_BUILD_TESTS` | ON | Build unit tests under `tests/` |
| `QBUEM_BUILD_EXAMPLES` | ON | Build all examples under `examples/` |
| `QBUEM_BUILD_BENCH` | ON | Build benchmarks under `bench/` |
| `QBUEM_JSON_TAG` | `"main"` | optional `qbuem-json` tag (only enables JSON examples) |

---

## Layered Architecture (9 Levels)

| Level | Modules | Key Types |
|---|---|---|
| 1 Foundation | `core/arena`, `crypto` | `Arena`, `FixedPoolResource`, `Result<T>` |
| 2 Async Core | `core/task`, `core/reactor`, `core/dispatcher` | `Task<T>`, `Reactor`, `Dispatcher` |
| 3 IO Primitives | `net`, `io`, `shm`, `buf` | `TcpStream`, `IOVec<N>`, `scattered_span`, `SHMChannel<T>` |
| 4 Transport / Codec | `transport`, `codec` | `PlainTransport`, `LengthPrefixCodec`, `LineCodec` |
| 5 Web / HTTP | `http`, `server` | `Request`, `Response`, `Router`, `App` |
| 6 Pipeline | `pipeline` | `StaticPipeline`, `DynamicPipeline`, `PipelineGraph`, `AsyncChannel<T>` |
| 7 Resilience / Ext | `pipeline/resilience`, `db`, `security` | `RetryAction`, `CircuitBreaker`, `IConnectionPool` |
| 8 Protocols | `server/http2`, `server/websocket`, `server/grpc` | `Http2Handler`, `WebSocketHandler`, `GrpcHandler` |
| 9 Umbrella | `qbuem_stack.hpp` | `App`, `StackController` |

---

## Feature Support Matrix

| Category | Features |
| :--- | :--- |
| **Network** | TCP / UDP / Unix sockets, `SO_REUSEPORT`, UDP multicast, `recvmmsg` batching, UDS FD passing, DNS |
| **Async** | `Task<T>` coroutines, multi-core `Dispatcher`, epoll / io_uring / kqueue reactors, `TimerWheel` |
| **HTTP** | SIMD HTTP/1.1 parser, router, static files, curl-free fetch client (HTTP), HTTP/2 + WebSocket + gRPC server handlers |
| **Pipeline** | Static / Dynamic / Graph pipelines, hot-swap, MessageBus, SHM bridge, batching, windows, backpressure |
| **IPC** | `SHMChannel<T>`, `SHMBus`, `SHMSource`/`SHMSink`, `MessageBusSource`/`Sink`, futex sync |
| **Resilience** | Retry, CircuitBreaker, DeadLetterQueue, Saga, Canary, Checkpoint, SLO tracking |
| **Crypto** | SHA-256/512, HMAC, HKDF, PBKDF2, ChaCha20-Poly1305, AES-GCM, Base64, CSPRNG (zero-dep, SIMD-accelerated) |
| **Security** | SIMD JWT parser, `JwtAuthAction`, bearer/token auth middleware, security headers |
| **Memory** | `Arena`, `FixedPoolResource`, `GenerationPool`, `inplace_function`, spatial bitsets, SIMD erasure coding |
| **Observability** | W3C TraceContext, Span exporters (OTLP), samplers, in-process lifecycle tracer, trace-correlated logger |
| **Config** | Zero-allocation `ConfigManager` + `Secret<T>` |

> **Not in scope (by design):** TLS/HTTPS (needs a third-party crypto lib), RDMA / AF_XDP / NVMe-passthrough /
> eBPF / PCIe-VFIO (need exotic hardware or third-party libraries). Terminate TLS at a reverse proxy, or wrap a
> TLS library at the application layer. These were removed to keep the core honest and dependency-free.

---

## Performance Benchmarks

> Single-threaded micro-benchmarks, `-O3 -march=native`. Reproduce with `bench/`.
> Throughput figures are component micro-benchmarks, not end-to-end server RPS.

| Component | Operation | Result |
| :--- | :--- | :--- |
| **Arena** | 64 B bump-alloc | **2.6 ns** / alloc |
| **Arena** | request lifecycle (10 alloc + reset) | **1.3 ns** / req |
| **FixedPool** | alloc + dealloc (free-list) | **1.2 ns** / round-trip |
| **AsyncChannel** | try_send + try_recv (MPMC) | **47M ops/s** |
| **SpscChannel** | try_send + try_recv (wait-free) | **113M ops/s** |
| **SpscChannel** | batch fill 1000 + drain 1000 | **271M ops/s** (~1 GB/s) |
| **HTTP Parser** | GET (74 B) | **320 MB/s** |
| **HTTP Parser** | POST + 10 headers (310 B) | **303 MB/s** |
| **Router** | static route lookup | **120 ns** |
| **Router** | param route (`/users/:id`) | **147 ns** |
| **IOVec / IOSlice** | create + `to_iovec()` | **0.3 ns** |
| **SHMChannel** | inter-process latency | **< 150 ns** |
| **inplace_function** | call vs `std::function` | **~8.7×** faster, 0 allocs |

---

## Spatial Bitsets — `GridBitset` & `TiledBitset`

Zero-allocation, wait-free spatial indexes for fixed-size and infinite worlds
(game / simulation / robotics / GIS). Reads are wait-free; writes are lock-free
via `fetch_or`/`fetch_and`/`fetch_xor`.

| Type | Mode | Use case |
| :--- | :--- | :--- |
| `GridBitset<W,H,D>` (D=1) | 2D flag | walkability map |
| `GridBitset<W,H,D>` (D≤64) | 2.5D layers | multi-floor / voxel chunk |
| `GridBitset2D<W,H>` | 2D Morton (8×8 super-blocks) | tile map, collision, FOV |
| `TiledBitset<TW,TH,D>` | infinite tiles (`int64_t` coords) | open-world, GIS, robotics |

```cpp
GridBitset<256, 256, 32> world;
world.set(x, y, layer);                       // lock-free write
world.any_in_box(x1, y1, x2, y2, 0, 15);      // broad-phase overlap (~28 ns)
auto hit = world.raycast(0, 0, 1, 0, 0, 15, 128);  // DDA line-of-sight
world.for_each_set_in_radius(cx, cy, r, 0, 15, fn); // per-row sqrt + AVX2/NEON

TiledBitset<256, 256, 16> open_world;         // infinite dynamic world
open_world.set(wx, wy, layer);                // allocates tile on demand
size_t freed = open_world.evict_empty_tiles();
```

See `examples/11-advanced-apps/open_world/` and `spatial_fusion/`.

---

## Examples

The [`examples/`](./examples/) directory contains **58 registered programs** in 11 categories.

| # | Category | Programs | Highlights |
| :--- | :--- | :---: | :--- |
| [01](./examples/01-foundation/) | Foundation | 4 | `hello_world`, `async_timer`, `micro_ticker`, `config` |
| [02](./examples/02-network/) | Network | 7 | TCP echo, UDP advanced, Unix socket, WebSocket, HTTP fetch, HTTP/2 server, gRPC |
| [03](./examples/03-memory/) | Memory | 4 | Arena, zero-copy arena channel, NUMA + huge pages, lock-free bench |
| [04](./examples/04-codec-security/) | Codec & Security | 6 | Codecs, crypto URL, security middleware, **crypto primitives**, transport codec/plain |
| [05](./examples/05-pipeline/) | Pipeline | 12 | Fan-out, hot-swap, batching, dynamic router, backpressure, stateful window, windowed action |
| [06](./examples/06-ipc-messaging/) | IPC & Messaging | 5 | SHMChannel, **flagship IPC pipeline**, MessageBus, SPSC, scatter-send |
| [07](./examples/07-resilience/) | Resilience | 6 | Retry + CircuitBreaker + DLQ, Saga, Canary, Checkpoint, SLO |
| [08](./examples/08-observability/) | Observability | 3 | W3C tracing, TimerWheel, TaskGroup |
| [09](./examples/09-database/) | Database | 3 | Connection pool / session, coroutine JSON, SmartCache |
| [10](./examples/10-hardware/) | Hardware | 1 | kqueue sophistication (macOS) |
| [11](./examples/11-advanced-apps/) | Advanced Apps | 9 | Autonomous driving, HFT matching, open-world spatial, trading platform, game server |

**Recommended learning path:**
```
hello_world → async_timer → tcp_echo_server → arena → pipeline/fanout
    → ipc_pipeline → resilience → trading_platform
```

---

## Documentation

- **[Feature guide](./docs/guide/)** — detailed per-module reference (the main docs).
- **[Usage guide](./docs/usage-guide.md)** — task-oriented quick reference.
- **[Codebase audit](./docs/audit/)** — review + over-engineering cleanup record.
- **Per-platform design notes** under `docs/` (kqueue, epoll, NEON, primitives).

---

*qbuem-stack — built for mechanical sympathy, kept honest and dependency-free.*
