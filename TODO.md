# qbuem-stack Roadmap

**Zero Latency · Zero Cost · Zero Dependency**

> 파이프라인 상세 설계: **[docs/pipeline-design.md](./docs/pipeline-design.md)**

---

## 완료 (v0.4.0)

### IO Core 고도화
- [x] io_uring SQPOLL 모드 (`IORING_SETUP_SQPOLL`) — 권한 있으면 활성화, graceful fallback
- [x] `SO_REUSEPORT` per-reactor accept socket — accept storm 제거 (reactor당 전용 listen socket)
- [x] `Dispatcher::thread_count()` / `register_listener_at()` API 추가

### HTTP
- [x] Drain mode: 신규 accept 중단 후 기존 연결 자연 종료 대기 (`stop(drain_ms)`)
- [x] HTTP Trailer 지원 (Chunked Transfer 종료 후 헤더 추가 — `res.trailer(k, v)`)

### Middleware
- [x] `next()` 기반 async 미들웨어 (`AsyncMiddleware` 타입, `App::use_async()`)
- [x] Structured logging — `remote_addr`, `request_id`, `trace_id` 포함 (`enable_structured_log()`)
- [x] 동적 Rate Limit — IP별 설정 오버라이드 (`RateLimitConfig::per_key_override`)

---

## 완료 (v0.3.0)

### 문서화 (Doxygen)
- [x] `Doxyfile` 생성 — SVG 그래프, 다크 모드, 한국어 출력
- [x] `version.hpp`, `common.hpp` Doxygen 주석 완료
- [x] `core/arena.hpp`, `core/reactor.hpp`, `core/task.hpp` 완료
- [x] `core/dispatcher.hpp`, `core/awaiters.hpp`, `core/connection.hpp` 완료
- [x] `core/async_logger.hpp` 완료
- [x] `http/request.hpp`, `http/response.hpp`, `http/parser.hpp`, `http/router.hpp` 완료
- [x] `middleware/body_encoder.hpp`, `middleware/token_auth.hpp` 완료
- [x] `middleware/cors.hpp`, `middleware/rate_limit.hpp`, `middleware/security.hpp` 완료
- [x] `middleware/sse.hpp`, `middleware/static_files.hpp` 완료
- [x] `qbuem-stack.hpp`, `crypto.hpp`, `url.hpp` 완료

### Layer 0: Common
- [x] `Result<T>` + `unexpected<E>` — C++20 예외 없는 에러 처리
- [x] `FixedPoolResource<Size>` — O(1) slab allocator (embedded free-list)
- [x] `MonotonicBufferResource` — per-request arena
- [x] `qbuem::constant_time_equal()` — timing-safe 비교
- [x] `qbuem::csrf_token()` — CSPRNG 기반 128-bit 토큰
- [x] `qbuem::random_bytes()` — getrandom(2) / arc4random_buf()

### Layer 1: IO Core
- [x] `IReactor` 추상 인터페이스 (epoll / kqueue / io_uring)
- [x] `EpollReactor` (Linux)
- [x] `KqueueReactor` (macOS)
- [x] `IOUringReactor` (Linux, POLL_ADD 기반)
- [x] Thread-per-core Dispatcher (CPU affinity)
- [x] `Task<T>` 코루틴 (symmetric transfer)
- [x] `AsyncLogger` — lock-free SPSC ring-buffer, Text/JSON 포맷
- [x] vDSO 타이밍 (`CLOCK_REALTIME_COARSE`)

### Layer 2: HTTP
- [x] SIMD HTTP/1.1 FSM 파서 (AVX2 / SSE2 / NEON / scalar)
- [x] Keep-Alive, Chunked Transfer, 100-Continue, Pipelining
- [x] Radix Tree 라우터 (`/:param`, prefix 라우트)
- [x] Range Requests (206 / 416)
- [x] Conditional Requests (ETag, If-None-Match → 304)
- [x] HEAD fallback (GET 핸들러 재사용)

### Layer 3: Middleware
- [x] CORS (단일 origin + 동적 whitelist, `allow_origins`)
- [x] Rate Limit (토큰 버킷, thread-local, 429 + Retry-After)
- [x] Security Headers (HSTS, CSP, X-Frame-Options, …)
- [x] Request ID (UUID v4)
- [x] Static Files (ETag, Last-Modified, MIME, sendfile zero-copy)
- [x] SSE (`SseStream` — text/event-stream chunked streaming)
- [x] `IBodyEncoder` 추상 인터페이스 (압축 주입점)
- [x] `ITokenVerifier` + `bearer_auth()` 추상 인터페이스 (인증 주입점)

### Layer 4: App / Lifecycle
- [x] `App::listen()` — IPv4/IPv6 dual-stack
- [x] `App::listen_unix()` — AF_UNIX SOCK_STREAM (로컬 IPC)
- [x] `StackController` — 다중 App 라이프사이클 통합
- [x] SIGTERM/SIGINT graceful shutdown + drain flag
- [x] `health_check()`, `health_check_detailed()`
- [x] `liveness_endpoint("/live")` — K8s liveness probe
- [x] `readiness_endpoint("/ready")` — K8s readiness probe (drain 중 503)
- [x] `metrics_endpoint("/metrics")` — Prometheus 포맷
- [x] `set_max_connections(N)` — 초과 시 503 + Retry-After
- [x] `on_error(ErrorHandler)` — 전역 에러 핸들러
- [x] `enable_access_log()` / `enable_json_log()`
- [x] Cache-line 정렬 카운터 (requests, errors, active, bytes_sent)

### 소켓 최적화
- [x] TCP_FASTOPEN (Linux listen)
- [x] TCP_CORK + TCP_QUICKACK (세그먼트 병합)
- [x] SO_BUSY_POLL 50µs (Linux 수신 지연)
- [x] SO_SNDTIMEO 5s (느린 클라이언트 차단)
- [x] SO_NOSIGPIPE (macOS)
- [x] TCP_DEFER_ACCEPT (Linux — 데이터 도착 후 accept)
- [x] sendfile(2) Linux + macOS zero-copy
- [x] writev() scatter-gather (header+body 단일 syscall)

---

## v0.5.0 — IO 기반 확장 (파이프라인 전제조건)

> 파이프라인은 cross-thread 작업 주입(`Reactor::post`)이 없으면 구현 불가.
> 이 버전에서 해당 인프라를 완성한다.

### Reactor cross-thread wakeup
- [ ] `Reactor::post(fn)` 인터페이스 추가 — 어느 스레드에서나 안전하게 호출 가능
- [ ] `EpollReactor::post()` — `eventfd(EFD_NONBLOCK|EFD_CLOEXEC)` 기반
- [ ] `IOUringReactor::post()` — Linux 6.0+: `IORING_OP_MSG_RING`, 폴백: eventfd
- [ ] `KqueueReactor::post()` — `EVFILT_USER` + `NOTE_TRIGGER` 기반

### Dispatcher 확장
- [ ] `Dispatcher::post(fn)` — round-robin으로 워커 reactor에 주입
- [ ] `Dispatcher::post_to(idx, fn)` — 특정 reactor 인덱스에 주입
- [ ] `Dispatcher::spawn(Task<void>&&)` — suspended Task를 reactor에서 kick-off
- [ ] `Dispatcher::spawn_on(idx, Task<void>&&)` — 특정 reactor에서 시작

### IO Core 고도화
- [ ] io_uring Fixed Buffers (`io_uring_register_buffers`) — DMA 직접 쓰기
- [ ] io_uring Buffer Ring (`IORING_OP_PROVIDE_BUFFERS`) — 커널 버퍼 선택
- [ ] Write timeout 타이머 (응답 전송 최대 시간)

### 추상 인터페이스 확장
- [ ] `ITransport` — TLS 계층 주입점 (OpenSSL, mbedTLS, BoringSSL)
- [ ] `ISessionStore` — 세션 저장소 추상화 (Redis, in-memory)

---

## v0.6.0 — Pipeline MVP (StaticPipeline)

> 최소 완성 경로: `AsyncChannel → Action → StaticPipeline`
> Linux 단일 플랫폼 기준. v0.5.0의 `Reactor::post` + `Dispatcher::spawn` 필수.

### Layer 5: Pipeline 기반
- [ ] `AsyncChannel<T>` — Dmitry Vyukov MPMC ring buffer
  - head_/tail_ cache-line 분리 (`alignas(64)`)
  - `send()` / `recv()` — backpressure: 포화/비면 co_await 대기
  - `try_send()` / `try_recv()` — lock-free, 논블로킹
  - `close()` + EOS 전파
  - waiter → `{Reactor*, coroutine_handle<>}` intrusive list
  - cross-reactor wakeup: `waiter.reactor->post([h]{h.resume();})`
- [ ] `Stream<T>` — `StreamItem<T> = variant<T, StreamEnd>` (move-only 완전 지원)
  - `yield()`, `finish()`, `next()`, async range-for
  - `tee()` — 동일 스트림 두 소비자 분기

### Action (정적)
- [ ] `Action<In, Out>` — 코루틴 워커 풀
  - `Config`: min/max_workers, channel_cap, auto_scale, keyed_ordering
  - `scale_to(n)` / `scale_in()` / `scale_out()`
  - scale-in: `atomic<size_t> target_workers` + 워커 인덱스 비교 (poison pill 미사용)
  - `std::stop_token` 기반 취소
  - 처리 함수: `Task<Result<Out>>(In, std::stop_token)` — 예외 금지
- [ ] `BatchAction<In, Out>` — 최대 N개 아이템 묶음 처리

### StaticPipeline
- [ ] `PipelineBuilder<In>` — `add<Out>(action)` 마다 새 타입 반환 (컴파일타임 체인)
  - `[[nodiscard]] build()` — `StaticPipeline<OrigIn, CurIn>` 반환
  - 타입 불일치 시 컴파일 에러
- [ ] `StaticPipeline<In, Out>`
  - `start(Dispatcher&)` / `drain()` / `stop()`
  - `push(In)` (backpressure) / `try_push(In)` (논블로킹)
  - `then()` / `fan_out()` / `fan_out_round_robin()` / `route_if()` / `tee()`
  - 파이프라인 상태 머신: Created → Built → Starting → Running → Draining → Stopped
  - `IPipelineInput<T>` — 타입 소거 입력 인터페이스 (fan-out에 활용)
- [ ] 통합 테스트: StaticPipeline 3단계 체인, scale-out, drain, backpressure

---

## v0.7.0 — DynamicPipeline + PipelineGraph + MessageBus

### DynamicPipeline
- [ ] `IDynamicAction` — 타입 소거 Action 인터페이스
  - `ActionSchema { input_type, output_type }` — 런타임 스키마 호환성 체크
  - `process_erased(void*, void*, stop_token)` — 타입 소거 처리
- [ ] `make_dynamic_action<In,Out>(Action)` — 정적 Action → 동적 어댑터
- [ ] `DynamicPipeline`
  - `add_action()` / `insert_before()` / `insert_after()` / `remove_action()` — stopped 상태
  - 상태 머신: Created → Configured → Starting → Running → Reconfiguring → Draining → Stopped
  - `start()` / `drain()` / `stop()`

### PipelineGraph
- [ ] `PipelineGraph` — DAG 오케스트레이션
  - `add(name, pipeline)` — Static/Dynamic 모두 지원 (타입 소거)
  - `connect()` / `fan_out()` / `merge_into()` / `route_if()`
  - `start()` — Kahn's algorithm으로 사이클 감지 + 위상 정렬 후 순서대로 시작
  - `drain_all()` / `stop_all()`
  - A/B 라우팅: `ab_route(from, target_a, target_b, b_fraction)`

### MessageBus
- [ ] `MessageBus` — gRPC 스타일 4가지 메시지 패턴
  - **Unary**: `RequestEnvelope<Req,Res>` = `{request, shared_ptr<AsyncChannel<Result<Res>>>}`
  - **Server Streaming**: `ServerStreamEnvelope<Req,Res>`
  - **Client Streaming**: `ClientStreamEnvelope<Req,Res>`
  - **Bidirectional**: `BidiEnvelope<Req,Res>` (두 방향 독립 채널)
  - `create_*()` 등록 / 이름 기반 채널 접근 / DLQ 접근

### 관찰 가능성 기반
- [ ] `ActionMetrics` — items_processed, errors, retried, dlq, latency_buckets (4구간)
- [ ] `PipelineMetrics` — 파이프라인 단위 집계
- [ ] `PipelineObserver` 훅 인터페이스
  - `on_item_start/done`, `on_error`, `on_scale_event`, `on_state_change`
  - `on_dlq_item`, `on_circuit_open/close`
- [ ] `LoggingObserver` 기본 구현

---

## v0.8.0 — 복원력 + 분산 트레이싱

### 복원력 패턴
- [ ] `RetryPolicy` — Fixed / Exponential / ExponentialJitter backoff
  - `max_attempts`, `base_delay`, `max_delay`, `deadline`, `retryable_errors`
- [ ] `CircuitBreaker` — Closed / Open / HalfOpen 상태 머신
  - `failure_threshold`, `success_threshold`, `open_duration`
  - Open 상태 아이템 → 즉시 DLQ (처리 시도 없음)
- [ ] `DeadLetter<T>` — `{item, error_code, attempt_count, failed_at}`
  - `DeadLetterQueue`: `MessageBus` 채널명으로 접근
- [ ] Bulkhead: `channel_cap` 기반 자동 backpressure (별도 구현 불필요, 문서화)

### 분산 트레이싱 (OpenTelemetry 호환)
- [ ] `TraceContext` — W3C Trace Context 표준
  - `trace_id[16]` (128-bit) / `span_id[8]` (64-bit) / `trace_flags`
  - `generate()` / `child_span()` / `to_traceparent()` / `from_traceparent()`
- [ ] thread-local TraceContext 전파 (전략 B — 타입 오염 없음)
  - `Reactor::set_current_trace_context()` / `get_current_trace_context()`
- [ ] Pluggable `Sampler` 인터페이스
  - `AlwaysSampler` / `NeverSampler`
  - `ProbabilitySampler(rate)` — 0.0~1.0
  - `RateLimitingSampler(max_per_second)` — token bucket
  - `ParentBasedSampler` — 부모 결정 따름
- [ ] `SpanExporter` 인터페이스 + `SpanData` (pipeline, action, context, timing, error)
  - `LoggingExporter` — 디버그 기본 구현
  - `NoopExporter` — 트레이싱 비활성화 시 zero-overhead
  - `OtlpGrpcExporter` / `OtlpHttpExporter` — OpenTelemetry Collector
  - `JaegerExporter` / `ZipkinExporter`
- [ ] `PipelineTracer` — `start_span()` / `end_span()`, 전역 등록 (`set_global_tracer`)
- [ ] HTTP 통합: `traceparent` 헤더 자동 파싱 → thread-local 설정 → 응답 `traceresponse`
- [ ] `IMetricsExporter` — Prometheus push 추상화

---

## v0.9.0 — Pipeline 고도화

### Hot-swap (무중단 액션 교체)
- [ ] `DynamicPipeline::hot_swap(name, new_action, timeout)`
  - Seal → Drain → Swap → Resume 절차
  - 타임아웃 초과 시 `errc::timed_out`
  - 스키마 불일치 시 `errc::invalid_argument`
  - Running 상태 아닌 경우 `errc::operation_not_permitted`

### 우선순위 채널
- [ ] `PriorityChannel<T>` — High / Normal / Low 3레벨
  - recv: High 소진 → Normal 소진 → Low 순서 보장
  - Aging: Low가 N회 연속 skip 시 강제 처리 (스타베이션 방지)
  - `set_aging_threshold(n)` — 기본 100

### Config-driven Pipeline
- [ ] `PipelineFactory` — JSON/YAML → `DynamicPipeline` / `PipelineGraph` 생성
  - `register_plugin(name, factory)` — 코드 or .so 플러그인 등록
  - `from_json()` / `from_yaml()` / `graph_from_json()`

### Pipeline 합성
- [ ] `SubpipelineAction<In,Out>` — `StaticPipeline<In,Out>`을 `Action<In,Out>`처럼 내장
  - 재사용성: 공통 처리 로직 캡슐화
  - 테스트 용이성: inner pipeline mock 교체 가능

### Arena 통합
- [ ] reactor-local `FixedPoolResource<sizeof(PipelineItem<T>)>` 아이템 할당
  - malloc/free 제거, 캐시 효율 극대화
  - `ArenaChannel<T>` — 동일 reactor 내 zero-copy 전달

### 성능 최적화
- [ ] Reactor / Connection 구조체 cache-line 패킹 측정 및 최적화
- [ ] `__builtin_prefetch` — 다음 Connection 구조체 미리 로드
- [ ] 2KB 이하 요청 헤더 스택 할당 (힙 회피)
- [ ] `MSG_ZEROCOPY` (`SO_ZEROCOPY`) — 송신 kernel→user 복사 제거
- [ ] PGO 2-pass 빌드 가이드 (Instrumented → wrk → Optimized)

---

## v1.0.0 — 프로토콜 확장

### 프로토콜
- [ ] HTTP/2 (nghttp2 추상화, `IHttp2Handler`)
- [ ] WebSocket (RFC 6455, `IWebSocketHandler`)
- [ ] HTTP/3 / QUIC (quiche FFI 추상화)
- [ ] gRPC — `MessageBus` Bidi 채널 → gRPC 백엔드 연결 (protobuf 추상화)
- [ ] `AF_XDP` eXpress Data Path (극한 성능, 별도 레이어)

### gRPC ↔ Pipeline 통합
- [ ] gRPC 서버 스트리밍 → `Stream<T>` 직접 연결
- [ ] gRPC 클라이언트 스트리밍 → `AsyncChannel<T>` 직접 연결
- [ ] `BidiEnvelope<Req,Res>` → gRPC bidi 핸들러 어댑터

---

## 구현 의존성 그래프

```
[0] Reactor::post() + Dispatcher::spawn()   ← v0.5.0
         │
         ▼
[1] AsyncChannel<T>                         ← v0.6.0
         │
    ┌────┴────┐
    ▼         ▼
[2a] Stream<T>  [2b] RetryPolicy/CB/DLQ (헤더 only)
    │         │
    └────┬────┘
         ▼
[3] Action<In,Out>  IDynamicAction           ← v0.6.0 / v0.7.0
         │
    ┌────┴────┐
    ▼         ▼
[4a] StaticPipeline    [4b] DynamicPipeline  ← v0.6.0 / v0.7.0
[4c] PipelineObserver/Metrics (병행)
         │
    ┌────┴────┐
    ▼         ▼
[5a] PipelineGraph  [5b] MessageBus          ← v0.7.0
[5c] PipelineFactory                         ← v0.9.0
         │
         ▼
[6] PipelineTracer + SpanExporter            ← v0.8.0
[7] hot_swap + PriorityChannel + Arena       ← v0.9.0
[8] HTTP/2 / WebSocket / gRPC               ← v1.0.0
```

---

## 배포 전략

```
qbuem-stack (이 레포)
├── qbuem-stack::core      — IO 레이어만 (임베디드, 게임 서버 등)
├── qbuem-stack::http      — HTTP 파서 + 라우터만
├── qbuem-stack::pipeline  — Pipeline 레이어 (core 의존)
└── qbuem-stack::qbuem     — 전체 (대부분의 서비스)

외부 통합 (서비스에서 직접 구현):
├── qbuem-json             — JSON (권장)
├── [서비스]-gzip-encoder  — IBodyEncoder 구현체 (zlib/brotli/zstd)
├── [서비스]-jwt-verifier  — ITokenVerifier 구현체 (OpenSSL/mbedTLS)
├── [서비스]-otlp-exporter — SpanExporter 구현체 (OpenTelemetry Collector)
└── [서비스]-tls-transport — ITransport 구현체 (예정)
```

---

## 라이선스 참고

- liburing (LGPL-2.1) — 동적 링크 권장
- OpenSSL 3.x (Apache-2.0) — 서비스에서 직접 링크, qbuem-stack은 링크 안 함
- qbuem-json (MIT) — 서비스에서 직접 링크, tests에서만 사용
