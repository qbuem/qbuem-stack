# qbuem-stack 라이브러리 분리 전략

> **원칙**: 기능 단위 독립 라이브러리 · 단방향 의존성 · Zero external dependency
>
> 관련 문서:
> - IO 아키텍처: [docs/io-architecture.md](./io-architecture.md)
> - IO 기술 심층 분석: [docs/io-deep-dive.md](./io-deep-dive.md)
> - 파이프라인 설계: [docs/pipeline-design.md](./pipeline-design.md)

---

## 1. 설계 원칙

| 원칙 | 내용 |
|------|------|
| **단일 책임** | 라이브러리 하나 = 명확한 기능 하나 |
| **단방향 의존** | 상위 레이어가 하위를 의존. 역방향 없음 |
| **Zero external dep** | 각 라이브러리 헤더에 외부 라이브러리 노출 금지 |
| **독립 소비** | 임베디드 서버는 `qbuem::reactor`만, HTTP 서버는 `qbuem::http-server`만 |
| **헤더 온리 우선** | template/concept 위주 라이브러리는 헤더 온리로 노출 |
| **모노레포** | 하나의 git 레포, 다수의 CMake 타겟으로 관리 |

---

## 2. 라이브러리 전체 목록 (9레벨)

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

## 3. 의존성 그래프

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

## 4. 라이브러리별 상세

### Level 0 — Foundation

#### `qbuem::result` (header-only)
```
헤더: include/qbuem/result.hpp  (현재 common.hpp 분리)
의존: 없음
내용: Result<T>, unexpected<E>, errc enum, ok(), err()
용도: 모든 라이브러리의 공통 에러 타입
```

#### `qbuem::arena` (header-only)
```
헤더: include/qbuem/arena.hpp  (현재 core/arena.hpp → 이동)
의존: 없음
내용: Arena, FixedPoolResource<N,A>, BufferPool<Size,Count>
용도: 모든 레이어의 zero-alloc 메모리 기반
```

#### `qbuem::crypto` (static lib)
```
헤더: include/qbuem/crypto.hpp
소스: src/crypto/crypto.cpp
의존: result (+ POSIX getrandom/arc4random)
내용: constant_time_equal(), csrf_token(), random_bytes()
용도: 타이밍 안전 비교, CSPRNG. 외부 OpenSSL 없음.
```

---

### Level 1 — Async Core

#### `qbuem::task` (header-only)
```
헤더: include/qbuem/task.hpp, include/qbuem/awaiters.hpp
의존: result
내용: Task<T>, promise_type, symmetric transfer, AsyncRead/Write awaiters
용도: 모든 코루틴 기반 비동기 작업의 토대
```

#### `qbuem::reactor` (static lib — interface + TimerWheel)
```
헤더: include/qbuem/reactor/reactor.hpp
       include/qbuem/reactor/timer_wheel.hpp
소스: src/reactor/reactor.cpp (current() thread_local)
       src/reactor/timer_wheel.cpp
의존: result, task
내용: Reactor abstract interface, EventType, TimerWheel O(1)
용도: 플랫폼 독립 이벤트 루프 추상화
주의: 구체 구현 없음 — epoll/kqueue/iouring 중 하나와 링크 필요
```

#### `qbuem::dispatcher` (static lib)
```
헤더: include/qbuem/reactor/dispatcher.hpp
소스: src/reactor/dispatcher.cpp
의존: reactor
내용: Dispatcher, thread-per-core, post(), spawn(), CPU affinity
용도: 다중 Reactor 조율, Worker 스레드 관리
```

---

### Level 2 — Reactor Implementations

#### `qbuem::epoll` (static lib, Linux only)
```
헤더: include/qbuem/reactor/epoll_reactor.hpp
소스: src/reactor/epoll_reactor.cpp
의존: reactor
내용: EpollReactor (eventfd wakeup, level-trigger)
플랫폼: Linux
```

#### `qbuem::kqueue` (static lib, macOS/BSD only)
```
헤더: include/qbuem/reactor/kqueue_reactor.hpp
소스: src/reactor/kqueue_reactor.cpp
의존: reactor
내용: KqueueReactor (EVFILT_USER wakeup)
플랫폼: macOS, FreeBSD
```

#### `qbuem::iouring` (static lib, Linux 5.1+)
```
헤더: include/qbuem/reactor/io_uring_reactor.hpp
소스: src/reactor/io_uring_reactor.cpp
의존: reactor (liburing — pimpl 내부, 헤더 미노출)
내용: IOUringReactor (SQPOLL, Fixed Buffers, Buffer Ring)
       고급: multishot accept/recv, linked SQEs, fixed files
플랫폼: Linux 5.1+
옵션:  QBUEM_IOURING=ON/OFF (없으면 epoll 자동 선택)
```

**CMake 자동 선택 로직:**
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
헤더: include/qbuem/net/socket_addr.hpp
       include/qbuem/net/tcp_listener.hpp
       include/qbuem/net/tcp_stream.hpp
       include/qbuem/net/udp_socket.hpp
       include/qbuem/net/unix_socket.hpp
소스: src/net/*.cpp
의존: reactor, result, task
내용: SocketAddr(IPv4/IPv6/Unix), TcpListener(SO_REUSEPORT),
       TcpStream(readv/writev), UdpSocket(sendmsg/recvmsg),
       UnixSocket(stream/dgram)
용도: 모든 네트워크 I/O의 기반. TLS 없음 (transport에서 주입).
```

#### `qbuem::buf` (header-only)
```
헤더: include/qbuem/buf/io_slice.hpp
       include/qbuem/buf/read_buf.hpp
       include/qbuem/buf/write_buf.hpp
의존: arena
내용: IOSlice, IOVec<N>, ReadBuf<N>(링버퍼), WriteBuf(코르크)
용도: 할당 없는 scatter-gather IO 버퍼 추상화
```

#### `qbuem::file` (static lib)
```
헤더: include/qbuem/io/async_file.hpp
소스: src/io/async_file.cpp
의존: reactor, buf
내용: AsyncFile (io_uring READ/WRITE_FIXED 우선, pread 폴백)
       O_DIRECT 지원, 정렬 버퍼 검증
```

#### `qbuem::zerocopy` (static lib, Linux)
```
헤더: include/qbuem/io/zero_copy.hpp
소스: src/io/zero_copy.cpp
의존: net, reactor
내용: sendfile(2), splice(2), MSG_ZEROCOPY + wait
       kTLS TX 오프로드 감지 및 연동 (Linux 4.13+)
플랫폼: Linux (macOS: sendfile 헤더는 있으나 기능 제한)
```

---

### Level 4 — Transport + Codec + Server

#### `qbuem::transport` (header-only + small impl)
```
헤더: include/qbuem/transport/itransport.hpp
       include/qbuem/transport/plain_transport.hpp
소스: src/transport/plain_transport.cpp
의존: net, task, result
내용: ITransport (추상), PlainTransport (TCP 구체 구현)
주입점: 서비스에서 OpenSSL/mbedTLS 구현체를 ITransport로 주입
        kTLS: 핸드셰이크 후 setsockopt(SOL_TLS) 설정하는 구현체도 가능
```

#### `qbuem::codec` (header-only)
```
헤더: include/qbuem/codec/frame_codec.hpp
       include/qbuem/codec/length_prefix_codec.hpp
       include/qbuem/codec/line_codec.hpp
의존: buf, result
내용: IFrameCodec<Frame>, LengthPrefixedCodec<H>, LineCodec
용도: 프레임 경계 파싱. 할당 없음 — 모든 작업이 IOSlice/ReadBuf 위에서.
```

#### `qbuem::server` (static lib)
```
헤더: include/qbuem/server/accept_loop.hpp
       include/qbuem/server/iconnection_handler.hpp
       include/qbuem/server/connection_pool.hpp
소스: src/server/*.cpp
의존: net, transport, codec, dispatcher, arena
내용: AcceptLoop<Frame,HandlerFactory> (SO_REUSEPORT, coroutine)
       IConnectionHandler<Frame>
       ConnectionPool<T> (min_idle, max_size, health_check)
용도: 서버 연결 수락/관리 인프라. 프로토콜 독립.
```

---

### Level 5 — HTTP

#### `qbuem::http` (static lib)
```
헤더: include/qbuem/http/parser.hpp
       include/qbuem/http/request.hpp
       include/qbuem/http/response.hpp
       include/qbuem/http/router.hpp
소스: src/http/*.cpp
의존: buf, codec, result
내용: SIMD HTTP/1.1 FSM 파서 (AVX2/SSE2/NEON/scalar),
       Request/Response 값 타입, Radix Tree 라우터
용도: HTTP 레이어. App/미들웨어 없음 — http-server에서 제공.
```

#### `qbuem::http-server` (static lib)
```
헤더: include/qbuem/http/http1_handler.hpp
       include/qbuem/http/http_server.hpp
소스: src/http/http1_handler.cpp
의존: http, server, transport, arena
내용: Http1Handler (IConnectionHandler<http::Request> 구현)
       keep-alive, chunked, 100-continue, Upgrade 처리
       App (listen/middleware/lifecycle) — 현재 qbuem_stack.cpp
용도: 실제 HTTP 서버 구동. 미들웨어 체인 포함.
```

---

### Level 6 — Pipeline

#### `qbuem::context` (header-only)
```
헤더: include/qbuem/pipeline/context.hpp
       include/qbuem/pipeline/service_registry.hpp
       include/qbuem/pipeline/action_env.hpp
       include/qbuem/pipeline/concepts.hpp
의존: task, result
내용: Context (불변 persistent list), ServiceRegistry (DI),
       ActionEnv, WorkerLocal<T>, ContextualItem<T>
       C++20 Concepts: ActionFn, BatchActionFn, PipelineInputFor
```

#### `qbuem::channel` (header-only)
```
헤더: include/qbuem/pipeline/async_channel.hpp
       include/qbuem/pipeline/stream.hpp
       include/qbuem/pipeline/task_group.hpp
의존: task, reactor, context
내용: AsyncChannel<T> (Vyukov MPMC), Stream<T>, TaskGroup
       cross-reactor wakeup via Reactor::post()
```

#### `qbuem::pipeline` (header-only)
```
헤더: include/qbuem/pipeline/action.hpp
       include/qbuem/pipeline/static_pipeline.hpp
       include/qbuem/pipeline/pipeline.hpp  ← umbrella include
의존: channel, context, dispatcher
내용: Action<In,Out>, StaticPipeline<In,Out>, PipelineBuilder
```

---

### Level 7 — Pipeline Extensions

#### `qbuem::pipeline-graph` (header-only)
```
헤더: include/qbuem/pipeline/dynamic_pipeline.hpp
       include/qbuem/pipeline/pipeline_graph.hpp
       include/qbuem/pipeline/message_bus.hpp
의존: pipeline
내용: IDynamicAction, DynamicPipeline (hot-swap, insert/remove)
       PipelineGraph (DAG, Kahn's algo, fan-out, A/B route)
       MessageBus (Unary/ServerStream/ClientStream/Bidi)
```

#### `qbuem::resilience` (header-only)
```
헤더: include/qbuem/pipeline/retry_policy.hpp
       include/qbuem/pipeline/circuit_breaker.hpp
       include/qbuem/pipeline/dead_letter.hpp
의존: pipeline, context
내용: RetryPolicy (Fixed/Exp/Jitter), CircuitBreaker (3-state),
       DeadLetter<T>, DeadLetterQueue
```

#### `qbuem::tracing` (header-only)
```
헤더: include/qbuem/tracing/trace_context.hpp
       include/qbuem/tracing/span_exporter.hpp
       include/qbuem/tracing/pipeline_tracer.hpp
의존: context
내용: TraceContext (W3C), Sampler 인터페이스, SpanExporter 인터페이스
       PipelineTracer, LoggingExporter, NoopExporter
주입점: OtlpGrpcExporter/ZipkinExporter 등은 서비스에서 구현
```

#### `qbuem::metrics` (header-only)
```
헤더: include/qbuem/metrics/action_metrics.hpp
       include/qbuem/metrics/pipeline_observer.hpp
의존: pipeline, context
내용: ActionMetrics, PipelineMetrics, PipelineObserver 훅
       LoggingObserver (기본 구현)
주입점: Prometheus push는 서비스에서 IMetricsExporter 구현
```

---

### Level 8 — Protocol Handlers

#### `qbuem::ws` (static lib)
```
헤더: include/qbuem/protocol/websocket_handler.hpp
소스: src/protocol/websocket_handler.cpp
의존: http-server, server, buf
내용: WebSocketHandler (RFC 6455)
       HTTP Upgrade, masking/unmasking, PING/PONG, CLOSE handshake
```

#### `qbuem::http2` (static lib)
```
헤더: include/qbuem/protocol/http2_handler.hpp
소스: src/protocol/http2_handler.cpp, src/protocol/hpack.cpp
의존: http-server, server, buf, arena
내용: Http2Handler, HPACK encoder/decoder (arena 기반, zero-alloc),
       stream multiplexing, SETTINGS/WINDOW_UPDATE/PING/GOAWAY
외부dep: 없음 (nghttp2 사용 안 함 — 직접 구현)
```

#### `qbuem::grpc` (static lib)
```
헤더: include/qbuem/protocol/grpc_handler.hpp
소스: src/protocol/grpc_handler.cpp
의존: http2, pipeline, channel
내용: GrpcHandler<Req,Res> (Unary/Server/Client/Bidi)
       Stream<Res> / AsyncChannel<Req> 직접 연결
주입점: protobuf 직렬화는 서비스에서 주입 (템플릿 파라미터)
```

---

### Level 9 — Umbrella

#### `qbuem` (interface lib, links all)
```
헤더: include/qbuem/qbuem-stack.hpp  ← 현재 파일 유지
의존: 모든 레이어
용도: 대부분의 서비스 — 하나만 링크하면 전체 사용 가능
```

---

## 5. CMake 타겟 이름 규칙

```cmake
# 네임스페이스: qbuem-stack::
# 타겟 이름: qbuem_<name>   (내부용)
# 별칭:      qbuem-stack::<name>  (소비용)

# 예시
add_library(qbuem_result INTERFACE)
add_library(qbuem-stack::result ALIAS qbuem_result)

add_library(qbuem_net STATIC ...)
add_library(qbuem-stack::net ALIAS qbuem_net)
```

### 소비 예시 (서비스 CMakeLists.txt)

```cmake
# 케이스 1: 전체 스택 (웹 서비스)
find_package(qbuem-stack REQUIRED)
target_link_libraries(my-web-service PRIVATE qbuem-stack::qbuem)

# 케이스 2: HTTP 서버만 (API 서버)
find_package(qbuem-stack REQUIRED COMPONENTS http-server pipeline)
target_link_libraries(my-api PRIVATE
    qbuem-stack::http-server
    qbuem-stack::pipeline)

# 케이스 3: IO 레이어만 (게임 서버, 임베디드)
find_package(qbuem-stack REQUIRED COMPONENTS net buf server)
target_link_libraries(my-game-server PRIVATE
    qbuem-stack::net
    qbuem-stack::buf
    qbuem-stack::server)

# 케이스 4: Pipeline만 (데이터 처리)
find_package(qbuem-stack REQUIRED COMPONENTS pipeline resilience tracing)
target_link_libraries(my-worker PRIVATE
    qbuem-stack::pipeline
    qbuem-stack::resilience
    qbuem-stack::tracing)

# 케이스 5: Reactor만 (임베디드 IO 서버)
find_package(qbuem-stack REQUIRED COMPONENTS reactor iouring net)
target_link_libraries(my-embedded PRIVATE
    qbuem-stack::iouring
    qbuem-stack::net)
```

---

## 6. 디렉토리 구조 (목표 상태)

```
qbuem-stack/
├── include/qbuem/
│   ├── result.hpp          ← qbuem::result (헤더 온리)
│   ├── arena.hpp           ← qbuem::arena  (헤더 온리)
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
│   │   ├── io_slice.hpp    ← qbuem::buf (헤더 온리)
│   │   ├── read_buf.hpp
│   │   └── write_buf.hpp
│   ├── io/
│   │   ├── async_file.hpp  ← qbuem::file
│   │   └── zero_copy.hpp   ← qbuem::zerocopy
│   ├── transport/
│   │   ├── itransport.hpp  ← qbuem::transport
│   │   └── plain_transport.hpp
│   ├── codec/
│   │   ├── frame_codec.hpp ← qbuem::codec (헤더 온리)
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
│   │   ├── context.hpp     ← qbuem::context (헤더 온리)
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
│       ├── http1_server_test.cpp   ← 실제 포트 bind + 요청 전송
│       ├── pipeline_e2e_test.cpp   ← 3단계 파이프라인 end-to-end
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

## 7. 버전 관리 전략

### SemVer 정책

| 변경 유형 | 버전 증가 |
|-----------|-----------|
| API 추가 (backward-compat) | MINOR |
| API 변경/삭제 | MAJOR |
| 버그 수정 | PATCH |
| 내부 구현 개선 | PATCH |

### 라이브러리별 버전

모든 라이브러리는 **단일 버전** (`qbuem-stack VERSION`)을 공유한다.
개별 라이브러리 버전 없음 — 모노레포 단일 릴리스.

```cmake
project(qbuem-stack VERSION 1.0.0)
# 모든 타겟이 동일한 VERSION_MAJOR.MINOR.PATCH 사용
```

---

## 8. 외부 주입 지점 (서비스가 구현할 것들)

```
인터페이스               주입 라이브러리          서비스 구현 예시
───────────────────────────────────────────────────────────────────────
ITransport               qbuem::transport        OpenSSLTransport
                                                 mbedTLSTransport
                                                 kTLSTransport (kTLS 오프로드)
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
GrpcHandler<Req,Res>     qbuem::grpc             서비스별 핸들러 구현
                                                 (protobuf serialization 포함)
```

---

## 9. 마이그레이션 계획 (현재 → 목표 구조)

| 단계 | 내용 | 버전 | 상태 |
|------|------|------|------|
| 1 | 헤더 이동: `core/arena.hpp` → `arena.hpp`, `core/reactor.hpp` → `reactor/reactor.hpp` | v0.7.0 | ✅ |
| 2 | CMake 타겟 분리: `qbuem_core` → `qbuem_result` + `qbuem_arena` + `qbuem_task` + `qbuem_reactor` | v0.7.0 | ✅ |
| 3 | `net/` 디렉토리 신설, IO 프리미티브 추가 | v0.7.0 | ✅ |
| 4 | `buf/`, `io/`, `transport/`, `codec/`, `server/` 추가 | v0.7.0 | ✅ |
| 5 | Pipeline 헤더 재구성: `context`, `channel`, `pipeline` 분리 | v0.7.0 | ✅ |
| 6 | `tracing/`, `metrics/` 분리 | v0.8.0 | ✅ |
| 7 | `protocol/` 추가 (ws, http2, grpc 핸들러 스텁) | v1.0.0 | ✅ |
| 8 | `find_package(qbuem-stack COMPONENTS ...)` 전체 25개 컴포넌트 지원 | v1.1 | ✅ |
| 9 | DynamicPipeline, PipelineGraph, MessageBus 구현 | v1.0.0 | ✅ |
| 10 | HTTP/2 full (HPACK), WebSocket (RFC 6455), gRPC (4패턴) 구현 | v1.0.0 | ✅ |
| 11 | `reactor/*` 포워딩 헤더 추가 (하위 호환 경로 제공) | v1.1 | ✅ |
| 12 | `qbuem::xdp` — AF_XDP + UMEM 라이브러리 (선택적, QBUEM_XDP=ON) | v1.1 | ✅ |
| 13 | QUIC/HTTP3 `ITransport` 레퍼런스 가이드 | v1.1 | ✅ |
| 14 | HTTP/3 native (quiche 동봉) / AF_XDP 프로덕션 예제 | v2.0 | 계획 |

### 하위 호환성 유지

```cmake
# 이전 사용법도 계속 동작하도록 alias 유지
add_library(qbuem-stack::core  ALIAS qbuem_reactor)  # 기존 core → reactor
add_library(qbuem-stack::http  ALIAS qbuem_http)
add_library(qbuem-stack::qbuem ALIAS qbuem_all)
```

헤더 경로도 하위 호환 포워딩 헤더로 유지됩니다:

```cpp
// 모두 동일하게 동작 — 실제 헤더는 core/ 에 있음
#include <qbuem/reactor/task.hpp>     // 새 경로 (포워딩)
#include <qbuem/core/task.hpp>        // 기존 경로 (실제 헤더)
```

---

## 10. AF_XDP 선택적 라이브러리 (`qbuem::xdp`)

커널 네트워크 스택을 완전히 우회하는 극한 성능 패킷 I/O.

### 빌드

```bash
# AF_XDP 인터페이스만 (stub — 컴파일은 되나 실제 동작 없음)
cmake -DQBUEM_XDP=ON ..

# AF_XDP + libbpf 실제 연동 (Linux 4.18+, libbpf-dev 필요)
cmake -DQBUEM_XDP=ON -DQBUEM_XDP_LIBBPF=ON ..
```

### 사용

```cmake
find_package(qbuem-stack REQUIRED COMPONENTS xdp)
target_link_libraries(myapp PRIVATE qbuem-stack::xdp)
```

```cpp
#include <qbuem/xdp/xdp.hpp>

// UMEM 생성 (NIC ↔ 유저스페이스 공유 메모리)
auto umem = qbuem::xdp::Umem::create({
    .frame_count   = 4096,
    .frame_size    = 4096,
    .use_hugepages = true,
});

// XSK 소켓 (eth0, 큐 0, native mode)
auto xsk = qbuem::xdp::XskSocket::create("eth0", 0, *umem, {
    .mode           = qbuem::xdp::XskConfig::Mode::Native,
    .force_zerocopy = true,
});

// 수신 루프
umem->fill_frames(2048);
while (true) {
    qbuem::xdp::UmemFrame frames[64];
    uint32_t n = xsk->recv(frames, 64);
    // 처리 ...
    umem->fill_frames(n);
}
```

### 적용 사례

| 사례 | 목표 PPS | 비고 |
|------|---------|------|
| 게임 서버 UDP | 10M+ PPS | RTT < 50µs |
| 고속 QUIC (HTTP/3) | 1M+ conn/s | 연결 마이그레이션 |
| 패킷 캡처/분석 | 100M+ PPS | tcpdump 대체 |
| L4 로드밸런서 | 100M+ PPS | XDP redirect |
