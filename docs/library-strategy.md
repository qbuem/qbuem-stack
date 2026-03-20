# qbuem-stack Library Separation Strategy

> **Principle**: Function-unit independent libraries · Unidirectional dependencies · Zero external dependency
>
> Related documents:
> - IO Architecture: [docs/io-architecture.md](./io-architecture.md)
> - IO Technology Deep Dive: [docs/io-deep-dive.md](./io-deep-dive.md)
> - Pipeline Design: [docs/pipeline-design.md](./pipeline-design.md)

---

## 1. Design Principles

| Principle | Description |
|------|------|
| **Single responsibility** | One library = one clear function |
| **Unidirectional dependency** | Upper layers depend on lower. No reverse. |
| **Zero external dep** | No external library exposure in any library header |
| **Independent consumption** | Embedded server uses only `qbuem::reactor`; HTTP server only `qbuem::http-server` |
| **Header-only first** | template/concept-heavy libraries are exposed as header-only |
| **Monorepo** | One git repository, multiple CMake targets |

---

## 2. Complete Library List (9 levels)

```
Level 0  ─ Foundation   : result · arena · crypto
Level 1  ─ Async Core   : task · reactor · dispatcher
Level 2  ─ Reactor Impl : epoll · kqueue · iouring
Level 3  ─ IO           : net · buf · file · zerocopy · **shm** (IPC Messaging)
Level 4  ─ Transport    : transport · codec · server
Level 5  ─ HTTP         : http · http-server
Level 6  ─ Pipeline     : context · channel · pipeline
Level 7  ─ Extensions   : dynamic-pipeline · resilience · tracing · metrics · **db-abstraction** · **security-core**
Level 8  ─ Protocol     : ws · http2 · grpc · **db-postgres** · **db-redis** · **security-ktls**
Level 9  ─ Umbrella     : qbuem (all)
```

---

## 3. Dependency Graph

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Level 9: qbuem (umbrella)                                              │
└────────────────────────┬────────────────────────────────────────────────┘
                         │ depends on all
          ┌──────────────┼──────────────────────────┐
          ▼              ▼                          ▼
┌─────────────┐  ┌──────────────┐  ┌──────────────────────────────────┐
│  Level 8    │  │  Level 7     │  │  Level 5                         │
│  ws         │  │ pipeline-    │  │  http-server                     │
│  http2      │  │  graph       │  │  http                            │
│  grpc       │  │ resilience   │  └──────────────────────────────────┘
└──────┬──────┘  │ tracing      │
       │         │ metrics      │
       │         └──────┬───────┘
       │                │
       ▼                ▼
┌─────────────────────────────┐
│  Level 6: Pipeline          │
│  context  channel  pipeline │
└──────────────┬──────────────┘
               │
       ┌───────┼────────────────────────────┐
       ▼       ▼                            ▼
┌────────┐ ┌──────────────────────────┐  ┌───────────────────────────┐
│ Level 4│ │ Level 4: Transport+Codec │  │ Level 5: http             │
│ server │ │ transport  codec         │  │ (HTTP/1.1 parser+router)  │
└────┬───┘ └──────────┬───────────────┘  └────────────┬──────────────┘
     │                │                               │
     └────────────────┼───────────────────────────────┘
                      │
          ┌───────────┼────────────────┐
          ▼           ▼                ▼
┌──────────────┐  ┌──────┐  ┌──────────────────────┐
│ Level 3: net │  │ buf  │  │ file   zerocopy       │
└──────┬───────┘  └──┬───┘  └──────────────────────┘
       │              │
       └──────┬────────┘
              ▼
┌──────────────────────────────────────────────────┐
│  Level 2: Reactor Impl (platform-specific)       │
│  epoll (Linux)  kqueue (macOS)  iouring (Linux)  │
└──────────────────────────┬───────────────────────┘
                           ▼
┌──────────────────────────────────────────────────┐
│  Level 1: Async Core                             │
│  reactor (interface)  dispatcher  task           │
└──────────────────────────┬───────────────────────┘
                           ▼
┌──────────────────────────────────────────────────┐
│  Level 0: Foundation (header-only)               │
│  result            arena         crypto          │
└──────────────────────────────────────────────────┘
```

---

## 4. Per-Library Details

### Level 0 — Foundation

#### `qbuem::result` (header-only)
```
Headers: include/qbuem/result.hpp  (currently split from common.hpp)
Depends on: none
Contents: Result<T>, unexpected<E>, errc enum, ok(), err()
Purpose: Common error type for all libraries
```

#### `qbuem::arena` (header-only)
```
Headers: include/qbuem/arena.hpp  (currently core/arena.hpp → to be moved)
Depends on: none
Contents: Arena, FixedPoolResource<N,A>, BufferPool<Size,Count>
Purpose: zero-alloc memory foundation for all layers
```

#### `qbuem::crypto` (static lib)
```
Headers: include/qbuem/crypto.hpp
Sources: src/crypto/crypto.cpp
Depends on: result (+ POSIX getrandom/arc4random)
Contents: constant_time_equal(), csrf_token(), random_bytes()
Purpose: Timing-safe comparison, CSPRNG. No external OpenSSL.
```

---

### Level 1 — Async Core

#### `qbuem::task` (header-only)
```
Headers: include/qbuem/task.hpp, include/qbuem/awaiters.hpp
Depends on: result
Contents: Task<T>, promise_type, symmetric transfer, AsyncRead/Write awaiters
Purpose: Foundation for all coroutine-based async operations
```

#### `qbuem::reactor` (static lib — interface + TimerWheel)
```
Headers: include/qbuem/reactor/reactor.hpp
         include/qbuem/reactor/timer_wheel.hpp
Sources: src/reactor/reactor.cpp (current() thread_local)
         src/reactor/timer_wheel.cpp
Depends on: result, task
Contents: Reactor abstract interface, EventType, TimerWheel O(1)
Purpose: Platform-independent event loop abstraction
Note: No concrete implementation — must link with one of epoll/kqueue/iouring
```

#### `qbuem::dispatcher` (static lib)
```
Headers: include/qbuem/reactor/dispatcher.hpp
Sources: src/reactor/dispatcher.cpp
Depends on: reactor
Contents: Dispatcher, thread-per-core, post(), spawn(), CPU affinity
Purpose: Coordinates multiple Reactors, manages Worker threads
```

---

### Level 2 — Reactor Implementations

#### `qbuem::epoll` (static lib, Linux only)
```
Headers: include/qbuem/reactor/epoll_reactor.hpp
Sources: src/reactor/epoll_reactor.cpp
Depends on: reactor
Contents: EpollReactor (eventfd wakeup, level-trigger)
Platform: Linux
```

#### `qbuem::kqueue` (static lib, macOS/BSD only)
```
Headers: include/qbuem/reactor/kqueue_reactor.hpp
Sources: src/reactor/kqueue_reactor.cpp
Depends on: reactor
Contents: KqueueReactor (EVFILT_USER wakeup)
Platform: macOS, FreeBSD
```

#### `qbuem::iouring` (static lib, Linux 5.1+)
```
Headers: include/qbuem/reactor/io_uring_reactor.hpp
Sources: src/reactor/io_uring_reactor.cpp
Depends on: reactor (liburing — pimpl internal, not exposed in headers)
Contents: IOUringReactor (SQPOLL, Fixed Buffers, Buffer Ring)
          Advanced: multishot accept/recv, linked SQEs, fixed files
Platform: Linux 5.1+
Option:   QBUEM_IOURING=ON/OFF (epoll selected automatically if unavailable)
```

**CMake auto-selection logic:**
```cmake
if(APPLE)
    set(QBUEM_DEFAULT_REACTOR kqueue)
elseif(LINUX)
    if(liburing FOUND AND kernel >= 5.1)
        set(QBUEM_DEFAULT_REACTOR iouring)
    else()
        set(QBUEM_DEFAULT_REACTOR epoll)
    endif()
endif()
```

---

### Level 3 — IO Primitives

#### `qbuem::net` (static lib)
```
Headers: include/qbuem/net/socket_addr.hpp
         include/qbuem/net/tcp_listener.hpp
         include/qbuem/net/tcp_stream.hpp
         include/qbuem/net/udp_socket.hpp
         include/qbuem/net/unix_socket.hpp
Sources: src/net/*.cpp
Depends on: reactor, result, task
Contents: SocketAddr(IPv4/IPv6/Unix), TcpListener(SO_REUSEPORT),
          TcpStream(readv/writev), UdpSocket(sendmsg/recvmsg),
          UnixSocket(stream/dgram)
Purpose: Foundation for all network I/O. No TLS (injected via transport).
```

#### `qbuem::buf` (header-only)
```
Headers: include/qbuem/buf/io_slice.hpp
         include/qbuem/buf/read_buf.hpp
         include/qbuem/buf/write_buf.hpp
Depends on: arena
Contents: IOSlice, IOVec<N>, ReadBuf<N>(ring buffer), WriteBuf(cork)
Purpose: Allocation-free scatter-gather IO buffer abstraction
```

#### `qbuem::file` (static lib)
```
Headers: include/qbuem/io/async_file.hpp
Sources: src/io/async_file.cpp
Depends on: reactor, buf
Contents: AsyncFile (prefers io_uring READ/WRITE_FIXED, falls back to pread)
          O_DIRECT support, aligned buffer validation
```

#### `qbuem::zerocopy` (static lib, Linux)
```
Headers: include/qbuem/io/zero_copy.hpp
Sources: src/io/zero_copy.cpp
Depends on: net, reactor
Contents: sendfile(2), splice(2), MSG_ZEROCOPY + wait
          kTLS TX offload detection and integration (Linux 4.13+)
Platform: Linux (macOS: sendfile header present but functionality limited)
```

---

### Level 4 — Transport + Codec + Server

#### `qbuem::transport` (header-only + small impl)
```
Headers: include/qbuem/transport/itransport.hpp
         include/qbuem/transport/plain_transport.hpp
Sources: src/transport/plain_transport.cpp
Depends on: net, task, result
Contents: ITransport (abstract), PlainTransport (concrete TCP implementation)
Injection point: service injects OpenSSL/mbedTLS implementation as ITransport
                 kTLS: implementation that calls setsockopt(SOL_TLS) after handshake is also possible
```

#### `qbuem::codec` (header-only)
```
Headers: include/qbuem/codec/frame_codec.hpp
         include/qbuem/codec/length_prefix_codec.hpp
         include/qbuem/codec/line_codec.hpp
Depends on: buf, result
Contents: IFrameCodec<Frame>, LengthPrefixedCodec<H>, LineCodec
Purpose: Frame boundary parsing. No allocation — all operations on IOSlice/ReadBuf.
```

#### `qbuem::server` (static lib)
```
Headers: include/qbuem/server/accept_loop.hpp
         include/qbuem/server/iconnection_handler.hpp
         include/qbuem/server/connection_pool.hpp
Sources: src/server/*.cpp
Depends on: net, transport, codec, dispatcher, arena
Contents: AcceptLoop<Frame,HandlerFactory> (SO_REUSEPORT, coroutine)
          IConnectionHandler<Frame>
          ConnectionPool<T> (min_idle, max_size, health_check)
Purpose: Server connection accept/management infrastructure. Protocol-agnostic.
```

---

### Level 5 — HTTP

#### `qbuem::http` (static lib)
```
Headers: include/qbuem/http/parser.hpp
         include/qbuem/http/request.hpp
         include/qbuem/http/response.hpp
         include/qbuem/http/router.hpp
Sources: src/http/*.cpp
Depends on: buf, codec, result
Contents: SIMD HTTP/1.1 FSM parser (AVX2/SSE2/NEON/scalar),
          Request/Response value types, Radix Tree router
Purpose: HTTP layer. No App/middleware — provided by http-server.
```

#### `qbuem::http-server` (static lib)
```
Headers: include/qbuem/http/http1_handler.hpp
         include/qbuem/http/http_server.hpp
Sources: src/http/http1_handler.cpp
Depends on: http, server, transport, arena
Contents: Http1Handler (IConnectionHandler<http::Request> implementation)
          keep-alive, chunked, 100-continue, Upgrade handling
          App (listen/middleware/lifecycle) — currently in qbuem_stack.cpp
Purpose: Actual HTTP server runtime. Includes middleware chain.
```

---

### Level 6 — Pipeline

#### `qbuem::context` (header-only)
```
Headers: include/qbuem/pipeline/context.hpp
         include/qbuem/pipeline/service_registry.hpp
         include/qbuem/pipeline/action_env.hpp
         include/qbuem/pipeline/concepts.hpp
Depends on: task, result
Contents: Context (immutable persistent list), ServiceRegistry (DI),
          ActionEnv, WorkerLocal<T>, ContextualItem<T>
          C++23 Concepts: ActionFn, BatchActionFn, PipelineInputFor
```

#### `qbuem::channel` (header-only)
```
Headers: include/qbuem/pipeline/async_channel.hpp
         include/qbuem/pipeline/stream.hpp
         include/qbuem/pipeline/task_group.hpp
Depends on: task, reactor, context
Contents: AsyncChannel<T> (Vyukov MPMC), Stream<T>, TaskGroup
          cross-reactor wakeup via Reactor::post()
```

#### `qbuem::pipeline` (header-only)
```
Headers: include/qbuem/pipeline/action.hpp
         include/qbuem/pipeline/static_pipeline.hpp
         include/qbuem/pipeline/pipeline.hpp  ← umbrella include
Depends on: channel, context, dispatcher
Contents: Action<In,Out>, StaticPipeline<In,Out>, PipelineBuilder
```

---

### Level 7 — Pipeline Extensions

#### `qbuem::pipeline-graph` (header-only)
```
Headers: include/qbuem/pipeline/dynamic_pipeline.hpp
         include/qbuem/pipeline/pipeline_graph.hpp
         include/qbuem/pipeline/message_bus.hpp
Depends on: pipeline
Contents: IDynamicAction, DynamicPipeline (hot-swap, insert/remove)
          PipelineGraph (DAG, Kahn's algo, fan-out, A/B route)
          MessageBus (Unary/ServerStream/ClientStream/Bidi)
```

#### `qbuem::resilience` (header-only)
```
Headers: include/qbuem/pipeline/retry_policy.hpp
         include/qbuem/pipeline/circuit_breaker.hpp
         include/qbuem/pipeline/dead_letter.hpp
Depends on: pipeline, context
Contents: RetryPolicy (Fixed/Exp/Jitter), CircuitBreaker (3-state),
          DeadLetter<T>, DeadLetterQueue
```

#### `qbuem::tracing` (header-only)
```
Headers: include/qbuem/tracing/trace_context.hpp
         include/qbuem/tracing/span_exporter.hpp
         include/qbuem/tracing/pipeline_tracer.hpp
Depends on: context
Contents: TraceContext (W3C), Sampler interface, SpanExporter interface
          PipelineTracer, LoggingExporter, NoopExporter
Injection point: OtlpGrpcExporter/ZipkinExporter etc. implemented by service
```

#### `qbuem::metrics` (header-only)
```
Headers: include/qbuem/metrics/action_metrics.hpp
         include/qbuem/metrics/pipeline_observer.hpp
Depends on: pipeline, context
Contents: ActionMetrics, PipelineMetrics, PipelineObserver hooks
          LoggingObserver (default implementation)
Injection point: Prometheus push implemented by service via IMetricsExporter
```

---

### Level 8 — Protocol Handlers

#### `qbuem::ws` (static lib)
```
Headers: include/qbuem/protocol/websocket_handler.hpp
Sources: src/protocol/websocket_handler.cpp
Depends on: http-server, server, buf
Contents: WebSocketHandler (RFC 6455)
          HTTP Upgrade, masking/unmasking, PING/PONG, CLOSE handshake
```

#### `qbuem::http2` (static lib)
```
Headers: include/qbuem/protocol/http2_handler.hpp
Sources: src/protocol/http2_handler.cpp, src/protocol/hpack.cpp
Depends on: http-server, server, buf, arena
Contents: Http2Handler, HPACK encoder/decoder (arena-based, zero-alloc),
          stream multiplexing, SETTINGS/WINDOW_UPDATE/PING/GOAWAY
External deps: none (nghttp2 not used — self-implemented)
```

#### `qbuem::grpc` (static lib)
```
Headers: include/qbuem/protocol/grpc_handler.hpp
Sources: src/protocol/grpc_handler.cpp
Depends on: http2, pipeline, channel
Contents: GrpcHandler<Req,Res> (Unary/Server/Client/Bidi)
          Stream<Res> / AsyncChannel<Req> direct connection
Injection point: protobuf serialization injected by service (template parameter)
```

---

### Level 9 — Umbrella

#### `qbuem` (interface lib, links all)
```
Headers: include/qbuem/qbuem-stack.hpp  ← current file retained
Depends on: all layers
Purpose: most services — link just this one to use everything
```

---

## 5. CMake Target Naming Convention

```cmake
# Namespace: qbuem-stack::
# Target name: qbuem_<name>   (internal use)
# Alias:       qbuem-stack::<name>  (consumer use)

# Example
add_library(qbuem_result INTERFACE)
add_library(qbuem-stack::result ALIAS qbuem_result)

add_library(qbuem_net STATIC ...)
add_library(qbuem-stack::net ALIAS qbuem_net)
```

### Consumption Examples (service CMakeLists.txt)

```cmake
# Case 1: full stack (web service)
find_package(qbuem-stack REQUIRED)
target_link_libraries(my-web-service PRIVATE qbuem-stack::qbuem)

# Case 2: HTTP server only (API server)
find_package(qbuem-stack REQUIRED COMPONENTS http-server pipeline)
target_link_libraries(my-api PRIVATE
    qbuem-stack::http-server
    qbuem-stack::pipeline)

# Case 3: IO layer only (game server, embedded)
find_package(qbuem-stack REQUIRED COMPONENTS net buf server)
target_link_libraries(my-game-server PRIVATE
    qbuem-stack::net
    qbuem-stack::buf
    qbuem-stack::server)

# Case 4: Pipeline only (data processing)
find_package(qbuem-stack REQUIRED COMPONENTS pipeline resilience tracing)
target_link_libraries(my-worker PRIVATE
    qbuem-stack::pipeline
    qbuem-stack::resilience
    qbuem-stack::tracing)

# Case 5: Reactor only (embedded IO server)
find_package(qbuem-stack REQUIRED COMPONENTS reactor iouring net)
target_link_libraries(my-embedded PRIVATE
    qbuem-stack::iouring
    qbuem-stack::net)
```

---

## 6. Directory Structure (target state)

```
qbuem-stack/
├── include/qbuem/
│   ├── result.hpp          ← qbuem::result (header-only)
│   ├── arena.hpp           ← qbuem::arena  (header-only)
│   ├── crypto.hpp
│   ├── reactor/
│   │   ├── reactor.hpp     ← qbuem::reactor
│   │   ├── timer_wheel.hpp
│   │   ├── dispatcher.hpp  ← qbuem::dispatcher
│   │   ├── epoll_reactor.hpp
│   │   ├── kqueue_reactor.hpp
│   │   └── io_uring_reactor.hpp
│   ├── net/
│   │   ├── socket_addr.hpp ← qbuem::net
│   │   ├── tcp_listener.hpp
│   │   ├── tcp_stream.hpp
│   │   ├── udp_socket.hpp
│   │   └── unix_socket.hpp
│   ├── buf/
│   │   ├── io_slice.hpp    ← qbuem::buf (header-only)
│   │   ├── read_buf.hpp
│   │   └── write_buf.hpp
│   ├── io/
│   │   ├── async_file.hpp  ← qbuem::file
│   │   └── zero_copy.hpp   ← qbuem::zerocopy
│   ├── transport/
│   │   ├── itransport.hpp  ← qbuem::transport
│   │   └── plain_transport.hpp
│   ├── codec/
│   │   ├── frame_codec.hpp ← qbuem::codec (header-only)
│   │   ├── length_prefix_codec.hpp
│   │   └── line_codec.hpp
│   ├── server/
│   │   ├── accept_loop.hpp ← qbuem::server
│   │   ├── iconnection_handler.hpp
│   │   └── connection_pool.hpp
│   ├── http/
│   │   ├── parser.hpp      ← qbuem::http
│   │   ├── request.hpp
│   │   ├── response.hpp
│   │   ├── router.hpp
│   │   └── http_server.hpp ← qbuem::http-server
│   ├── pipeline/
│   │   ├── context.hpp     ← qbuem::context (header-only)
│   │   ├── service_registry.hpp
│   │   ├── action_env.hpp
│   │   ├── concepts.hpp
│   │   ├── async_channel.hpp ← qbuem::channel
│   │   ├── stream.hpp
│   │   ├── task_group.hpp
│   │   ├── action.hpp      ← qbuem::pipeline
│   │   ├── static_pipeline.hpp
│   │   ├── pipeline.hpp    (umbrella)
│   │   ├── dynamic_pipeline.hpp ← qbuem::pipeline-graph
│   │   ├── pipeline_graph.hpp
│   │   ├── message_bus.hpp
│   │   ├── retry_policy.hpp ← qbuem::resilience
│   │   ├── circuit_breaker.hpp
│   │   └── dead_letter.hpp
│   ├── tracing/
│   │   ├── trace_context.hpp ← qbuem::tracing
│   │   ├── span_exporter.hpp
│   │   └── pipeline_tracer.hpp
│   ├── metrics/
│   │   ├── action_metrics.hpp ← qbuem::metrics
│   │   └── pipeline_observer.hpp
│   └── protocol/
│       ├── websocket_handler.hpp ← qbuem::ws
│       ├── http2_handler.hpp     ← qbuem::http2
│       └── grpc_handler.hpp      ← qbuem::grpc
│
├── src/
│   ├── crypto/crypto.cpp
│   ├── reactor/reactor.cpp timer_wheel.cpp dispatcher.cpp
│   ├── reactor/epoll_reactor.cpp
│   ├── reactor/kqueue_reactor.cpp
│   ├── reactor/io_uring_reactor.cpp
│   ├── net/*.cpp
│   ├── io/*.cpp
│   ├── transport/*.cpp
│   ├── server/*.cpp
│   ├── http/*.cpp
│   └── protocol/*.cpp
│
├── tests/
│   ├── result_test.cpp
│   ├── arena_test.cpp
│   ├── reactor_test.cpp
│   ├── net_test.cpp
│   ├── buf_test.cpp
│   ├── codec_test.cpp
│   ├── http_test.cpp
│   ├── pipeline_test.cpp
│   ├── dispatcher_post_test.cpp
│   └── integration/
│       ├── http1_server_test.cpp   ← actual port bind + request sending
│       ├── pipeline_e2e_test.cpp   ← 3-stage pipeline end-to-end
│       └── websocket_test.cpp
│
├── cmake/
│   ├── qbuem-stack-config.cmake.in
│   ├── FindLiburing.cmake
│   └── PlatformDetect.cmake
│
└── CMakeLists.txt
```

---

## 7. Version Management Strategy

### SemVer Policy

| Change Type | Version Bump |
|-----------|-----------|
| API addition (backward-compat) | MINOR |
| API change/removal | MAJOR |
| Bug fix | PATCH |
| Internal implementation improvement | PATCH |

### Per-library Versioning

All libraries share a **single version** (`qbuem-stack VERSION`).
No per-library versions — monorepo single release.

```cmake
project(qbuem-stack VERSION 1.0.0)
# All targets use the same VERSION_MAJOR.MINOR.PATCH
```

---

## 8. External Injection Points (what services implement)

```
Interface                Injection Library        Service Implementation Examples
───────────────────────────────────────────────────────────────────────
ITransport               qbuem::transport        OpenSSLTransport
                                                 mbedTLSTransport
                                                 kTLSTransport (kTLS offload)
IBodyEncoder             qbuem::http-server      GzipEncoder (zlib)
                                                 BrotliEncoder (brotli)
                                                 ZstdEncoder (zstd)
ITokenVerifier           qbuem::http-server      JwtVerifier (OpenSSL)
                                                 PasetoVerifier
ISessionStore            qbuem::transport        RedisSessionStore
                                                 InMemorySessionStore
SpanExporter             qbuem::tracing          OtlpGrpcExporter
                                                 ZipkinExporter
                                                 JaegerExporter
IMetricsExporter         qbuem::metrics          PrometheusExporter
                                                 StatsdExporter
IIdempotencyStore        qbuem::resilience       RedisIdempotency
                                                 InMemoryIdempotency
ICheckpointStore         qbuem::pipeline-graph   RedisCheckpoint
                                                 FileCheckpoint
GrpcHandler<Req,Res>     qbuem::grpc             per-service handler implementation
                                                 (protobuf serialization included)
```

---

## 9. Migration Plan (current → target structure)

| Step | Description | Version | Status |
|------|------|------|------|
| 1 | Header move: `core/arena.hpp` → `arena.hpp`, `core/reactor.hpp` → `reactor/reactor.hpp` | v0.7.0 | ✅ |
| 2 | CMake target split: `qbuem_core` → `qbuem_result` + `qbuem_arena` + `qbuem_task` + `qbuem_reactor` | v0.7.0 | ✅ |
| 3 | Create net/ directory, add IO primitives | v0.7.0 | ✅ |
| 4 | Add buf/, io/, transport/, codec/, server/ | v0.7.0 | ✅ |
| 5 | Reorganize pipeline headers: separate context, channel, pipeline | v0.7.0 | ✅ |
| 6 | Split tracing/, metrics/ | v0.8.0 | ✅ |
| 7 | Add protocol/ (ws, http2, grpc handler stubs) | v1.0.0 | ✅ |
| 8 | Full 25-component support for `find_package(qbuem-stack COMPONENTS ...)` | v1.1 | ✅ |
| 9 | Implement DynamicPipeline, PipelineGraph, MessageBus | v1.0.0 | ✅ |
| 10 | Implement full HTTP/2 (HPACK), WebSocket (RFC 6455), gRPC (4 patterns) | v1.0.0 | ✅ |
| 11 | Add reactor/* forwarding headers (provide backward-compat paths) | v1.1 | ✅ |
| 12 | `qbuem::xdp` — AF_XDP + UMEM library (optional, QBUEM_XDP=ON) | v1.1 | ✅ |
| 13 | QUIC/HTTP3 `ITransport` reference guide | v1.1 | ✅ |
| 14 | HTTP/3 native (quiche bundled) / AF_XDP production example | v2.0 | planned |

### Maintaining Backward Compatibility

```cmake
# Maintain aliases so previous usage continues to work
add_library(qbuem-stack::core  ALIAS qbuem_reactor)  # old core → reactor
add_library(qbuem-stack::http  ALIAS qbuem_http)
add_library(qbuem-stack::qbuem ALIAS qbuem_all)
```

Header paths are also maintained via backward-compat forwarding headers:

```cpp
// All work equivalently — actual header is in core/
#include <qbuem/reactor/task.hpp>     // new path (forwarding)
#include <qbuem/core/task.hpp>        // legacy path (actual header)
```

---

## 10. AF_XDP Optional Library (`qbuem::xdp`)

Extreme-performance packet I/O that completely bypasses the kernel network stack.

### Build

```bash
# AF_XDP interface only (stub — compiles but no actual behavior)
cmake -DQBUEM_XDP=ON ..

# AF_XDP + libbpf actual integration (Linux 4.18+, requires libbpf-dev)
cmake -DQBUEM_XDP=ON -DQBUEM_XDP_LIBBPF=ON ..
```

### Usage

```cmake
find_package(qbuem-stack REQUIRED COMPONENTS xdp)
target_link_libraries(myapp PRIVATE qbuem-stack::xdp)
```

```cpp
#include <qbuem/xdp/xdp.hpp>

// Create UMEM (shared memory between NIC ↔ user space)
auto umem = qbuem::xdp::Umem::create({
    .frame_count   = 4096,
    .frame_size    = 4096,
    .use_hugepages = true,
});

// XSK socket (eth0, queue 0, native mode)
auto xsk = qbuem::xdp::XskSocket::create("eth0", 0, *umem, {
    .mode           = qbuem::xdp::XskConfig::Mode::Native,
    .force_zerocopy = true,
});

// Receive loop
umem->fill_frames(2048);
while (true) {
    qbuem::xdp::UmemFrame frames[64];
    uint32_t n = xsk->recv(frames, 64);
    // process ...
    umem->fill_frames(n);
}
```

### Use Cases

| Use Case | Target PPS | Notes |
|------|---------|------|
| Game server UDP | 10M+ PPS | RTT < 50µs |
| High-speed QUIC (HTTP/3) | 1M+ conn/s | connection migration |
| Packet capture/analysis | 100M+ PPS | tcpdump replacement |
| L4 load balancer | 100M+ PPS | XDP redirect |
