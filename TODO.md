# qbuem-stack Roadmap

**Zero Latency · Zero Cost · Zero Dependency**

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
- [x] `draco.hpp`, `crypto.hpp`, `url.hpp` 완료

### Layer 0: Common
- [x] `Result<T>` + `unexpected<E>` — C++20 예외 없는 에러 처리
- [x] `FixedPoolResource<Size>` — O(1) slab allocator (embedded free-list)
- [x] `MonotonicBufferResource` — per-request arena
- [x] `draco::constant_time_equal()` — timing-safe 비교
- [x] `draco::csrf_token()` — CSPRNG 기반 128-bit 토큰
- [x] `draco::random_bytes()` — getrandom(2) / arc4random_buf()

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

## 진행 예정

### 단기 (v0.4.0)

#### IO Core 고도화
- [ ] io_uring SQPOLL 모드 (`IORING_SETUP_SQPOLL`) — steady-state syscall 0
- [ ] io_uring Fixed Buffers (`io_uring_register_buffers`) — DMA 직접 쓰기
- [ ] io_uring Buffer Ring (`IORING_OP_PROVIDE_BUFFERS`) — 커널 버퍼 선택
- [ ] `SO_REUSEPORT` per-reactor accept socket — accept storm 제거
- [ ] macOS `EVFILT_USER` 기반 wakeup (cross-thread notification)

#### HTTP
- [ ] Write timeout 타이머 (응답 전송 최대 시간, 현재 SO_SNDTIMEO로 대체)
- [ ] Drain mode: 신규 accept 중단 후 기존 연결 자연 종료 대기
- [ ] HTTP Trailer 지원 (Chunked Transfer 종료 후 헤더 추가)

#### Middleware
- [ ] `next()` 기반 async 미들웨어 (`AsyncMiddleware` 타입)
- [ ] Structured logging — remote_addr, request_id, trace_id 포함
- [ ] 동적 Rate Limit (IP별 설정 오버라이드)

### 중기 (v0.5.0)

#### 추상 인터페이스 확장
- [ ] `ITransport` — TLS 계층 주입점 (OpenSSL, mbedTLS, BoringSSL)
- [ ] `ISessionStore` — 세션 저장소 추상화 (Redis, in-memory)
- [ ] `IMetricsExporter` — Prometheus push 추상화

#### 성능
- [ ] Reactor / Connection 구조체 cache-line 패킹 측정 및 최적화
- [ ] `__builtin_prefetch` — 다음 Connection 구조체 미리 로드
- [ ] 2KB 이하 요청 헤더 스택 할당 (힙 회피)
- [ ] `MSG_ZEROCOPY` (`SO_ZEROCOPY`) — 송신 kernel→user 복사 제거
- [ ] PGO 2-pass 빌드 가이드 (Instrumented → wrk → Optimized)

### 장기 (v1.0.0)

- [ ] HTTP/2 (nghttp2 추상화, `IHttp2Handler`)
- [ ] WebSocket (RFC 6455, `IWebSocketHandler`)
- [ ] HTTP/3 / QUIC (quiche FFI 추상화)
- [ ] gRPC (protobuf 추상화)
- [ ] `AF_XDP` eXpress Data Path (극한 성능, 별도 레이어)

---

## 배포 전략

```
qbuem-stack (이 레포)
├── qbuem-stack::core      — IO 레이어만 (임베디드, 게임 서버 등)
├── qbuem-stack::http      — HTTP 파서 + 라우터만
└── qbuem-stack::qbuem     — 전체 (대부분의 서비스)

외부 통합 (서비스에서 직접 구현):
├── qbuem-json             — JSON (권장)
├── [서비스]-gzip-encoder  — IBodyEncoder 구현체 (zlib/brotli/zstd)
├── [서비스]-jwt-verifier  — ITokenVerifier 구현체 (OpenSSL/mbedTLS)
└── [서비스]-tls-transport — ITransport 구현체 (예정)
```

---

## 라이선스 참고

- liburing (LGPL-2.1) — 동적 링크 권장
- OpenSSL 3.x (Apache-2.0) — 서비스에서 직접 링크, qbuem-stack은 링크 안 함
- qbuem-json (MIT) — 서비스에서 직접 링크, tests에서만 사용
