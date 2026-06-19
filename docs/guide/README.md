# qbuem-stack — Feature Guide

The complete, detailed reference for qbuem-stack. Every feature is documented
with **what it is · when to use it · how to use it (real code) · gotchas**, and
linked to a runnable example under [`examples/`](../../examples/).

All APIs and code snippets are grounded in the actual headers under
`include/qbuem/`. Supported platforms: **Linux x86_64 · ARM64 boards · macOS aarch64**.
Zero third-party dependencies; errors are returned as `std::expected`, never thrown.

---

## Table of contents

| # | Section | Covers |
|---|---|---|
| 01 | **[Getting Started](./01-getting-started.md)** | Build, install, the 4 pillars, the reactor+coroutine model, feature map |
| 02 | **[Core & Async Runtime](./02-core-and-async.md)** | `Task<T>` coroutines, `Reactor` (epoll/io_uring/kqueue), `Dispatcher`, awaiters, `Arena` + `FixedPoolResource`, `TimerWheel`, `AsyncLogger`, `cpu_hints`, `MicroTicker`, `TickLoop`/`TickScheduler` (fixed-timestep ticking), NUMA, `ISessionStore`/`ITransport` |
| 03 | **[Pipeline System](./03-pipeline.md)** | `StaticPipeline`, `DynamicPipeline` (hot-swap), `PipelineGraph` (DAG), channels (async/spsc/arena/priority), `RetryAction`/`CircuitBreaker`/`DeadLetterQueue`, batching, windows, `MessageBus`, backpressure, saga/canary/checkpoint/SLO |
| 04 | **[HTTP & Web Server](./04-http-and-server.md)** | `App`/`StackController`, `Router`, `Request`/`Response`, middleware, the SIMD HTTP parser, the curl-free `fetch` client, HTTP/2 + WebSocket + gRPC handlers, `template_engine` |
| 05 | **[Cryptography & Security](./05-crypto-and-security.md)** | SHA-256/512, HMAC, HKDF, PBKDF2, ChaCha20-Poly1305, AES-GCM, Base64, CSPRNG, `secure_zero`, SIMD JWT parser, `JwtAuthAction` |
| 06 | **[Networking, I/O & Shared Memory](./06-net-io-shm.md)** | TCP/UDP/Unix sockets, UDS FD passing, DNS, UDP multicast/`recvmmsg`, `IOVec<N>` + `scattered_span` scatter-gather, buffers, async/direct file, `sendfile`, `SHMChannel<T>`, `SHMBus`, futex sync |
| 07 | **[Buffers, Codecs & Middleware](./07-buffers-codecs-middleware.md)** | `GenerationPool`, `lock_free_hash_map`, `inplace_function`, spatial bitsets (`GridBitset`/`TiledBitset`), `simd_erasure`, frame/length-prefix/line codecs, CORS/rate-limit/security/SSE/auth middleware |
| 08 | **[DB Abstraction, Tracing & Config](./08-db-tracing-config.md)** | `Value`, `IConnection`/`IConnectionPool`, `connection_pool`, `SmartCache`, W3C `TraceContext`, `SpanExporter`, samplers, lifecycle tracer, trace logger, `ConfigManager` + `Secret<T>`, `std::print` shim |

---

## Quick links

- **[Usage guide](../usage-guide.md)** — shorter, task-oriented quick reference.
- **[Examples](../../examples/)** — 58 runnable programs in 11 categories.
- **[Codebase audit & cleanup record](../audit/)** — the over-engineering review and what was trimmed.
- **Root [`README.md`](../../README.md)** — overview, benchmarks, feature matrix.

## Reading order

New to the library? Read **01 → 02 → 04** (or **03** if you're building data
pipelines), then dip into the rest as needed. The flagship end-to-end example is
[`examples/06-ipc-messaging/ipc_pipeline`](../../examples/06-ipc-messaging/ipc_pipeline/).
