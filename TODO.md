# qbuem-stack Roadmap

**Zero Latency · Zero Allocation · Zero Dependency**

> 파이프라인 설계: **[docs/pipeline-design.md](./docs/pipeline-design.md)**
> IO 레이어 아키텍처: **[docs/io-architecture.md](./docs/io-architecture.md)**
> IO 기술 심층 분석: **[docs/io-deep-dive.md](./docs/io-deep-dive.md)**
> 라이브러리 분리 전략: **[docs/library-strategy.md](./docs/library-strategy.md)**

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
- [x] `Reactor::post(fn)` 인터페이스 추가 — 어느 스레드에서나 안전하게 호출 가능
- [x] `EpollReactor::post()` — `eventfd(EFD_NONBLOCK|EFD_CLOEXEC)` 기반
- [x] `IOUringReactor::post()` — eventfd + OpKind::Wake POLL_ADD
- [x] `KqueueReactor::post()` — `EVFILT_USER` + `NOTE_TRIGGER` 기반

### Dispatcher 확장
- [x] `Dispatcher::post(fn)` — round-robin으로 워커 reactor에 주입
- [x] `Dispatcher::post_to(idx, fn)` — 특정 reactor 인덱스에 주입
- [x] `Dispatcher::spawn(Task<void>&&)` — suspended Task를 reactor에서 kick-off
- [x] `Dispatcher::spawn_on(idx, Task<void>&&)` — 특정 reactor에서 시작

### IO Core 고도화
- [x] io_uring Fixed Buffers (`io_uring_register_buffers`) — DMA 직접 쓰기
- [x] io_uring Buffer Ring (`IORING_OP_PROVIDE_BUFFERS`) — 커널 버퍼 선택
- [x] Write timeout 타이머 (`Reactor::register_write_timeout` 헬퍼 — register_timer 기반)

### 추상 인터페이스 확장
- [x] `ITransport` — TLS 계층 주입점 (OpenSSL, mbedTLS, BoringSSL)
- [x] `ISessionStore` — 세션 저장소 추상화 (Redis, in-memory)

---

## v0.6.0 — Pipeline MVP (StaticPipeline)

> 최소 완성 경로: `Context → ServiceRegistry → AsyncChannel → Action → StaticPipeline`
> Linux 단일 플랫폼 기준. v0.5.0의 `Reactor::post` + `Dispatcher::spawn` 필수.
> 상태 관리 상세 설계: **[docs/pipeline-design.md §26-34](./docs/pipeline-design.md)**

### State Management 기반 (Pipeline보다 먼저 구현)
- [x] `Context` — 불변 persistent linked-list 아이템 컨텍스트
  - `put<T>(value)` → 새 Context 반환 (원본 불변)
  - `get<T>()` → `std::optional<T>`
  - `get_ptr<T>()` → `const T*` (복사 없는 참조)
  - 내장 슬롯: `TraceCtx`, `RequestId`, `AuthSubject`, `AuthRoles`, `Deadline`, `ActiveSpan`
- [x] `ServiceRegistry` — 스코프 기반 의존성 주입 컨테이너
  - `register_singleton<T>(shared_ptr<T>)` / `register_factory<T>(fn)`
  - 약한 의존성: `get<T>()` → nullptr if missing
  - 강한 의존성: `require<T>()` → terminate if missing (fail-fast)
  - 계층: `parent_` 포인터로 GlobalRegistry → PipelineRegistry fallback
  - `global_registry()` — 프로세스 싱글톤
- [x] `ContextualItem<T>` — `{T value; Context ctx}` 채널 전송 단위
- [x] `ActionEnv` — `{Context ctx; std::stop_token stop; size_t worker_idx}`
- [x] `WorkerLocal<T>` — `alignas(64)` vector + worker_idx 접근, 락 불필요
- [x] ⚠️ 코루틴 thread_local 경고 테스트 — co_await 경계에서 Context 전파 검증

### C++20 Concepts (타입 안전)
- [x] `ActionFn<Fn, In, Out>` concept — `FullActionFn` (ActionEnv 포함) + `SimpleActionFn` (stop_token만)
- [x] `BatchActionFn<Fn, In, Out>` concept — `span<In>` + `ActionEnv` 서명 검증
- [x] `PipelineInputFor<Pipeline, In>` concept — push(In) 서명 검증
- [x] `Action<In,Out>` 생성자에 concept 적용 → 명확한 컴파일 에러

### TaskGroup (구조적 동시성)
- [x] `TaskGroup::spawn(Task<Result<T>>)` — 자식 등록
- [x] `TaskGroup::join()` — 모두 완료 대기 (실패 시 나머지 cancel + 에러 전파)
- [x] `TaskGroup::join_all<T>()` — 결과 벡터 수집
- [x] 내부: `std::atomic<size_t> pending`, `std::stop_source`, cancel-on-first-error 옵션

### Layer 5: Pipeline 기반
- [x] `AsyncChannel<T>` — Dmitry Vyukov MPMC ring buffer
  - head_/tail_ cache-line 분리 (`alignas(64)`)
  - `send()` / `recv()` — backpressure: 포화/비면 co_await 대기
  - `try_send()` / `try_recv()` — lock-free, 논블로킹
  - `close()` + EOS 전파
  - waiter → `{Reactor*, coroutine_handle<>}` intrusive list
  - cross-reactor wakeup: `waiter.reactor->post([h]{h.resume();})`
- [x] `Stream<T>` — async pull 인터페이스 (next(), tee(), make_stream())
  - stream_map, stream_filter, stream_chunk, stream_take_while, stream_scan 연산자
  - `operator|` 파이프 문법 지원

### Action (정적)
- [x] `Action<In, Out>` — 코루틴 워커 풀
  - `Config`: min/max_workers, channel_cap, auto_scale, keyed_ordering, registry
  - `scale_to(n)` / `scale_in()` / `scale_out()`
  - scale-in: `atomic<size_t> target_workers` + 워커 인덱스 비교 (poison pill 미사용)
  - 처리 함수: **`Task<Result<Out>>(In, ActionEnv)`** — 예외 금지
  - ActionEnv 구성: `{ctx = upstream_item.ctx, stop, worker_idx}`
  - ContextualItem 언래핑/래핑: 채널 내부는 `ContextualItem<T>`, Action Fn은 `T`만 봄
  - Stateless / Immutable / Mutable(WorkerLocal) / External 4가지 패턴 지원
- [x] `BatchAction<In, Out>` — 최대 N개 아이템 묶음 처리

### StaticPipeline
- [x] `PipelineBuilder<In>` — `add<Out>(action)` 마다 새 타입 반환 (컴파일타임 체인)
  - `[[nodiscard]] build()` — `StaticPipeline<OrigIn, CurIn>` 반환
  - 타입 불일치 시 컴파일 에러
- [x] `StaticPipeline<In, Out>`
  - `start(Dispatcher&)` / `drain()` / `stop()`
  - `push(In)` (backpressure) / `try_push(In)` (논블로킹)
  - 파이프라인 상태 머신: Created → Starting → Running → Draining → Stopped
  - `IPipelineInput<T>` — 타입 소거 입력 인터페이스 (fan-out에 활용)
- [x] 통합 테스트: StaticPipeline 3단계 체인, scale-out, drain, backpressure

---

## v0.7.0 — 라이브러리 분리 + IO 프리미티브 + DynamicPipeline

> IO 레이어와 Pipeline 레이어는 병렬 작업 스트림.
> 라이브러리 분리 전략: **[docs/library-strategy.md](./docs/library-strategy.md)**
> IO 기술 심층: **[docs/io-deep-dive.md](./docs/io-deep-dive.md)**

### 라이브러리 분리 (CMake 타겟 재구조화)

- [ ] CMakeLists.txt 재구조화 — 9레벨 타겟 분리
  - `qbuem::result` (header-only) — `Result<T>`, `errc` 독립 분리
  - `qbuem::arena` (header-only) — `Arena`, `FixedPoolResource`, `BufferPool<N>` 독립
  - `qbuem::task` (header-only) — `Task<T>`, `awaiters` 독립
  - `qbuem::reactor` (static) — Reactor 인터페이스 + `TimerWheel` (구현 없음)
  - `qbuem::epoll` / `qbuem::kqueue` / `qbuem::iouring` — 플랫폼별 분리
  - `qbuem::dispatcher` (static) — Dispatcher 독립
- [ ] 헤더 이동: `include/qbuem/core/*` → `include/qbuem/reactor/*`
- [ ] `include/qbuem/net/`, `buf/`, `io/`, `transport/`, `codec/`, `server/` 신설
- [ ] `find_package(qbuem-stack COMPONENTS net buf pipeline ...)` COMPONENTS 지원
- [ ] 하위 호환 alias 유지: `qbuem-stack::core` → `qbuem::reactor` 등

### IO 프리미티브 (Layer 3 — Network Sockets)

- [x] `SocketAddr` — IPv4/IPv6/Unix 값 타입, zero-alloc, `to_chars()` (할당 없음)
- [x] `TcpListener` — `SO_REUSEPORT` bind, `accept()` coroutine
- [x] `TcpStream` — `readv()`/`writev()` scatter-gather, `set_nodelay()`/`set_keepalive()`
- [x] `UdpSocket` — `sendto()`/`recvfrom()`, `recvmsg_batch()` (io_uring RECVMSG_MULTI)
- [x] `UnixSocket` — `AF_UNIX` SOCK_STREAM / SOCK_DGRAM

### IO 버퍼 / 슬라이스 (Layer 4 — Zero-copy Buffer)

- [x] `IOSlice` — `{const byte*, size_t}` fat pointer (zero-alloc)
- [x] `IOVec<N>` — 스택 할당 scatter-gather 배열 (wraps `iovec[N]`), `writev()` 직접 전달
- [x] `ReadBuf<N>` — 컴파일타임 고정 링버퍼, `write_head()`/`commit()`/`consume()`, zero-alloc
- [x] `WriteBuf` — Arena 기반 코르크 버퍼, `as_iovec()` → 단일 `writev()` 시스템콜
- [x] `BufferPool<BufSize, Count>` — `FixedPoolResource` 위 io_uring Buffer Ring 연동 래퍼

### Zero-copy IO (Layer 4b)

- [x] `zero_copy::sendfile()` — `sendfile(2)` 정적 파일 서빙 (kernel space only)
- [x] `zero_copy::splice()` — pipe 기반 fd→fd 전송 (generic)
- [x] `zero_copy::send_zerocopy()` — `MSG_ZEROCOPY` 송신 (Linux 4.14+)
- [x] `zero_copy::wait_zerocopy()` — errqueue 완료 대기

### AsyncFile (Layer 4c)

- [x] `AsyncFile` — 비동기 open/read_at/write_at/close
  - io_uring `IORING_OP_READ_FIXED` 우선, 없으면 `pread/pwrite` 폴백
  - `O_DIRECT` 지원 (정렬 버퍼 필수)

### PlainTransport (Layer 5 확장)

- [x] `PlainTransport` — `ITransport` 구체 TCP 구현체 (TLS 없음)
  - 서비스에서 OpenSSL/mbedTLS `ITransport` 구현 주입 패턴 예제 추가

### TimerWheel (Layer 2 교체)

- [x] `TimerWheel` — 4레벨 × 256슬롯 계층적 타이밍 휠
  - `schedule(delay_ms, fn)` O(1) / `cancel(id)` O(1) / `tick(elapsed_ms)` O(만료수)
  - `Entry` 할당: `FixedPoolResource<sizeof(Entry)>` — zero-heap
  - `next_expiry_ms()` — `poll()` timeout 계산에 사용
  - `Reactor::register_timer()` 내부 구현을 TimerWheel로 교체

### io_uring 고급 기능 (Linux 5.1+)

> 상세: **[docs/io-deep-dive.md §2](./io-deep-dive.md)**

- [ ] io_uring 직접 RECV/SEND SQE — POLL_ADD 기반에서 실제 비동기 I/O 제출로 전환
  - `IORING_OP_RECV` + Buffer Ring → recv() syscall 제거
  - `IORING_OP_SEND` → send() syscall 제거
- [ ] `IORING_OP_ACCEPT_MULTISHOT` (Linux 5.19+) — SQE 1회로 다중 연결 수락
  - 연결마다 SQE 재제출 없음 → high-concurrency accept 오버헤드 제거
- [ ] `IORING_OP_RECV_MULTISHOT` (Linux 5.19+) — SQE 1회로 다중 패킷 수신
  - Buffer Ring 자동 선택과 결합 → zero-copy recv 완성
- [ ] io_uring Fixed Files — `io_uring_register_files()` + `IOSQE_FIXED_FILE`
  - SQE마다 fd 테이블 조회 제거 → 고연결 환경 오버헤드 감소
- [ ] io_uring Linked SQEs — `IOSQE_IO_LINK` / `IOSQE_IO_HARDLINK`
  - 읽기→쓰기 원자적 체인 → 프록시/파이프 전달에 활용
- [ ] `IORING_OP_SOCKET` + `IORING_OP_CONNECT` — 소켓 생성/연결도 io_uring으로
  - `ConnectionPool::acquire()` 신규 연결 경로에 적용

### 소켓 고급 옵션 (신규)

- [ ] `SO_INCOMING_CPU` (Linux 3.19+) — 연결을 특정 CPU reactor에 고정
  - SO_REUSEPORT와 조합 → L1/L2 캐시 적중률 극대화
- [ ] `TCP_MIGRATE_REQ` (Linux 5.14+) — SO_REUSEPORT 그룹 내 연결 마이그레이션
  - 무중단 worker 재시작에 활용

### Codec / Framing (Layer 6)

- [x] `IFrameCodec<Frame>` — `decode(span<byte>, Frame&)` / `encode(Frame, IOVec<16>&, Arena&)`
- [x] `LengthPrefixedCodec<Header>` — N바이트 길이 헤더 프레임
- [x] `LineCodec` — `\n` / `\r\n` 구분 (RESP, SMTP 등)
- [x] `Http1Codec` — 기존 `http::Parser` 래핑, `IFrameCodec<http::Request>` 구현

### Connection Lifecycle (Layer 7)

- [x] `IConnectionHandler<Frame>` — `on_connect()` / `on_frame()` / `on_disconnect()`
- [x] `AcceptLoop<Frame, HandlerFactory>` — SO_REUSEPORT coroutine 루프
  - reactor당 독립 `TcpListener` → accept 경합 없음
  - 각 연결 → `Dispatcher::spawn(handle_connection(...))`
- [x] `ConnectionPool<T>` — 아웃바운드 풀 (min_idle, max_size, health_check, idle_timeout)
  - O(1) hot path `acquire()`, RAII `ReturnToPool` deleter

---

### DynamicPipeline
- [x] `IDynamicAction` — 타입 소거 Action 인터페이스
  - `ActionSchema { input_type, output_type }` — 런타임 스키마 호환성 체크
  - `process_erased(void*, void*, stop_token)` — 타입 소거 처리
- [x] `make_dynamic_action<In,Out>(Action)` — 정적 Action → 동적 어댑터
- [x] `DynamicPipeline`
  - `add_action()` / `insert_before()` / `insert_after()` / `remove_action()` — stopped 상태
  - 상태 머신: Created → Configured → Starting → Running → Reconfiguring → Draining → Stopped
  - `start()` / `drain()` / `stop()`

### PipelineGraph
- [x] `PipelineGraph` — DAG 오케스트레이션
  - `add(name, pipeline)` — Static/Dynamic 모두 지원 (타입 소거)
  - `connect()` / `fan_out()` / `merge_into()` / `route_if()`
  - `start()` — Kahn's algorithm으로 사이클 감지 + 위상 정렬 후 순서대로 시작
  - `drain_all()` / `stop_all()`
  - A/B 라우팅: `ab_route(from, target_a, target_b, b_fraction)`

### MessageBus
- [x] `MessageBus` — gRPC 스타일 4가지 메시지 패턴
  - **Unary**: `RequestEnvelope<Req,Res>` = `{request, shared_ptr<AsyncChannel<Result<Res>>>}`
  - **Server Streaming**: `ServerStreamEnvelope<Req,Res>`
  - **Client Streaming**: `ClientStreamEnvelope<Req,Res>`
  - **Bidirectional**: `BidiEnvelope<Req,Res>` (두 방향 독립 채널)
  - `create_*()` 등록 / 이름 기반 채널 접근 / DLQ 접근

### 관찰 가능성 기반
- [x] `ActionMetrics` — items_processed, errors, retried, dlq, latency_buckets (4구간)
- [x] `PipelineMetrics` — 파이프라인 단위 집계
- [x] `PipelineObserver` 훅 인터페이스
  - `on_item_start/done`, `on_error`, `on_scale_event`, `on_state_change`
  - `on_dlq_item`, `on_circuit_open/close`
- [x] `LoggingObserver` 기본 구현

---

## v0.8.0 — 복원력 + 분산 트레이싱 + Zero-copy 고도화

### Zero-copy 고도화 및 메모리 최적화

> 상세: **[docs/io-deep-dive.md §5-6](./io-deep-dive.md)**

- [ ] kTLS (Kernel TLS) 통합 — `setsockopt(SOL_TLS, TLS_TX/RX)` 지원
  - `kTLSTransport` — ITransport 구현체, TLS 핸드셰이크 후 키 커널 전달
  - kTLS + `sendfile()` 조합 → TLS 연결에서도 정적 파일 zero-copy 가능
- [ ] `IORING_OP_SENDMSG_ZC` (Linux 6.0+) — io_uring zero-copy send
  - 두 CQE 패턴(전송 시작 + completion notification) 처리
  - `zero_copy::` 모듈에 io_uring 경로 추가
- [x] Huge Pages 버퍼 풀 — `mmap(MAP_HUGETLB)` 기반 `HugeBufferPool<N, Count>`
  - TLB miss 감소 → 고스루풋 IO에서 5-15% 성능 향상
  - `/proc/sys/vm/nr_hugepages` 확인, 폴백 처리
- [x] `mmap` 기반 Arena — `madvise(MADV_DONTNEED)` reset (OS 반환 없음)
- [x] `madvise(MADV_FREE)` — 연결 종료 시 버퍼 lazy 반환
- [ ] Prefetch 힌트 — `__builtin_prefetch` 연결 구조체 선제 로드

### 복원력 패턴
- [x] `RetryPolicy` — Fixed / Exponential / ExponentialJitter backoff
  - `max_attempts`, `base_delay`, `max_delay`, `deadline`, `retryable_errors`
- [x] `CircuitBreaker` — Closed / Open / HalfOpen 상태 머신
  - `failure_threshold`, `success_threshold`, `open_duration`
  - Open 상태 아이템 → 즉시 DLQ (처리 시도 없음)
- [x] `DeadLetter<T>` — `{item, error_code, attempt_count, failed_at}`
  - `DeadLetterQueue`: `MessageBus` 채널명으로 접근
- [ ] Bulkhead: `channel_cap` 기반 자동 backpressure (별도 구현 불필요, 문서화)

### 분산 트레이싱 (OpenTelemetry 호환)
- [x] `TraceContext` — W3C Trace Context 표준
  - `trace_id[16]` (128-bit) / `span_id[8]` (64-bit) / `trace_flags`
  - `generate()` / `child_span()` / `to_traceparent()` / `from_traceparent()`
- [x] **Context 슬롯 기반 TraceContext 전파** (§27 경고 참조 — thread_local은 코루틴에서 위험)
  - `TraceCtx` Context 슬롯 → `ActionEnv.ctx.get<TraceCtx>()`로 접근
  - `ActiveSpan` Context 슬롯 → Action이 child span 생성 후 ctx에 추가
- [x] Pluggable `Sampler` 인터페이스
  - `AlwaysSampler` / `NeverSampler`
  - `ProbabilitySampler(rate)` — 0.0~1.0
  - `RateLimitingSampler(max_per_second)` — token bucket
  - `ParentBasedSampler` — 부모 결정 따름
- [x] `SpanExporter` 인터페이스 + `SpanData` (pipeline, action, context, timing, error)
  - `LoggingExporter` — 디버그 기본 구현
  - `NoopExporter` — 트레이싱 비활성화 시 zero-overhead
  - `OtlpGrpcExporter` / `OtlpHttpExporter` — OpenTelemetry Collector
  - `JaegerExporter` / `ZipkinExporter`
- [x] `PipelineTracer` — `start_span()` / `end_span()`, 전역 등록 (`set_global_tracer`)
- [x] HTTP 통합: `traceparent` 헤더 자동 파싱 → thread-local 설정 → 응답 `traceresponse`
- [x] `IMetricsExporter` — Prometheus push 추상화

---

## v0.9.0 — Pipeline 고도화

### Hot-swap (무중단 액션 교체)
- [x] `DynamicPipeline::hot_swap(name, new_action, timeout)`
  - Seal → Drain → Swap → Resume 절차
  - 타임아웃 초과 시 `errc::timed_out`
  - 스키마 불일치 시 `errc::invalid_argument`
  - Running 상태 아닌 경우 `errc::operation_not_permitted`

### 우선순위 채널
- [x] `PriorityChannel<T>` — High / Normal / Low 3레벨
  - recv: High 소진 → Normal 소진 → Low 순서 보장
  - Aging: Low가 N회 연속 skip 시 강제 처리 (스타베이션 방지)
  - `set_aging_threshold(n)` — 기본 100

### Config-driven Pipeline
- [x] `PipelineFactory` — JSON/YAML → `DynamicPipeline` / `PipelineGraph` 생성
  - `register_plugin(name, factory)` — 코드 or .so 플러그인 등록
  - `from_json()` / `from_yaml()` / `graph_from_json()`

### Pipeline 합성
- [x] `SubpipelineAction<In,Out>` — `StaticPipeline<In,Out>`을 `Action<In,Out>`처럼 내장
  - 재사용성: 공통 처리 로직 캡슐화
  - 테스트 용이성: inner pipeline mock 교체 가능

### Arena 통합
- [x] reactor-local `FixedPoolResource<sizeof(PipelineItem<T>)>` 아이템 할당
  - malloc/free 제거, 캐시 효율 극대화
  - `ArenaChannel<T>` — 동일 reactor 내 zero-copy 전달

### SPSC Channel (고성능 1:1 경로)
- [x] `SpscChannel<T>` — Lamport Queue (head_/tail_ alignas(64) 분리)
  - `try_push()` / `try_pop()` — wait-free O(1)
  - `send()` / `recv()` — async blocking
  - `Action::Config::min_workers==1 && max_workers==1` → 자동 선택

### Batch 연산
- [x] `AsyncChannel<T>::try_recv_batch(span<T> out, size_t max_n)` — lock-free 배치 dequeue
- [x] `AsyncChannel<T>::send_batch(span<T> items)` — 배치 enqueue
- [x] `BatchAction<In, Out>` — `span<In>` 단위 처리 (DB bulk insert 등)

### 스트림 연산자 (Rx-style)
- [x] `stream_map`, `stream_filter`, `stream_flat_map` — 기본 변환
- [x] `stream_zip`, `stream_merge` — 멀티 스트림 결합
- [x] `stream_chunk(n)` — N개씩 묶어 vector로 (BatchAction 입력용)
- [x] `stream_take_while`, `stream_scan` — 상태 유지 변환
- [x] `operator|` 파이프 문법 지원

### 이벤트 처리 고급 패턴
- [x] `DebounceAction<T>` — gap_duration 이후 마지막 아이템만 처리
- [x] `ThrottleAction<T>` — token bucket 기반 처리 속도 제한
- [x] `ScatterGatherAction<In,SubIn,SubOut,Out>` — 병렬 서브작업 후 결과 집계
  - `ScatterFn`, `ProcessFn`, `GatherFn`, `max_parallelism` 설정

### 성능 최적화
- [ ] Reactor / Connection 구조체 cache-line 패킹 측정 및 최적화
- [ ] `__builtin_prefetch` — 다음 Connection 구조체 미리 로드
- [ ] 2KB 이하 요청 헤더 스택 할당 (힙 회피)
- [x] `MSG_ZEROCOPY` (`SO_ZEROCOPY`) — 송신 kernel→user 복사 제거
- [x] PGO 2-pass 빌드 가이드 (Instrumented → wrk → Optimized)

---

## v0.9.1 — 신뢰성 & 고급 처리 패턴

### Windowing & Event-time Processing
- [x] `EventTime { system_clock::time_point }` Context 슬롯
- [x] `Watermark` — out-of-order 이벤트 처리 진행 신호
- [x] `TumblingWindow` / `SlidingWindow` / `SessionWindow` 구조체
- [x] `WindowedAction<T,Acc,Out>` — key 기반 시간 창 집계
  - per-key state map (WorkerLocal 기반), watermark 도달 시 emit

### Saga & 보상 트랜잭션
- [x] `SagaStep<In,Out>` — execute + compensate 쌍
- [x] `SagaOrchestrator<T>` — 순차 실행, 실패 시 역순 compensate
  - 보상 실패 → `saga_compensation_failures` DLQ 기록
- [x] Context에 `SagaId` 슬롯 추가 (분산 추적 연동)

### Exactly-once
- [x] `IdempotencyKey { std::string }` Context 슬롯
- [x] `IIdempotencyStore` — `get()` / `set_if_absent(key, ttl)` 인터페이스
- [x] `IdempotencyFilter<T>` — 중복 아이템 skip Action

### Checkpoint / Snapshot
- [x] `ICheckpointStore` — `save(pipeline, offset, metadata_json)` / `load(pipeline)`
- [x] `DynamicPipeline::enable_checkpoint(store, every_n, every_t)`
- [x] `DynamicPipeline::resume_from_checkpoint()`

### SLO Tracking & Error Budget
- [x] `SloConfig` — `p99_target`, `p999_target`, `error_budget`, `on_violation`
- [x] `ErrorBudgetTracker` — rolling window 에러율 + budget 소진 시 CB 강제 Open
- [x] `Action::Config::slo` 필드 추가
- [x] 위반 시 `PipelineObserver::on_slo_violation()` 콜백

### Pipeline Health & Topology
- [x] `PipelineHealth` — HEALTHY/DEGRADED/UNHEALTHY per pipeline
- [x] `ActionHealth` — circuit_state, error_rate_1m, p99_1m, queue_depth
- [x] `App::health_check_detailed()` 응답에 pipeline 상태 포함
- [x] `PipelineGraph::to_json()` / `to_dot()` / `to_mermaid()` — 위상 Export
- [x] `/pipeline/topology` 엔드포인트 (App 통합)

### Canary 자동화
- [x] `CanaryRouter::start_gradual_rollout(Config)` — 1%→5%→25%→100% 단계별
- [x] 자동 롤백 조건: error_delta 초과 / P99 초과 / budget 소진
- [x] `rollback_to_stable()` 수동 롤백

---

## v0.9.2 — 인프라 고도화 + NUMA + 성능 측정

### NUMA-aware 스케줄링
- [x] `Dispatcher::pin_reactor_to_cpu(idx, cpu_id)` — pthread_setaffinity_np
- [x] `Dispatcher::auto_numa_bind()` — NUMA 노드별 reactor 그룹 자동 배치
- [ ] reactor-local Arena를 같은 NUMA 노드 메모리에서 할당 (mbind(2) / numa_alloc_local)
- [ ] `SO_INCOMING_CPU` NUMA 그룹 기반 설정 — 연결→CPU→NUMA 완전 고정

### 성능 프로파일링 통합
- [x] `PerfCounters` — PMU 이벤트 (cycles, instructions, LLC-miss, branch-miss)
- [ ] eBPF 트레이싱 가이드 — io_uring tracepoints, tcp_sendmsg kprobe
- [x] PGO (Profile-Guided Optimization) 2-pass 빌드 CMake 지원
  - `QBUEM_PGO_GENERATE=ON` → instrumented 빌드
  - `QBUEM_PGO_USE=ON` → 프로파일 기반 최적화 빌드
- [ ] `IORING_OP_FUTEX_WAIT/WAKE` (Linux 6.7+) — eventfd 대체 wakeup

### Pipeline Versioning & Schema Evolution
- [x] `PipelineVersion { major, minor, patch }` — compatible_with() 검사
- [x] `PipelineGraph::set_version(name, version)` — 버전 메타데이터 등록
- [x] `MigrationFn<OldT, NewT>` — 타입 마이그레이션 함수
- [x] `DlqReprocessor::register_migration()` — DLQ 재처리 시 마이그레이션 적용
- [x] 점진적 타입 변경 가이드: MigrationAction 삽입 → 병렬 운영 → 제거

---

## v1.0.0 — 프로토콜 핸들러

> 전제: v0.7.0 IO 프리미티브 (SocketAddr, TcpListener, TcpStream, IFrameCodec, AcceptLoop) 완료

### Protocol Handlers

- [x] `Http1Handler` — `IConnectionHandler<http::Request>` 구현
  - keep-alive 자동 처리, 100-continue, chunked transfer
  - `Router` 주입, `Upgrade` 헤더 처리 (→ WebSocket upgrade)
- [ ] `Http2Handler` — `IConnectionHandler<Http2Frame>` 구현
  - HPACK 헤더 압축/해제 (Arena 기반, zero-alloc)
  - 스트림 멀티플렉싱: `AsyncChannel<Http2Frame>` per stream
  - SETTINGS / WINDOW_UPDATE / PING / GOAWAY 지원
  - ALPN "h2" 협상 후 Http1Codec → Http2Codec 자동 전환
- [x] `WebSocketHandler` — RFC 6455 구현
  - HTTP/1.1 Upgrade 검증 → 101 Switching Protocols
  - Masking/Unmasking (Arena 기반, zero-alloc)
  - PING/PONG keepalive, CLOSE handshake
- [x] `GrpcHandler<Req, Res>` — HTTP/2 위 gRPC 구현
  - protobuf 직접 의존 없음 — 서비스에서 serialize/deserialize 제공
  - Unary / Server Streaming / Client Streaming / Bidi 4가지 패턴
  - `Stream<Res>` / `AsyncChannel<Req>` 직접 연결
- [ ] HTTP/3 / QUIC — quiche FFI 추상화 (별도 `ITransport` 구현체)
- [ ] `AF_XDP` eXpress Data Path — 극한 성능, 별도 레이어 (선택적)

### gRPC ↔ Pipeline 통합

- [ ] gRPC 서버 스트리밍 → `Stream<T>` 직접 연결
- [ ] gRPC 클라이언트 스트리밍 → `AsyncChannel<T>` 직접 연결
- [ ] `BidiEnvelope<Req,Res>` → gRPC bidi 핸들러 어댑터

### 극한 성능 (선택적)

- [ ] AF_XDP + UMEM — 커널 네트워크 스택 우회, 10-100M PPS 목표
  - `qbuem::xdp` 별도 라이브러리 (libbpf 의존, 선택적)
  - UMEM: `BufferPool` 기반 관리, Huge Pages 사용
  - XDP 프로그램 로드: `load_xdp_prog()` BPF bytecode
  - 적용처: 게임 서버 UDP, 고속 QUIC, 패킷 캡처
- [ ] QUIC/HTTP3 — quiche FFI `ITransport` 구현체 레퍼런스 예제
  - 0-RTT, HOL blocking 없음, 연결 마이그레이션
  - `QuicheTransport` 구현 가이드 (서비스에서 구현)

---

## 구현 의존성 그래프

```
[0] Reactor::post() + Dispatcher::spawn()           ← v0.5.0 ✓
         │
         ├──────────────────────────────────────────┐
         │                                          │
         ▼                                          ▼
[S] Context + ServiceRegistry + ActionEnv       [IO-0] TimerWheel O(1)        ← v0.7.0
    WorkerLocal<T> + ContextualItem<T>  ← v0.6.0✓  │
         │                                          │
         ▼                                      [IO-1] SocketAddr + TcpListener
[1] AsyncChannel<T>                  ← v0.6.0✓     TcpStream + UdpSocket      ← v0.7.0
         │                                          │
    ┌────┴────┐                               [IO-2] IOSlice + IOVec<N>
    ▼         ▼                                    ReadBuf<N> + WriteBuf       ← v0.7.0
[2a] Stream<T>  [2b] RetryPolicy/CB/DLQ            BufferPool<N>
    │         │                                     zero_copy::sendfile/splice ← v0.7.0
    └────┬────┘                                     AsyncFile                  ← v0.7.0
         ▼                                          │
[3] Action<In,Out>  IDynamicAction   ← v0.6.0/v0.7.0
         │                                      [IO-3] IFrameCodec<F>
    ┌────┴────┐                                     LengthPrefixedCodec        ← v0.7.0
    ▼         ▼                                     LineCodec + Http1Codec     ← v0.7.0
[4a] StaticPipeline  [4b] DynamicPipeline ← v0.6.0/v0.7.0
[4c] PipelineObserver/Metrics (병행)               │
         │                                      [IO-4] AcceptLoop (SO_REUSEPORT)
    ┌────┴────┐                                     IConnectionHandler<Frame>  ← v0.7.0
    ▼         ▼                                     ConnectionPool<T>          ← v0.7.0
[5a] PipelineGraph  [5b] MessageBus  ← v0.7.0      │
[5c] PipelineFactory                 ← v0.9.0       │
         │                                          │
         ▼                                          ▼
[6] PipelineTracer + SpanExporter    ← v0.8.0  [IO-5] Http1Handler            ← v1.0.0
[7] hot_swap + PriorityChannel + Arena ← v0.9.0     Http2Handler (HPACK)
[8] IO-5: Protocol Handlers          ← v1.0.0       WebSocketHandler
                                                     GrpcHandler<Req,Res>
```

**병렬 작업 스트림:**
- `[S]→[1]→[2]→[3]→[4]→[5]`: Pipeline 레이어 (기존 경로)
- `[IO-0]→[IO-1]→[IO-2]→[IO-3]→[IO-4]→[IO-5]`: IO 레이어 (신규)
- 두 스트림은 v1.0.0에서 `AcceptLoop + Http1Handler + Pipeline` 결합으로 합류

**라이브러리 분리 (9레벨):**
```
Level 0: result · arena · crypto                    (header-only)
Level 1: task · reactor · dispatcher                (header-mostly)
Level 2: epoll · kqueue · iouring                   (platform, static)
Level 3: net · buf · file · zerocopy               (IO, static)
Level 4: transport · codec · server                 (static)
Level 5: http · http-server                         (static)
Level 6: context · channel · pipeline               (header-only)
Level 7: pipeline-graph · resilience · tracing · metrics (header-only)
Level 8: ws · http2 · grpc                          (static)
Level 9: qbuem (umbrella)
```

---

## 배포 전략

> 상세: **[docs/library-strategy.md](./docs/library-strategy.md)**

```
# 라이브러리 단위 소비 (CMake COMPONENTS)
find_package(qbuem-stack REQUIRED COMPONENTS net buf pipeline)

qbuem-stack (이 레포, 단일 모노레포)
├── qbuem::result          — Result<T>, errc (header-only, zero-dep)
├── qbuem::arena           — Arena, FixedPoolResource (header-only)
├── qbuem::task            — Task<T>, awaiters (header-only)
├── qbuem::reactor         — Reactor 인터페이스 + TimerWheel
├── qbuem::epoll/kqueue/iouring — 플랫폼별 구현
├── qbuem::dispatcher      — 다중 Reactor 관리
├── qbuem::net             — SocketAddr, TcpListener, TcpStream, UdpSocket
├── qbuem::buf             — IOSlice, IOVec<N>, ReadBuf<N>, WriteBuf (header-only)
├── qbuem::file            — AsyncFile (io_uring/pread 폴백)
├── qbuem::zerocopy        — sendfile, splice, MSG_ZEROCOPY, kTLS 연동
├── qbuem::transport       — ITransport, PlainTransport
├── qbuem::codec           — IFrameCodec, LengthPrefixed, LineCodec (header-only)
├── qbuem::server          — AcceptLoop, IConnectionHandler, ConnectionPool
├── qbuem::http            — HTTP/1.1 파서 + 라우터
├── qbuem::http-server     — Http1Handler, App, Middleware 체인
├── qbuem::context         — Context, ServiceRegistry, ActionEnv (header-only)
├── qbuem::channel         — AsyncChannel<T>, Stream<T>, TaskGroup (header-only)
├── qbuem::pipeline        — Action, StaticPipeline, PipelineBuilder (header-only)
├── qbuem::pipeline-graph  — DynamicPipeline, PipelineGraph, MessageBus
├── qbuem::resilience      — RetryPolicy, CircuitBreaker, DLQ (header-only)
├── qbuem::tracing         — TraceContext, SpanExporter (header-only)
├── qbuem::metrics         — ActionMetrics, PipelineObserver (header-only)
├── qbuem::ws              — WebSocketHandler
├── qbuem::http2           — Http2Handler, HPACK zero-alloc
├── qbuem::grpc            — GrpcHandler<Req,Res>
├── qbuem::xdp             — AF_XDP (선택적, libbpf 의존)
└── qbuem                  — 전체 umbrella

외부 통합 (서비스에서 ITransport/IBodyEncoder 등 구현 주입):
├── [서비스]-tls-transport — OpenSSL/mbedTLS/BoringSSL/kTLS ITransport 구현
├── [서비스]-quic-transport — quiche FFI ITransport 구현 (QUIC/HTTP3)
├── [서비스]-gzip-encoder  — IBodyEncoder (zlib/brotli/zstd)
├── [서비스]-jwt-verifier  — ITokenVerifier
├── [서비스]-otlp-exporter — SpanExporter (OpenTelemetry)
└── qbuem-json             — JSON 파싱 (별도 레포)
```

**Zero-dependency 원칙:**
```
의존성         헤더 노출  구현 노출  비고
─────────────────────────────────────────────────────────
POSIX sockets   YES        YES        항상 사용 가능
Linux io_uring  NO(pimpl)  YES(.cpp)  선택적, epoll 폴백
liburing        NO(pimpl)  YES(.cpp)  CMake QBUEM_IOURING=ON
libbpf/AF_XDP   NO         YES(.cpp)  qbuem::xdp 별도, 선택적
OpenSSL/TLS     NO         NO         서비스 ITransport 주입
nghttp2         NO         NO         미사용 (자체 HPACK 구현)
protobuf        NO         NO         서비스 직렬화 주입
quiche/ngtcp2   NO         NO         서비스 QuicTransport 주입
JSON            NO         NO         qbuem-json 별도 레포
zlib/brotli     NO         NO         서비스 IBodyEncoder 주입
```

---

## 라이선스 참고

- liburing (LGPL-2.1) — 동적 링크 권장
- OpenSSL 3.x (Apache-2.0) — 서비스에서 직접 링크, qbuem-stack은 링크 안 함
- qbuem-json (MIT) — 서비스에서 직접 링크, tests에서만 사용
