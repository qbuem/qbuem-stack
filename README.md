# qbuem-stack

**Zero Latency · Zero Copy · Zero Allocation · Zero Dependency**

> **v1.6.0** — A focused, zero-dependency C++23 infrastructure library for
> Web Application Servers (WAS), Inter-Process Communication (IPC), real-time
> simulation, and data pipelines.
>
> **Supported platforms:** Linux x86_64 (`io_uring`/`epoll`) · Linux ARM64
> (Jetson-class) · macOS aarch64 (`kqueue`). POSIX only — Windows is not
> supported. No third-party dependencies — C++23 stdlib + OS/arch intrinsics only.

> 📚 **Start here:** [Getting Started](./docs/guide/01-getting-started.md) ·
> **Full feature guide:** [`docs/guide/`](./docs/guide/) (per-module: role · when to use ·
> how to use · gotchas, grounded in the real API) ·
> **Runnable examples:** [`examples/`](./examples/) (61 programs) ·
> **What's stable vs experimental:** [Feature maturity](#feature-maturity) ·
> [CHANGELOG](./CHANGELOG.md) · [Roadmap](./docs/roadmap.md).

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

## Feature maturity

Honest status of every module, so you know what to build on. **Stable** = used in
production/dogfooding, well-tested (incl. sanitizers), API unlikely to change.
**Experimental** = works and tested but the API may evolve. **Codec-only** = the
wire format / state machine is implemented and tested, but there is **no wired
server transport** — usable as a building block, not a turnkey server yet.

| Module | Status | Notes |
| :--- | :--- | :--- |
| `core` — `Task`, reactor (epoll/io_uring/kqueue), `Dispatcher`, awaiters, `OffloadPool` | **Stable** | the runtime; thread-per-core, shared-nothing |
| `core` — `Arena`, `FixedPoolResource`, `TimerWheel`, `MicroTicker`, `cpu_hints`, NUMA | **Stable** | zero-alloc memory + timing |
| `core` — `TickLoop`, `TickScheduler` | **Stable** | fixed-timestep ticking; deterministic, TSan/ASan/UBSan-verified (v1.5.0) |
| `http` + HTTP/1.1 `App` server | **Stable** | SIMD parser, router, middleware, keep-alive, `serve_static`, `sendfile`, health/metrics |
| `http` — `fetch` client (HTTP) | **Stable** | curl-free; HTTP only (no TLS — see below) |
| `server` — `WsServer` / `WsConnection` (WebSocket) | **Stable** | non-blocking, rooms, broadcast, backpressure, heartbeat (v1.1.0). Also via `App::ws("/path", handlers)` — WS on the same port as HTTP (v1.7.0) |
| `pipeline` — Static/Dynamic/Graph, channels, actions, resilience, windows | **Stable** | the data-flow engine |
| `shm` — `SHMChannel`, `SHMBus`, futex sync | **Stable** | zero-copy cross-process IPC |
| `net` — TCP/UDP/Unix sockets, UDS FD passing, DNS | **Stable** | DNS is bounded thread-offload |
| `io` — `IOVec`/`scattered_span`, buffers, `BufferedReader`, async/direct file, `sendfile` | **Stable** | scatter-gather zero-copy |
| `crypto` + `security` — SHA/HMAC/HKDF/PBKDF2/ChaCha20-Poly1305/AES-GCM/Base64/CSPRNG, `secretbox`, HS256 JWT, SIMD JWT parser | **Stable** | misuse-resistant wrappers (v1.2.0); software + opt-in hardware paths |
| `buf` — pools, `GenerationPool`, `inplace_function`, `GridBitset`/`TiledBitset`, `SpatialGrid` | **Stable** | spatial indexes for game/sim/robotics |
| `middleware` — CORS, rate-limit, security headers, SSE, token/JWT auth, body encoder | **Stable** | composable HTTP middleware |
| `tracing` — W3C `TraceContext`, span exporter, sampler, lifecycle tracer | **Stable** | distributed tracing |
| `config` — `ConfigManager`, `Secret<T>` | **Stable** | zero-heap config + redacted secrets |
| `db` — `Value`, `IConnection`/`IConnectionPool`, `connection_pool`, `SmartCache` | **Experimental** | interfaces + pool; **no bundled driver** — bring your own |
| `server` — `Http2Handler` (HTTP/2) | **Codec-only** | HPACK static table + framing; **no flow control, no wired server**; DoS-hardened |
| `server` — `GrpcHandler` (gRPC) | **Codec-only** | message framing + dispatch; no HTTP/2 transport wiring |

**Not in this library (and not planned for the zero-dependency core):** TLS,
RDMA, PCIe/VFIO, NVMe passthrough, eBPF, AF_XDP, Windows. Any doc you find
referencing these is stale — they are out of scope (they require third-party
libraries or special hardware, which breaks the zero-dependency contract).

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

    // WebSocket on the same port — no upgrade plumbing (v1.7+)
    app.ws("/ws", {
        .on_open    = [](auto conn) { conn->send_text("welcome"); },
        .on_message = [](auto conn, qbuem::WsMessage m) { conn->send_text(m.text()); },
        .on_close   = [](auto conn, uint16_t code) {},
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
| `QBUEM_ENABLE_LTO` | OFF | Interprocedural optimization (LTO) for library targets |
| `QBUEM_ENABLE_NATIVE_CRYPTO` | OFF | Compile the crypto module with the host CPU's hardware SHA-2/AES instructions (host-targeting build; ~11× faster SHA-256/HMAC). Requires that CPU to support them. |
| `QBUEM_JSON_TAG` | `"main"` | optional `qbuem-json` tag (only enables JSON examples) |

---

## Layered Architecture (9 Levels)

| Level | Modules | Key Types |
|---|---|---|
| 1 Foundation | `core/arena`, `crypto` | `Arena`, `FixedPoolResource`, `Result<T>` |
| 2 Async Core | `core/task`, `core/reactor`, `core/dispatcher`, `core/tick_loop` | `Task<T>`, `Reactor`, `Dispatcher`, `TickLoop`, `TickScheduler` |
| 3 IO Primitives | `net`, `io`, `shm`, `buf` | `TcpStream`, `IOVec<N>`, `scattered_span`, `SHMChannel<T>` |
| 4 Transport / Codec | `transport`, `codec` | `PlainTransport`, `LengthPrefixCodec`, `LineCodec` |
| 5 Web / HTTP | `http`, `server` | `Request`, `Response`, `Router`, `App` |
| 6 Pipeline | `pipeline` | `StaticPipeline`, `DynamicPipeline`, `PipelineGraph`, `AsyncChannel<T>` |
| 7 Resilience / Ext | `pipeline/resilience`, `db`, `security` | `RetryAction`, `CircuitBreaker`, `IConnectionPool` |
| 8 Protocols | `server/http2`, `server/websocket`, `server/ws_server`, `server/grpc` | `Http2Handler`, `WebSocketHandler`, `WsServer`, `GrpcHandler` |
| 9 Umbrella | `qbuem_stack.hpp` | `App`, `StackController` |

---

## Module Catalog — what's here, how to use it, where to look

Each row links to the **detailed guide** (role · when to use · how to use · gotchas) and a
**runnable example**. Headers live under [`include/qbuem/`](./include/qbuem/).

| Module(s) | What it does | Key types / entry points | Guide | Examples |
|---|---|---|---|---|
| **`core`** | Async runtime: C++23 coroutines, per-core reactor (epoll/io_uring/kqueue), multi-core dispatcher with graceful `drain()`, blocking/CPU-bound offload pool, zero-alloc memory, timers, precise fixed-timestep ticking | `Task<T>` · `Reactor` · `Dispatcher` · `OffloadPool` · `Arena` · `FixedPoolResource` · `TimerWheel` · `MicroTicker` · `TickLoop` (drift-free + catch-up + 0-alloc metrics) · `TickScheduler` (multi-rate ordered systems + deterministic (seed,tick) RNG + pause/time-scale/step + per-system metrics) | [02 — Core & Async](./docs/guide/02-core-and-async.md) | [01-foundation](./examples/01-foundation/), [03-memory](./examples/03-memory/) |
| **`pipeline`** | Build & run processing graphs (ffmpeg-style): static / dynamic (hot-swap) / DAG (split+merge), channels, batching, windows, resilience | `PipelineBuilder` · `StaticPipeline` · `DynamicPipeline` · `PipelineGraph` · `RetryAction` · `CircuitBreaker` · `DeadLetterQueue` | [03 — Pipeline](./docs/guide/03-pipeline.md) | [05-pipeline](./examples/05-pipeline/), [07-resilience](./examples/07-resilience/) |
| **`http` + `server`** | HTTP/1.1 SIMD parser, router, `App` web server, curl-free fetch client; HTTP/2 + gRPC handlers; low-level `WebSocketHandler` codec **and** high-level non-blocking `WsServer` (rooms, broadcast, back-pressure, heartbeat — for high-concurrency game/realtime servers) | `App` · `Request` · `Response` · `Router` · `fetch()` · `Http1Handler` · `WebSocketHandler` · `WsServer` | [04 — HTTP & Server](./docs/guide/04-http-and-server.md) | [02-network](./examples/02-network/) |
| **`crypto` + `security`** | SHA-256/512, HMAC, HKDF, PBKDF2, ChaCha20-Poly1305, AES-GCM, Base64, CSPRNG; SIMD JWT; **misuse-resistant helpers**: `seal_easy`/`open_easy` (random-nonce AEAD), `password_hash`/`verify_password`, HS256 `encode_jwt_hs256`/`verify_jwt_hs256` + `HmacJwtVerifier` | `sha256` · `hmac` · `aes_gcm` · `chacha20_poly1305` · `seal_easy` · `password_hash` · `encode_jwt_hs256` · `SIMDJwtParser` | [05 — Crypto & Security](./docs/guide/05-crypto-and-security.md) | [04-codec-security](./examples/04-codec-security/) |
| **`net` + `io` + `shm`** | TCP/UDP/Unix sockets, UDS FD passing, bounded DNS; zero-copy scatter-gather + buffers + files; buffered line reading; shared-memory IPC | `TcpStream` (`write_all`/`read_exact`) · `TcpListener` · `UdpSocket` · `BufferedReader` · `IOVec<N>` · `scattered_span` · `SHMChannel<T>` · `SHMBus` | [06 — Net, I/O & SHM](./docs/guide/06-net-io-shm.md) | [02-network](./examples/02-network/), [06-ipc-messaging](./examples/06-ipc-messaging/) |
| **`buf` + `codec` + `middleware`** | Zero-alloc pools & `inplace_function`, spatial bitsets, erasure coding; frame/line/length codecs; HTTP middleware | `GenerationPool` · `inplace_function` · `GridBitset` · `LineCodec` · `LengthPrefixedCodec` · `cors`/`rate_limit`/`token_auth` | [07 — Buffers, Codecs & Middleware](./docs/guide/07-buffers-codecs-middleware.md) | [03-memory](./examples/03-memory/), [04-codec-security](./examples/04-codec-security/), [11-advanced-apps](./examples/11-advanced-apps/) |
| **`db` + `tracing` + `config`** | DB connection/cache interfaces; W3C distributed tracing; zero-alloc config + `Secret<T>` | `IConnectionPool` · `SmartCache` · `TraceContext` · `SpanExporter` · `ConfigManager` · `Secret<T>` | [08 — DB, Tracing & Config](./docs/guide/08-db-tracing-config.md) | [09-database](./examples/09-database/), [08-observability](./examples/08-observability/) |

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

The [`examples/`](./examples/) directory contains **61 registered programs** in 11 categories.

| # | Category | Programs | Highlights |
| :--- | :--- | :---: | :--- |
| [01](./examples/01-foundation/) | Foundation | 5 | `hello_world`, `async_timer`, `micro_ticker`, **`tick_loop`** (drift-free fixed-timestep), `config` |
| [02](./examples/02-network/) | Network | 8 | TCP echo, UDP advanced, Unix socket, WebSocket codec, **`ws_game_server`** (high-level rooms/broadcast), HTTP fetch, HTTP/2 + gRPC *(codec demos — see maturity)* |
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

- **[Getting Started](./docs/guide/01-getting-started.md)** — build, install, the model, first server.
- **[Feature guide](./docs/guide/)** (`01`–`08`) — detailed per-module reference: role · when to use · how to use · gotchas, grounded in the real API. **The main docs.**
- **[Usage guide](./docs/usage-guide.md)** — task-oriented quick reference.
- **[Best practices](./docs/best-practices.md)** — pattern selection (which primitive for which job).
- **[CHANGELOG](./CHANGELOG.md)** — per-release notes (real, tagged releases only).
- **[Roadmap](./docs/roadmap.md)** — current state + where this is going.
- **Deep dives** under `docs/` — pipeline, SHM messaging, crypto, DB, config, I/O architecture, and per-platform optimization (kqueue/epoll/NEON).

Every feature documented here exists in the code at this version. See
[Feature maturity](#feature-maturity) for what's stable vs experimental.

---

*qbuem-stack — built for mechanical sympathy, kept honest and dependency-free.*
