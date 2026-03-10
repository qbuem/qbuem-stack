# Draco WAS Development Roadmap (v0.2.0 → 1.0.0)

Draco WAS는 **Zero Latency · Zero Cost · Low Memory · Low CPU** 를 4대 핵심 원칙으로 삼는 C++23 초고성능 Web Application Server 라이브러리입니다.

> **표기 규칙**
> - `[Common]` — Linux / macOS 공통 작업
> - `[Linux]`  — Linux 전용 (io_uring / epoll / sendfile 등)
> - `[macOS]`  — macOS 전용 (kqueue / GCD 등)

---

## ✅ Phase 0–2: Foundation & Core Engine (완료)

- [x] `[Common]` Expert architecture design (Shared-Nothing, io_uring, Coroutines)
- [x] `[Common]` C++23 project foundation & directory structure
- [x] `[Common]` Dependency management via `FetchContent` (Beast-JSON, GTest)
- [x] `[Linux]`  **Reactor Core**: `epoll` reactor 구현
- [x] `[macOS]`  **Reactor Core**: `kqueue` reactor 구현
- [x] `[Common]` **Dispatcher**: Thread-per-core Dispatcher with CPU affinity
- [x] `[Common]` **HTTP Abstractions**: Zero-copy HTTP Parser, `Request`/`Response` API
- [x] `[Common]` **Modern Routing**: Radix Tree for fast route matching
- [x] `[Common]` **Library Infrastructure**: Header + source distribution (modern CMake)

## ✅ Phase 3: Coroutines & Beast JSON (완료)

- [x] `[Common]` Custom `draco::Task<T>` with symmetric transfer for nested coroutines
- [x] `[Common]` Beast JSON native integration in `Request`/`Response`
- [x] `[Common]` `SafeValue` / `.get()` pattern for untrusted JSON data

## ✅ Phase 4: Async I/O & Hardening (완료)

- [x] `[Common]` Multi-event support (Read/Write) per FD
- [x] `[Common]` `AsyncRead`, `AsyncWrite`, `AsyncSleep` awaiters
- [x] `[Linux]`  `EpollReactor` with `epoll` + `timerfd`
- [x] `[Common]` Callback safety — copy-before-invoke, preventing UAF
- [x] `[Common]` `Task::detach()` fire-and-forget coroutine lifecycle
- [x] `[Common]` Async handler hardening with corrected `detach()` semantics
- [x] `[Common]` Platform-specific conditional compilation (kqueue / epoll)
- [x] `[Common]` Test suite: `http_test.cpp`, `reactor_test.cpp`

---

## 🔴 Phase 5: io_uring 완성 & 네이티브 Zero-Copy I/O

> 목표: Linux syscall overhead를 정상 상태(steady-state)에서 **0**으로 줄이기

### Linux 전용

- [ ] `[Linux]`  **io_uring Reactor 완성**: `io_uring_reactor.cpp` stub → 실제 구현
  - [ ] `io_uring_setup` / `io_uring_enter` 기반 event loop
  - [ ] SQE/CQE 배치 처리 (Submission Queue Batching)
  - [ ] `IORING_OP_READ`, `IORING_OP_WRITE`, `IORING_OP_ACCEPT` 지원
  - [ ] `IORING_OP_TIMEOUT` 으로 `AsyncSleep` 구현
  - [ ] **Fixed buffers** (`io_uring_register_buffers`) — kernel이 직접 user buffer에 DMA write
  - [ ] **Buffer Ring** (`IORING_OP_PROVIDE_BUFFERS`) — kernel이 버퍼 선택, copy 제로
  - [ ] **SQPOLL 모드** (`IORING_SETUP_SQPOLL`) — kernel thread가 SQ 폴링, 정상 상태 syscall 0
  - [ ] Kernel 5.19+ `IORING_FEAT_CQE_SKIP` 활용
  - [ ] `SO_REUSEPORT` + `IORING_OP_ACCEPT_DIRECT` 다중 수신 소켓
- [ ] `[Linux]`  **`MSG_ZEROCOPY`** (`SO_ZEROCOPY`) — 송신 시 kernel→user 복사 제거, CQE 완료 알림
- [ ] `[Linux]`  **`sendfile(2)`** — 정적 파일 서빙 zero-copy 전송
- [ ] `[Linux]`  **`splice(2)`** — pipe 경유 소켓→소켓 zero-copy 프록시
- [ ] `[Linux]`  **`SO_REUSEPORT`** + BPF 소켓 배분 — accept storm 제거
- [ ] `[Linux]`  **`SO_BUSY_POLL`** / **`SO_PREFER_BUSY_POLL`** — 인터럽트 없이 busy-poll로 수신 지연 제거
- [ ] `[Linux]`  **AF_XDP** (eXpress Data Path) — 커널 네트워크 스택 bypass 옵션 (극단적 성능 필요 시)

### macOS 전용

- [ ] `[macOS]`  **`kqueue` Timer 정밀도 개선**: `EVFILT_TIMER` ms→µs 해상도
- [ ] `[macOS]`  **`sendfile(2)` macOS variant**: `off_t *len` 시그니처 처리
- [ ] `[macOS]`  **`EVFILT_USER`** 기반 wakeup (cross-thread notification)

### 공통

- [ ] `[Common]` Reactor 추상 인터페이스에 `batch_submit()` / `batch_wait()` 추가
- [ ] `[Common]` **`AsyncAccept` awaiter** — accept loop 완전 비동기화
- [ ] `[Common]` **`writev` / `sendmsg` scatter-gather** — 헤더 + body를 단일 syscall로 전송

---

## 🔴 Phase 6: Zero-Cost C++ Architecture

> 목표: 런타임 오버헤드 **0** — 모든 추상화는 컴파일 타임에 제거

### 컴파일 타임 최적화

- [ ] `[Common]` **CRTP 기반 정적 다형성** — 핫 패스에서 `virtual` 디스패치 제거
  - [ ] `Reactor<Derived>` CRTP 기반 리팩토링 — vtable 비용 제거
  - [ ] `Handler<Derived>` 정적 핸들러 체인
- [ ] `[Common]` **컴파일 타임 라우팅** — `consteval` / `constexpr` 라우팅 테이블 구축
  - [ ] 경로 패턴을 컴파일 시 파싱 → 런타임 비교 비용 최소화
  - [ ] 고정 경로(`/health`, `/metrics`)는 완전 컴파일 타임 resolve
- [ ] `[Common]` **Policy-based Design** — 로깅/직렬화/할당자 전략을 템플릿 파라미터로 교체
- [ ] `[Common]` **`[[always_inline]]` 어노테이션** — 핫 패스 함수 강제 인라인
- [ ] `[Common]` **`[[likely]]` / `[[unlikely]]`** — 분기 예측 힌트 전면 적용
- [ ] `[Common]` **`-fno-exceptions`** 빌드 옵션 지원 — 예외 핸들링 overhead 제거
- [ ] `[Common]` **`-fno-rtti`** 빌드 옵션 지원 — RTTI 테이블 메모리 제거

### 링크 타임 / 바이너리 최적화

- [ ] `[Common]` **LTO (Link-Time Optimization)** — `-flto=thin` whole-program 인라이닝
- [ ] `[Common]` **PGO (Profile-Guided Optimization)** 2-pass 빌드 가이드
  - [ ] Instrumented build → wrk 프로파일 수집 → Optimized build
- [ ] `[Linux]`  **BOLT** (Binary Optimization and Layout Tool) — 핫 함수 재배치로 I-cache miss 감소
- [ ] `[macOS]`  **`-fprofile-instr-generate`** Clang PGO 지원

### CPU 레벨 최적화

- [ ] `[Common]` **SIMD 가속 HTTP 파서** — AVX2 (x86) / NEON (ARM) 벡터 연산으로 헤더 스캐닝
  - [ ] 개행(`\r\n`) 탐색: 16바이트 병렬 비교 (picohttpparser 기법 참고)
  - [ ] Method / 상태코드 매칭: SIMD 문자열 비교
- [ ] `[Common]` **Cache Line 정렬** — 핫 데이터 구조에 `alignas(64)` 적용
  - [ ] Reactor, Connection, Arena 헤더를 캐시라인에 맞게 패킹
  - [ ] `std::hardware_destructive_interference_size` 로 false sharing 방지
- [ ] `[Common]` **프리패치 힌트** — `__builtin_prefetch` 로 다음 Connection 구조체 미리 로드
- [ ] `[Common]` **소규모 버퍼 스택 할당** — 2KB 이하 요청 헤더는 힙 대신 스택 사용
- [ ] `[Common]` **SSO (Small String Optimization)** — 짧은 헤더값 힙 할당 없이 인라인 저장
- [ ] `[Common]` **vDSO 타이밍** — `clock_gettime(CLOCK_MONOTONIC_COARSE)` 로 syscall 없는 타임스탬프

### 메모리 서브시스템

- [ ] `[Common]` **Slab 할당자** — Connection / Request / Response 오브젝트 풀 (free-list 재사용)
- [ ] `[Common]` **tcmalloc / jemalloc 통합 옵션** — 전역 malloc 교체 빌드 플래그
- [ ] `[Common]` **헤더 문자열 인터닝** — `Content-Type`, `Content-Length` 등 상용 헤더 포인터 재사용
- [ ] `[Linux]`  **`madvise(MADV_HUGEPAGE)`** — arena 메모리 Transparent Huge Page 활성화
- [ ] `[Linux]`  **`mmap(MAP_HUGETLB)`** — 명시적 2MB 페이지로 TLB 미스 감소
- [ ] `[macOS]`  **`VM_FLAGS_SUPERPAGE_SIZE_2MB`** — macOS Huge Page 매핑

---

## 🔴 Phase 7: HTTP/1.1 완전 준수

> 목표: RFC 7230/7231/7234 완전 지원

- [ ] `[Common]` **Keep-Alive / Connection Reuse**
  - [ ] `Connection: keep-alive` 헤더 파싱 및 응답
  - [ ] keep-alive timeout & max-requests per connection 설정
  - [ ] per-connection state machine (Idle → Reading → Processing → Writing → Idle)
- [ ] `[Common]` **Chunked Transfer Encoding** (송수신 모두)
  - [ ] 청크 파싱 (요청 body)
  - [ ] 청크 직렬화 (응답 body streaming)
- [ ] `[Common]` **Pipelining** — 순서 보장 응답 큐 (head-of-line 유지)
- [ ] `[Common]` **Content-Length 자동 계산** — 응답 직렬화 시 자동 삽입
- [ ] `[Common]` **100 Continue** 처리 (`Expect: 100-continue`)
- [ ] `[Common]` **Request body 크기 제한** — configurable `max_body_size`
- [ ] `[Common]` **Connection Timeout**
  - [ ] Idle timeout (keep-alive 유휴 시간)
  - [ ] Read timeout (헤더 수신 최대 시간)
  - [ ] Write timeout (응답 전송 최대 시간)
- [ ] `[Common]` **Graceful Shutdown** — 진행 중 요청 완료 후 종료
  - [ ] `SIGTERM` / `SIGINT` 핸들러 등록
  - [ ] drain 모드: 신규 accept 중단, 기존 연결 완료 대기
- [ ] `[Common]` **`Date` 헤더** — atomic cached 포맷 (1초 단위 갱신, lock-free)
- [ ] `[Common]` **Conditional Requests** — `ETag`, `Last-Modified`, `If-None-Match`, `If-Modified-Since`
- [ ] `[Common]` **Range Requests** — `Range` 헤더, 206 Partial Content
- [ ] `[Common]` **`TCP_QUICKACK`** — ACK 즉시 전송으로 RTT 단축
- [ ] `[Common]` **`TCP_DEFER_ACCEPT`** — 데이터 도착 후 accept() 실행 (SYN flood 방어 겸용)
- [ ] `[Common]` **`TCP_CORK` / `MSG_MORE`** — 헤더+body 단일 전송 배칭

---

## 🔴 Phase 8: 핵심 미들웨어 파이프라인

> 목표: 프로덕션 필수 미들웨어 — 모두 zero-allocation으로 구현

### 미들웨어 엔진

- [ ] `[Common]` **미들웨어 체인 개선**
  - [ ] `next()` 기반 비동기 미들웨어 (`AsyncMiddleware`)
  - [ ] 에러 미들웨어 (`ErrorHandler` 4-arg variant)
  - [ ] 미들웨어 실행 순서 보장 (등록 순서)
  - [ ] 미들웨어 체인을 컴파일 타임에 flatten (zero-overhead pipeline)

### 필수 미들웨어

- [ ] `[Common]` **Request ID 미들웨어** — UUID v4 자동 생성 (lock-free), `X-Request-ID` 헤더
- [ ] `[Common]` **Logging 미들웨어**
  - [ ] Combined Log Format (Apache/Nginx 호환)
  - [ ] JSON structured log 옵션
  - [ ] 응답 시간 측정 (ns 단위, `CLOCK_MONOTONIC`)
  - [ ] **비동기 로그 링 버퍼** — 로그를 ring buffer에 enqueue, 별도 thread가 flush (hot path 블로킹 없음)
- [ ] `[Common]` **CORS 미들웨어**
  - [ ] `Access-Control-Allow-Origin` / `Methods` / `Headers` 설정
  - [ ] Preflight (`OPTIONS`) 자동 처리
  - [ ] 동적 Origin 화이트리스트 검사
- [ ] `[Common]` **Body Parser 미들웨어**
  - [ ] `application/json` → beast-json 파싱
  - [ ] `application/x-www-form-urlencoded` 파싱
  - [ ] `multipart/form-data` 파싱 (파일 업로드)
- [ ] `[Common]` **Rate Limiting 미들웨어**
  - [ ] 토큰 버킷 알고리즘 (thread-local per reactor, lock-free)
  - [ ] IP 기반 / API key 기반 제한
  - [ ] `X-RateLimit-*` 응답 헤더
- [ ] `[Common]` **Compression 미들웨어**
  - [ ] `Content-Encoding: gzip` (zlib, streaming 압축)
  - [ ] `Content-Encoding: br` (brotli, optional)
  - [ ] `Accept-Encoding` 협상 (q-factor 파싱)
- [ ] `[Common]` **Cookie 파싱** — `Cookie:` 헤더 파싱, `Set-Cookie:` 응답 빌더
- [ ] `[Common]` **Session 미들웨어** — lock-free 세션 맵 (per-reactor 분산)
- [ ] `[Common]` **JWT 미들웨어**
  - [ ] HS256 / RS256 검증
  - [ ] `Authorization: Bearer <token>` 파싱
  - [ ] **상수 시간 서명 비교** (`CRYPTO_memcmp`) — timing attack 방지
- [ ] `[Common]` **Static File Serving 미들웨어**
  - [ ] MIME type 자동 감지
  - [ ] `ETag` / `Last-Modified` 자동 생성
  - [ ] Directory listing (optional)
- [ ] `[Common]` **Health Check** — `GET /health` → `{"status":"ok"}` 내장 핸들러 (컴파일 타임 상수 응답)
- [ ] `[Common]` **Panic Recovery 미들웨어** — 핸들러 예외 catch → 500 응답 변환

---

## 🔴 Phase 9: 보안 하드닝

> 목표: 프로덕션 보안 필수 항목 — 성능 페널티 없이 방어

### 프로토콜 레벨 방어

- [ ] `[Common]` **HTTP Request Smuggling 방지**
  - [ ] `Transfer-Encoding` + `Content-Length` 동시 존재 시 400 거부
  - [ ] 모호한 청크 헤더 파싱 엄격 모드 (`chunked` 외 무시)
- [ ] `[Common]` **HTTP Header Injection 방지** — 헤더값 `\r\n` 포함 시 reject
- [ ] `[Common]` **Slowloris 공격 완화** — Read timeout (Phase 7) + 헤더 최대 크기 제한 조합
- [ ] `[Common]` **Request Flood 방지** — Rate Limiting (Phase 8) + per-IP connection 수 제한
- [ ] `[Common]` **Path Traversal 방지** — `../`, `%2e%2e` URL 정규화 후 거부
- [ ] `[Common]` **Large Header Bomb 방지** — 단일 헤더 최대 크기 제한 (configurable, 기본 8KB)

### 보안 헤더 헬퍼

- [ ] `[Common]` **HSTS** — `Strict-Transport-Security` 헬퍼
- [ ] `[Common]` **CSP** — `Content-Security-Policy` 빌더 API
- [ ] `[Common]` **X-Frame-Options** / **X-Content-Type-Options** / **Referrer-Policy** 헬퍼
- [ ] `[Common]` **Permissions-Policy** 헤더 빌더

### 암호화

- [ ] `[Common]` **상수 시간 문자열 비교** — timing attack 방지 (`crypto_memcmp` 유틸)
- [ ] `[Common]` **CSRF 토큰 생성** — `CSPRNG`(`getrandom`/`arc4random`) 기반 안전 난수
- [ ] `[Linux]`  **`getrandom(2)` syscall** — `/dev/urandom` 파일 open 없이 난수 획득
- [ ] `[macOS]`  **`arc4random_buf()`** — macOS 커널 제공 CSPRNG

---

## 🔴 Phase 10: TLS/SSL 지원

> 목표: HTTPS 완전 지원 (OpenSSL / BoringSSL) — non-blocking, zero-copy

### 공통

- [ ] `[Common]` **TLS 추상 레이어** — `TlsStream` wrapper (openssl/boringssl 양쪽 지원)
- [ ] `[Common]` **TLS 핸드셰이크 비동기화** — reactor 이벤트 기반 non-blocking handshake
- [ ] `[Common]` **인증서 설정 API**
  - [ ] `app.tls({ cert, key, ca })` 설정
  - [ ] PEM / DER 포맷 지원
  - [ ] Hot reload (SIGUSR1 트리거) — 다운타임 없는 인증서 교체
- [ ] `[Common]` **SNI 지원** — 다중 도메인 인증서 (Server Name Indication)
- [ ] `[Common]` **mTLS** — 클라이언트 인증서 검증 (optional)
- [ ] `[Common]` **ALPN 협상** — `http/1.1` / `h2` / `h3` 프로토콜 선택
- [ ] `[Common]` **TLS 1.3 전용 모드** — TLS 1.2 이하 비활성화 옵션
- [ ] `[Common]` **Session Resumption** — TLS 1.3 0-RTT / Session Tickets
- [ ] `[Common]` **OCSP Stapling** — 인증서 유효성 캐시 (클라이언트 RTT 제거)

### Linux 전용

- [ ] `[Linux]`  **`KTLS` (Kernel TLS)** — `setsockopt(TCP_ULP, "tls")` zero-copy 암호화
  - [ ] Kernel 5.2+ `TLS_TX` / `TLS_RX` 모드
  - [ ] io_uring + kTLS 연동 (암호화된 sendfile)

---

## 🟠 Phase 11: HTTP/2 지원

> 목표: nghttp2 기반 HTTP/2 완전 구현

- [ ] `[Common]` **nghttp2 통합** — FetchContent 또는 vcpkg
- [ ] `[Common]` **Connection Preface** — 클라이언트 Magic + SETTINGS 프레임 처리
- [ ] `[Common]` **HPACK 헤더 압축** — nghttp2 내장 HPACK 활용
- [ ] `[Common]` **스트림 다중화** — 단일 TCP 연결 위 다중 요청/응답 스트림
- [ ] `[Common]` **Flow Control** — 스트림 / 연결 레벨 윈도우 관리
- [ ] `[Common]` **Server Push** — `app.push(path, headers)` API
- [ ] `[Common]` **스트림 우선순위** — `PRIORITY` 프레임 처리
- [ ] `[Common]` **`GOAWAY` / `RST_STREAM`** 정상 처리
- [ ] `[Common]` **HTTP/2 → HTTP/1.1 Upgrade** — `h2c` cleartext 업그레이드
- [ ] `[Common]` **ALPN 통합** — Phase 10 TLS ALPN과 연동하여 h2 자동 선택

---

## 🟠 Phase 12: HTTP/3 / QUIC 지원

> 목표: 차세대 프로토콜 — 0-RTT, 연결 마이그레이션, 패킷 손실 복원

- [ ] `[Common]` **QUIC 라이브러리 통합** — quiche (Cloudflare) 또는 msquic (Microsoft) 선택
- [ ] `[Common]` **UDP 소켓 비동기 처리** — io_uring `IORING_OP_RECVMSG` / kqueue `EVFILT_READ`
- [ ] `[Common]` **QUIC 핸드셰이크** — TLS 1.3 over QUIC, 0-RTT 재연결
- [ ] `[Common]` **스트림 다중화** — QUIC stream ID 기반 병렬 요청
- [ ] `[Common]` **연결 마이그레이션** — CID 기반 IP 변경 추적 (모바일 네트워크 전환)
- [ ] `[Common]` **패킷 손실 복원** — QUIC 자체 재전송 (TCP retransmit 대비 빠름)
- [ ] `[Common]` **`Alt-Svc` 헤더** — HTTP/1.1·2 응답에 `h3=":443"` 광고
- [ ] `[Common]` **HTTP/3 시맨틱** — QPACK 헤더 압축, pseudo-headers

---

## 🟠 Phase 13: WebSocket 지원

> 목표: RFC 6455 완전 준수 WebSocket

- [ ] `[Common]` **Upgrade 핸드셰이크** — `101 Switching Protocols` 처리
- [ ] `[Common]` **프레임 파서** — FIN, opcode, mask, payload 처리
- [ ] `[Common]` **프레임 직렬화** — 서버→클라이언트 마스킹 없이 전송
- [ ] `[Common]` **opcode 처리**
  - [ ] `0x1` Text frame
  - [ ] `0x2` Binary frame
  - [ ] `0x8` Close frame (정상 종료)
  - [ ] `0x9` / `0xA` Ping / Pong (자동 응답)
- [ ] `[Common]` **단편화 (Fragmentation)** — 멀티 프레임 메시지 재조립
- [ ] `[Common]` **메시지 크기 제한** — `max_message_size` 설정
- [ ] `[Common]` **`WebSocket` API**
  - [ ] `ws.send(text)` / `ws.send(binary)`
  - [ ] `ws.close(code, reason)`
  - [ ] `ws.on_message(handler)` / `ws.on_close(handler)`
- [ ] `[Common]` **Broadcast 지원** — lock-free 연결 레지스트리 + 멀티캐스트 전송
- [ ] `[Common]` **WS over TLS** (wss://) — Phase 10 TLS 통합
- [ ] `[Common]` **Subprotocol 협상** — `Sec-WebSocket-Protocol` 헤더
- [ ] `[Common]` **per-message deflate** — WS 압축 확장 (RFC 7692)

---

## 🟠 Phase 14: Streaming & SSE (Server-Sent Events)

- [ ] `[Common]` **Response Streaming API** — `res.write(chunk)` + `res.end()`
- [ ] `[Common]` **Server-Sent Events (SSE)**
  - [ ] `text/event-stream` 콘텐츠 타입 응답
  - [ ] `SseStream` 헬퍼: `send_event(name, data, id)` API
  - [ ] heartbeat (`comment:` ping) 자동 전송
- [ ] `[Common]` **파일 스트리밍** — 대용량 파일 청크 전송 (메모리에 올리지 않음)
- [ ] `[Common]` **백프레셔 (Backpressure)** — 소켓 쓰기 버퍼 포화 시 일시정지/재개 (`co_await` 기반)
- [ ] `[Common]` **Multipart 응답** — `multipart/x-mixed-replace` (MJPEG 스트림 등)

---

## 🟠 Phase 15: gRPC 지원

> 목표: HTTP/2 기반 gRPC — Protocol Buffers + 스트리밍

- [ ] `[Common]` **Protobuf 통합** — `protobuf` 라이브러리 FetchContent / vcpkg
- [ ] `[Common]` **gRPC 프레임 파싱** — 5바이트 length-prefix 프레임
- [ ] `[Common]` **gRPC 서비스 등록 API**
  - [ ] `app.grpc<Service>(handler)` 라우팅
  - [ ] Unary RPC
  - [ ] Server Streaming RPC
  - [ ] Client Streaming RPC
  - [ ] Bidirectional Streaming RPC
- [ ] `[Common]` **`grpc-status` / `grpc-message` Trailer** — HTTP/2 Trailer 처리
- [ ] `[Common]` **gRPC-Web** — 브라우저 호환 (HTTP/1.1 + base64 인코딩)
- [ ] `[Common]` **Deadline / Timeout** — `grpc-timeout` 헤더 파싱 및 `AsyncSleep` 연동
- [ ] `[Common]` **인터셉터 체인** — 미들웨어 패턴의 gRPC 인터셉터

---

## 🟡 Phase 16: 네트워크 심화 기능

### 공통

- [ ] `[Common]` **IPv6 완전 지원** — `::1` 바인딩, dual-stack `IPV6_V6ONLY` 설정
- [ ] **Unix Domain Socket** — `app.listen("/tmp/draco.sock")` 지원
  - [ ] `[Linux]`  `AF_UNIX` SOCK_STREAM
  - [ ] `[macOS]`  `AF_UNIX` SOCK_STREAM
- [ ] `[Common]` **리버스 프록시 지원**
  - [ ] `X-Forwarded-For` / `X-Real-IP` 헤더 파싱
  - [ ] Trusted proxy 화이트리스트 설정
- [ ] `[Common]` **Virtual Hosting** — Host 헤더 기반 라우터 분기
- [ ] `[Common]` **비동기 HTTP 클라이언트** — 백엔드 API 호출용 내장 클라이언트
  - [ ] `co_await client.get(url, headers)` / `co_await client.post(...)`
  - [ ] Keep-alive pool (per-domain connection reuse)
  - [ ] Connection timeout / response timeout
- [ ] `[Common]` **DNS 조회 (비동기)** — getaddrinfo 블로킹 해결 (thread pool offload)

### Linux 전용

- [ ] `[Linux]`  **`TCP_FASTOPEN`** — 3-way handshake 없이 첫 패킷에 데이터 전송
- [ ] `[Linux]`  **`SO_INCOMING_CPU`** — RSS 기반 CPU 친화성 소켓 배분
- [ ] `[Linux]`  **NUMA 인식 스레드 배치** — `numa_alloc_onnode` / `set_mempolicy`
- [ ] `[Linux]`  **NIC IRQ Affinity 가이드** — `/proc/irq/*/smp_affinity` 설정 스크립트

### macOS 전용

- [ ] `[macOS]`  **`TCP_FASTOPEN`** — macOS 12+ 지원
- [ ] `[macOS]`  **`SO_NOSIGPIPE`** — SIGPIPE 억제

---

## 🟡 Phase 17: Observability & 메트릭

> 목표: 성능 페널티 없는 프로덕션 가시성 — lock-free atomic 카운터 기반

- [ ] `[Common]` **Lock-free 내장 메트릭**
  - [ ] `std::atomic<uint64_t>` per-reactor 카운터 (요청 수, 에러 수, 활성 연결 수)
  - [ ] 히스토그램 버킷 (레이턴시 분포 P50/P95/P99)
  - [ ] 메트릭 집계 — reactor별 카운터 → 원자적 합산
- [ ] `[Common]` **Prometheus 메트릭 엔드포인트** — `GET /metrics` text exposition 포맷
  - [ ] `draco_requests_total{method, status}` counter
  - [ ] `draco_request_duration_seconds` histogram
  - [ ] `draco_connections_active` gauge
  - [ ] `draco_memory_arena_bytes` gauge
- [ ] `[Common]` **OpenTelemetry 트레이싱**
  - [ ] `traceparent` / `tracestate` W3C 헤더 파싱
  - [ ] span 컨텍스트 미들웨어로 전파
  - [ ] OTLP HTTP/gRPC exporter (Phase 15 gRPC 활용)
- [ ] `[Common]` **구조화 로깅 (Structured Logging)**
  - [ ] JSON 라인 포맷 (`{"ts":..,"level":..,"msg":..,"req_id":..}`)
  - [ ] **비동기 ring-buffer 로거** — hot path에서 메모리 복사만, I/O는 background thread
  - [ ] log level runtime 변경 (`SIGUSR2` 트리거)
- [ ] `[Common]` **요청 추적 (Distributed Tracing)** — `X-Request-ID` + `traceparent` 연동

---

## 🟡 Phase 18: URL / Request 유틸리티

- [ ] `[Common]` **URL 파싱** — path, query string, fragment 완전 분리
- [ ] `[Common]` **Query String 파싱** — `?foo=bar&baz=1` → `req.query("foo")` API
- [ ] `[Common]` **URL 인코딩/디코딩** — percent-encoding 유틸리티
- [ ] `[Common]` **Path Normalization** — `..` / `.` 제거, 이중 슬래시 처리
- [ ] `[Common]` **MIME Type 레지스트리** — 확장자 → Content-Type 자동 매핑 (컴파일 타임 해시맵)
- [ ] `[Common]` **Content Negotiation** — `Accept:` 헤더 q-factor 파싱 및 협상
- [ ] `[Common]` **`req.ip()`** — 실제 클라이언트 IP 추출 (프록시 헤더 포함)
- [ ] `[Common]` **Multipart Form Data 파서** — 바이너리 파일 업로드 포함

---

## 🟡 Phase 19: 성능 & 벤치마킹

> 목표: TechEmpower 프레임워크 벤치마크 Top 10 진입

### 공통

- [ ] `[Common]` **내장 벤치마크 스위트**
  - [ ] `wrk` / `wrk2` / `k6` 스크립트 제공 (`benchmarks/` 디렉토리)
  - [ ] Latency P50 / P95 / P99 / P99.9 측정
  - [ ] Throughput (RPS) 측정 자동화
  - [ ] **TechEmpower Fortune / JSON / Plaintext** 호환 엔드포인트
- [ ] `[Common]` **C10M 스트레스 테스트**
  - [ ] 10M 동시 연결 목표 시나리오
  - [ ] 연결당 메모리 사용량 측정 (목표: < 1KB/conn)
- [ ] `[Common]` **Flame Graph 통합** — 프로파일링 가이드 (`perf` / Instruments)
- [ ] `[Common]` **Zero-copy Serialization 최적화**
  - [ ] 응답 헤더 iovec scatter-gather 직렬화 (writev)
  - [ ] Beast JSON write buffer chain 최적화
- [ ] `[Common]` **Lock-free 라우팅 테이블** — 원자적 업데이트, RCU 패턴
- [ ] `[Common]` **pre-serialized 정적 응답** — `200 OK` / `404 Not Found` 바이트 배열 컴파일 타임 생성

### Linux 전용

- [ ] `[Linux]`  **`perf stat`** 통합 테스트 — cache miss / branch mispred 자동 리포트
- [ ] `[Linux]`  **io_uring SQ Polling** (`IORING_SETUP_SQPOLL`) — steady-state syscall 0
- [ ] `[Linux]`  **`taskset` + `chrt -f`** 실시간 우선순위 가이드

### macOS 전용

- [ ] `[macOS]`  **Instruments / DTrace 프로파일링 가이드**
- [ ] `[macOS]`  **`kqueue` kevent 배치 처리** — `nevents` 최대화 튜닝

---

## 🟡 Phase 20: 테스트 & CI/CD 강화

- [ ] `[Common]` **통합 테스트 확장**
  - [ ] Keep-alive 커넥션 재사용 테스트
  - [ ] Chunked 인코딩 수신/송신 테스트
  - [ ] 미들웨어 체인 순서 테스트
  - [ ] 에러 핸들러 테스트
  - [ ] Concurrent 요청 경쟁 조건 테스트
  - [ ] Request smuggling 방어 테스트
  - [ ] 대용량 파일 스트리밍 테스트
- [ ] `[Common]` **Fuzzing** — libFuzzer 기반 HTTP 파서 퍼징 (`corpus/` 제공)
- [ ] `[Common]` **Coverage Report** — lcov / gcovr + HTML 리포트 (목표: 90%+)
- [ ] `[Common]` **Sanitizer CI 통합**
  - [ ] AddressSanitizer (ASan)
  - [ ] UndefinedBehaviorSanitizer (UBSan)
  - [ ] ThreadSanitizer (TSan) — race condition 검출
  - [ ] MemorySanitizer (MSan) — 미초기화 읽기 검출
- [ ] `[Linux]`  **Valgrind / Helgrind** race condition 검사
- [ ] `[macOS]`  **Leaks / MallocStackLogging** 메모리 누수 검사
- [ ] `[Common]` **GitHub Actions 매트릭스 확장**
  - [ ] `ubuntu-22.04` + `ubuntu-24.04` (GCC 13, Clang 17, Clang 18)
  - [ ] `macos-13` (x86_64) + `macos-14` (Apple Silicon arm64)
  - [ ] Nightly 벤치마크 자동화 (PR 대비 성능 회귀 감지, 5% 이상 저하 시 fail)
  - [ ] Sanitizer 빌드 별도 매트릭스

---

## 🟢 Phase 21: 문서화 & DX (Developer Experience)

- [ ] `[Common]` **API 레퍼런스 문서** — Doxygen + GitHub Pages 자동 배포
- [ ] `[Common]` **Getting Started 가이드** — 5분 안에 Hello World 실행
- [ ] `[Common]` **예제 확장**
  - [ ] REST API 서버 (CRUD + JWT 인증)
  - [ ] WebSocket 채팅 서버
  - [ ] SSE 실시간 피드
  - [ ] 정적 파일 서버 (SPA 호스팅)
  - [ ] TLS HTTPS 서버
  - [ ] HTTP/2 서버
  - [ ] HTTP/3 / QUIC 서버
  - [ ] gRPC 서버
  - [ ] Prometheus + Grafana 연동 예제
- [ ] `[Common]` **Migration Guide** — v0.x → v1.0 Breaking Change 문서
- [ ] `[Common]` **성능 튜닝 가이드**
  - [ ] `[Linux]`  io_uring 커널 파라미터 (`ulimit -n`, `nr_open`, `memlock`, `SQPOLL` 권한)
  - [ ] `[Linux]`  OS 튜닝 스크립트 (`scripts/tune_linux.sh`) — sysctl, IRQ affinity, CPU governor
  - [ ] `[macOS]`  kqueue / `kern.maxfilesperproc` 설정 가이드
- [ ] `[Common]` **아키텍처 다이어그램** — Reactor / Dispatcher / Handler 흐름도 (Mermaid)
- [ ] `[Common]` **보안 가이드** — TLS 설정, 보안 헤더, Rate Limiting 베스트 프랙티스

---

## 🟠 Phase 22: SOLID 설계 원칙 & 확장 인터페이스

> 목표: Draco 전체 확장 포인트를 SOLID 원칙으로 설계 — 코어 수정 없이 드라이버/플러그인 추가 가능

### 인터페이스 분리 원칙 (ISP) — 작고 집중된 인터페이스

- [ ] `[Common]` **`IReadable`** / **`IWritable`** — I/O 역할 분리 (읽기·쓰기 독립 인터페이스)
- [ ] `[Common]` **`IDbReader`** — `co_await query(sql, params)` 만 노출
- [ ] `[Common]` **`IDbWriter`** — `co_await execute(sql, params)` 만 노출
- [ ] `[Common]` **`IDbPool`** — `co_await acquire()` / `release()` 만 노출
- [ ] `[Common]` **`ILogSink`** — `write(LogRecord)` / `flush()` 만 노출
- [ ] `[Common]` **`ILogFormatter`** — `format(LogRecord) → string_view` 만 노출
- [ ] `[Common]` **`ICacheReader`** — `co_await get(key)` 만 노출
- [ ] `[Common]` **`ICacheWriter`** — `co_await set(key, value, ttl)` 만 노출
- [ ] `[Common]` **`IPublisher`** — `co_await publish(topic, message)` 만 노출
- [ ] `[Common]` **`ISubscriber`** — `co_await subscribe(topic)` 만 노출
- [ ] `[Common]` **`ISerializer`** — `serialize(T) → bytes` / `deserialize<T>(bytes)` 만 노출
- [ ] `[Common]` **`IHealthCheck`** — `co_await ping() → bool` 만 노출

### 의존성 역전 원칙 (DIP) — 추상에 의존, 구현체에 미의존

- [ ] `[Common]` **의존성 주입 컨테이너 (`draco::Container`)** — 런타임 DI 지원
  - [ ] `container.bind<IDbPool>(PostgresPool::create(config))`
  - [ ] `container.bind<ICache>(RedisCache::create(config))`
  - [ ] `container.resolve<IDbPool>()` → 등록된 구현체 반환
  - [ ] 스코프 지원: Singleton / PerRequest / Transient
- [ ] `[Common]` **핸들러 DI 자동 주입** — 라우트 핸들러 파라미터로 추상 인터페이스 주입
  ```cpp
  app.get("/users", [](Request& req, Response& res,
                       IDbPool& db, ICache& cache) -> Task<void> { ... });
  ```

### 개방-폐쇄 원칙 (OCP) — 확장에 열리고 수정에 닫혀 있음

- [ ] `[Common]` **드라이버 레지스트리** — `DriverRegistry::register<IDbPool>("postgres", factory)` 패턴
- [ ] `[Common]` **플러그인 로더** — 동적 `.so` / `.dylib` 드라이버 로드 (선택적)
- [ ] `[Common]` **미들웨어 플러그인** — 써드파티 미들웨어 CMake 타겟으로 독립 배포 가능

### 단일 책임 원칙 (SRP) — 모듈 경계 정의

- [ ] `[Common]` **CMake 모듈 분리**
  ```
  draco::core        — Reactor, Dispatcher, Coroutine
  draco::http        — Parser, Router, Request/Response
  draco::tls         — TLS 추상 레이어
  draco::db          — DB 추상화 인터페이스만 (드라이버 미포함)
  draco::log         — 로깅 인터페이스 + 내장 구현체
  draco::cache       — 캐시 인터페이스만
  draco::queue       — 메시지 큐 인터페이스만
  draco::util        — 공용 유틸리티 (uuid, hash, base64 등)
  draco::concurrency — Channel, WaitGroup, Select 등
  draco::metrics     — Prometheus, OTel (Phase 17)
  draco::di          — 의존성 주입 컨테이너
  ```

---

## 🟠 Phase 23: DB 추상화 레이어 & 드라이버

> 목표: WAS 차원에서 공용 DB 인터페이스 제공 — 실제 드라이버는 별도 CMake 타겟, 프로젝트에서 선택 탑재

### 핵심 추상화 (`draco/db/`)

- [ ] `[Common]` **`DbParams`** — 타입 안전 바인드 파라미터 (`std::variant<int64_t, double, std::string, std::vector<uint8_t>, std::nullptr_t>`)
- [ ] `[Common]` **`IDbResult`** 인터페이스
  - [ ] `bool next()` — 다음 행으로 이동
  - [ ] `get<T>(int col)` / `get<T>(string_view name)` — 타입 안전 컬럼 조회
  - [ ] `column_count()` / `column_name(int)` — 메타데이터
  - [ ] `rows_affected()` — DML 결과 행 수
- [ ] `[Common]` **`IDbConnection`** 인터페이스
  - [ ] `co_await query(sql, params) → IDbResult::Ptr`
  - [ ] `co_await execute(sql, params) → uint64_t` (affected rows)
  - [ ] `co_await prepare(sql) → IDbStatement::Ptr` — 재사용 가능 prepared statement
  - [ ] `co_await begin() → IDbTransaction::Ptr`
- [ ] `[Common]` **`IDbTransaction`** 인터페이스
  - [ ] `co_await commit()` / `co_await rollback()`
  - [ ] RAII: 소멸자에서 자동 rollback
- [ ] `[Common]` **`IDbPool`** 인터페이스
  - [ ] `co_await acquire() → PooledConnection` (RAII wrapper, 소멸 시 자동 반환)
  - [ ] `size()` / `idle_count()` / `waiting_count()` — 풀 상태 조회
- [ ] `[Common]` **제네릭 `AsyncPool<T>`** — DB 외 Redis 등에도 재사용 가능한 커넥션 풀 템플릿
  - [ ] per-reactor 파티션 풀 (lock-free per shard)
  - [ ] Idle timeout / Max connection lifetime
  - [ ] Health check (ping before use)
  - [ ] Circuit breaker 연동 (Phase 26)
  - [ ] Backpressure: 풀 소진 시 `co_await` 대기 or timeout

### 드라이버 구현체 (별도 CMake 타겟 `draco::db_<name>`)

- [ ] `[Common]` **`draco::db_postgres`** — PostgreSQL 비동기 드라이버
  - [ ] `libpq` non-blocking API (`PQsendQuery` + `PQconsumeInput`) 기반
  - [ ] io_uring / kqueue 이벤트와 연동 (socket FD 직접 등록)
  - [ ] COPY 프로토콜 지원 (bulk insert)
  - [ ] `NOTIFY` / `LISTEN` 비동기 알림 수신
  - [ ] SSL/TLS 연결 지원
- [ ] `[Common]` **`draco::db_mysql`** — MySQL / MariaDB 드라이버
  - [ ] MariaDB C Connector 비동기 모드
  - [ ] Prepared statement 서버사이드 캐싱
- [ ] `[Common]` **`draco::db_sqlite`** — SQLite 드라이버
  - [ ] 블로킹 SQLite API → Draco thread pool offload (`AsyncTask`)
  - [ ] WAL 모드 자동 활성화 (동시 읽기 성능)
  - [ ] In-memory DB 지원 (테스트용)
- [ ] `[Common]` **`draco::db_redis`** — Redis RESP3 프로토콜 드라이버
  - [ ] 완전 비동기 RESP3 파서 (co_await)
  - [ ] Pipeline 모드 (배치 커맨드)
  - [ ] Pub/Sub 채널 (ISubscriber 구현)
  - [ ] Cluster 모드 (슬롯 기반 라우팅)
  - [ ] Sentinel 모드 (failover 자동 추적)

### 편의 API

- [ ] `[Common]` **`draco::db::scope`** — RAII 연결 스코프 헬퍼
  ```cpp
  auto conn = co_await db.acquire();
  auto result = co_await conn->query("SELECT ...", {user_id});
  while (result->next()) { ... }
  // 스코프 종료 시 자동 release
  ```
- [ ] `[Common]` **`draco::db::transaction`** — 람다 기반 트랜잭션 헬퍼
  ```cpp
  co_await draco::db::transaction(pool, [](auto& tx) -> Task<void> {
      co_await tx.execute("INSERT ...", {...});
      co_await tx.execute("UPDATE ...", {...});
  }); // 예외 시 자동 rollback
  ```
- [ ] `[Common]` **마이그레이션 러너** — `migrations/001_init.sql` 순차 실행 + 버전 추적

---

## 🟠 Phase 24: 로깅 시스템 심화

> 목표: 핫 패스에서 I/O 없는 비동기 로거 — `ILogSink` 교체만으로 출력 대상 변경

### 로거 코어 (`draco/log/`)

- [ ] `[Common]` **로그 레벨** — `TRACE < DEBUG < INFO < WARN < ERROR < FATAL`
- [ ] `[Common]` **`LogRecord`** — 구조체 (timestamp, level, logger_name, message, key-value pairs, source_location)
- [ ] `[Common]` **`draco::Logger`** — 비동기 front-end (hot path에서 ring-buffer enqueue만)
  - [ ] `log.info("user logged in", {{"user_id", id}, {"ip", ip}})` API
  - [ ] `log.error("db failed", {{"sql", sql}, {"error", e.what()}})` API
  - [ ] 로그 레벨 런타임 변경 (atomic)
  - [ ] 네임드 로거 계층 (`log.child("db")`, `log.child("http")`)
- [ ] `[Common]` **비동기 링 버퍼** — SPSC 링 버퍼로 핫 패스와 I/O 스레드 분리
  - [ ] 링 버퍼 포화 시 드롭 or 블로킹 선택 (configurable)
  - [ ] 백그라운드 flush 스레드 (전용 코어 핀 가능)

### 포매터 (`ILogFormatter`)

- [ ] `[Common]` **`TextFormatter`** — `[2025-03-10 12:00:00.123] [INFO] message key=value`
- [ ] `[Common]` **`JsonFormatter`** — `{"ts":..,"level":"INFO","msg":..,"user_id":..}`
- [ ] `[Common]` **`LogfmtFormatter`** — `ts=... level=INFO msg=... user_id=...`

### 싱크 (`ILogSink`)

- [ ] `[Common]` **`ConsoleSink`** — stdout/stderr, ANSI 컬러 레벨 표시
- [ ] `[Common]` **`FileSink`** — O_APPEND 파일 쓰기
  - [ ] **로그 로테이션** — 크기 기반 (`max_size`) + 시간 기반 (`daily`) 자동 rotate
  - [ ] 압축 아카이브 (gzip)
  - [ ] 최대 보관 파일 수 설정
- [ ] `[Linux]`  **`SyslogSink`** — `openlog` / `syslog(3)` 연동
- [ ] `[Common]` **`NetworkSink`** — UDP/TCP로 로그 집계 서버 전송
  - [ ] Loki HTTP API 호환 출력 (JSON labels + log stream)
  - [ ] Fluentd Forward 프로토콜 지원
  - [ ] 전송 실패 시 로컬 파일로 fallback
- [ ] `[Common]` **`MultiSink`** — 복수 싱크 동시 출력 (Fan-out)
- [ ] `[Common]` **`SampledSink`** — 고빈도 로그 샘플링 (1/N 비율 기록, 오버헤드 억제)
- [ ] `[Common]` **`NullSink`** — 테스트/벤치마크용 `/dev/null` 등가

---

## 🟠 Phase 25: 캐시 & 메시지 브로커 추상화

> 목표: `ICache` / `IQueue` 인터페이스 기반 — 인메모리 ↔ Redis ↔ Kafka 교체 가능

### 캐시 (`draco/cache/`)

- [ ] `[Common]` **`ICache`** 인터페이스
  - [ ] `co_await get(key) → optional<string>`
  - [ ] `co_await set(key, value, ttl)` / `co_await del(key)`
  - [ ] `co_await exists(key) → bool`
  - [ ] `co_await incr(key) → int64_t` — atomic increment (rate limiting 활용)
  - [ ] `co_await mget(keys) → map<string, optional<string>>` — multi-key 조회
- [ ] `[Common]` **`LruCache`** — 인프로세스 LRU 캐시
  - [ ] 샤딩 (`N` shard, 각 shard 독립 mutex) — lock contention 최소화
  - [ ] 메모리 한도 (`max_bytes`) 설정 — LRU eviction
  - [ ] TTL 지원 (lazy expiry)
  - [ ] `ICache` 인터페이스 구현
- [ ] `[Common]` **`RedisCache`** — Phase 23 `draco::db_redis` 기반 캐시 어댑터
- [ ] `[Common]` **`TwoLevelCache`** — L1(인메모리) + L2(Redis) 계층 캐시 자동 fill

### 메시지 브로커 (`draco/queue/`)

- [ ] `[Common]` **`IPublisher`** / **`ISubscriber`** 인터페이스
- [ ] `[Common]` **`InMemoryBus`** — 인프로세스 Pub/Sub
  - [ ] lock-free MPMC 링 버퍼 기반
  - [ ] 토픽 와일드카드 (`events.*`)
  - [ ] 백프레셔: 구독자 처리 지연 시 publisher 일시 정지
- [ ] `[Common]` **`RedisPubSub`** — Redis Pub/Sub 어댑터 (Phase 23 드라이버 활용)
- [ ] `[Common]` **`KafkaDriver`** (`draco::queue_kafka`) — librdkafka 기반
  - [ ] `IPublisher` 구현 (exactly-once semantics 옵션)
  - [ ] `ISubscriber` 구현 (consumer group, offset commit)
  - [ ] 배치 전송 (linger.ms 설정)
- [ ] `[Common]` **`ITaskQueue`** — 백그라운드 작업 큐 인터페이스
  - [ ] `co_await queue.enqueue(job)` — 작업 예약
  - [ ] 재시도 정책 (최대 횟수, 지수 백오프)
  - [ ] 작업 우선순위 (priority queue)
  - [ ] Dead-letter queue (실패 작업 보관)

---

## 🟠 Phase 26: 제네릭 커넥션 풀 & 서킷 브레이커

> 목표: DB·Redis·외부 API 등 모든 외부 자원에 재사용 가능한 복원력 패턴

### 제네릭 커넥션 풀 (`draco/pool/`)

- [ ] `[Common]` **`AsyncPool<T, Factory>`** 템플릿
  - [ ] `Factory` — `co_await create() → T`, `co_await destroy(T)`, `co_await validate(T) → bool`
  - [ ] Min/Max 크기, Idle timeout, Max lifetime 설정
  - [ ] 풀 소진 시 대기 큐 (`co_await acquire()` — 연결 가용 시 재개)
  - [ ] 대기 timeout (`co_await acquire_for(5s)`)
  - [ ] Per-reactor 분산 풀 (phase 22 Container 통합)
- [ ] `[Common]` **`PooledResource<T>`** — RAII 래퍼 (소멸 시 자동 release 또는 destroy)
- [ ] `[Common]` **풀 메트릭 익스포트** — Prometheus gauge (active, idle, waiting, created, destroyed)

### 서킷 브레이커 (`draco/resilience/`)

- [ ] `[Common]` **`CircuitBreaker`** — 3-state 상태 기계
  - [ ] Closed (정상) → Open (차단) → Half-Open (탐지) 전환
  - [ ] 실패율 임계치 설정 (`failure_rate_threshold`)
  - [ ] Open → Half-Open 대기 시간 (`recovery_timeout`)
  - [ ] Half-Open 성공 횟수 → Closed 복귀 (`success_threshold`)
  - [ ] `co_await breaker.call(fn)` — 차단 시 `CircuitOpenError` 즉시 반환
- [ ] `[Common]` **`RetryPolicy`** — 재시도 정책
  - [ ] 최대 횟수 (`max_attempts`)
  - [ ] 지수 백오프 + Jitter (`base_delay`, `max_delay`)
  - [ ] 재시도 가능 에러 필터 (`retryable_errors`)
  - [ ] `co_await retry(policy, fn)` 헬퍼
- [ ] `[Common]` **`Bulkhead`** — 동시 실행 수 제한 (세마포어 기반)
  - [ ] DB 쿼리 동시 실행 최대 N개 제한
  - [ ] 초과 시 `co_await` 대기 or `BulkheadFullError` 즉시 반환 선택
- [ ] `[Common]` **`Timeout`** 컴비네이터 — `co_await draco::with_timeout(task, 3s)` 글로벌 헬퍼

---

## 🟡 Phase 27: WAS 공용 유틸리티 라이브러리

> 목표: 핫 패스에서 heap 할당 없이 사용 가능한 고성능 유틸리티 (`draco/util/`)

### 고유 식별자

- [ ] `[Common]` **`draco::uuid::v4()`** — CSPRNG 기반 RFC 4122 UUID (no heap)
- [ ] `[Common]` **`draco::uuid::v7()`** — 시간 정렬 UUID v7 (단조 증가, DB 인덱스 친화)
- [ ] `[Common]` **`draco::uuid::to_string(uuid)`** — 하이픈 포맷 변환 (stack buffer)

### 인코딩 / 해시

- [ ] `[Common]` **`draco::base64::encode/decode`** — SIMD(AVX2/NEON) 가속 Base64
- [ ] `[Common]` **`draco::hex::encode/decode`** — 이진 ↔ 16진수 변환 (look-up table)
- [ ] `[Common]` **`draco::hash::xxhash64(data)`** — 비암호 초고속 해시 (라우팅, 샤딩)
- [ ] `[Common]` **`draco::hash::fnv1a(data)`** — 컴파일 타임 상수 해시 (`constexpr`)

### 암호 유틸리티

- [ ] `[Common]` **`draco::crypto::sha256(data)`** — OpenSSL/BoringSSL SHA-256
- [ ] `[Common]` **`draco::crypto::hmac_sha256(key, data)`** — HMAC-SHA256 (JWT, Webhook 서명)
- [ ] `[Common]` **`draco::crypto::bcrypt(password, cost)`** — 패스워드 해싱 (thread pool offload)
- [ ] `[Common]` **`draco::crypto::constant_time_eq(a, b)`** — timing attack 방지 비교
- [ ] `[Common]` **`draco::crypto::random_bytes(n)`** — CSPRNG (`getrandom`/`arc4random`)

### 시간 & 날짜

- [ ] `[Common]` **`draco::time::now_ns()`** — vDSO 기반 나노초 단조 시간 (syscall 없음)
- [ ] `[Common]` **`draco::time::iso8601()`** — ISO 8601 포맷 (stack buffer, `strftime` 미사용)
- [ ] `[Common]` **`draco::time::http_date()`** — HTTP `Date:` 헤더 포맷 (1초 단위 캐시)
- [ ] `[Common]` **`draco::time::parse_http_date(str)`** — HTTP 날짜 문자열 → `time_point`

### 직렬화 / 역직렬화

- [ ] `[Common]` **Beast JSON** — 이미 통합 완료 (Phase 3)
- [ ] `[Common]` **`draco::msgpack`** — MessagePack 직렬화 (`ISerializer` 구현)
  - [ ] `msgpack::serialize(obj) → bytes` / `msgpack::deserialize<T>(bytes)`
- [ ] `[Common]` **`draco::cbor`** — CBOR (RFC 8949) 직렬화 (IoT, CoAP 친화)
- [ ] `[Common]` **`draco::csv`** — CSV 스트리밍 파서 (데이터 임포트용)

### 검증

- [ ] `[Common]` **`draco::validate::email(str)`** — RFC 5321 이메일 형식 검증
- [ ] `[Common]` **`draco::validate::url(str)`** — URL 형식 검증
- [ ] `[Common]` **`draco::validate::json_schema(json, schema)`** — JSON Schema Draft 7 lite
- [ ] `[Common]` **`draco::validate::ip(str)`** — IPv4 / IPv6 형식 검증

### 에러 처리 (예외 없는 패턴)

- [ ] `[Common]` **`draco::Result<T, E>`** — `std::expected<T, E>` (C++23) 기반
  - [ ] `.map(fn)` / `.and_then(fn)` / `.or_else(fn)` 체이닝
  - [ ] `DRACO_TRY(expr)` — `co_await` 없이 Result 전파 매크로
- [ ] `[Common]` **`draco::Error`** — `std::error_code` 기반 도메인 에러 (heap 미사용)
- [ ] `[Common]` **에러 도메인 정의** — `draco::db_error_category`, `draco::http_error_category` 등

### 문자열 유틸리티

- [ ] `[Common]` **`draco::str::split(sv, delim)`** — `string_view` 기반 zero-copy 분할
- [ ] `[Common]` **`draco::str::trim(sv)`** — 앞뒤 공백 제거 (복사 없음)
- [ ] `[Common]` **`draco::str::to_lower/to_upper(sv)`** — ASCII 범위 제자리 변환
- [ ] `[Common]` **`draco::str::starts_with / ends_with / contains`** — C++23 보완
- [ ] `[Common]` **`draco::str::format(fmt, args...)`** — `std::format` (C++23) 래퍼

### 템플릿 엔진 (선택적)

- [ ] `[Common]` **`draco::template::render(tmpl, vars)`** — Mustache `{{key}}` 치환
  - [ ] `{{#section}}` 조건부 블록 / `{{#list}}` 반복 블록
  - [ ] 컴파일 타임 템플릿 파싱 (`consteval`)
  - [ ] HTML escape 자동 처리 (XSS 방지)

---

## 🟡 Phase 28: 고급 동시성 프리미티브

> 목표: `co_await` 기반 Go-style 동시성 — lock-free, zero-heap 프리미티브

### 비동기 채널 (`draco/concurrency/`)

- [ ] `[Common]` **`draco::Channel<T, N>`** — bounded SPSC 채널 (N=0이면 unbounded MPSC)
  - [ ] `co_await ch.send(value)` — 버퍼 포화 시 coroutine 일시 정지
  - [ ] `co_await ch.recv() → optional<T>` — 빈 채널 시 일시 정지
  - [ ] `ch.close()` — 채널 닫기 (수신자 `nullopt` 반환)
  - [ ] lock-free ring buffer 내부 구현

### Fan-out / Fan-in

- [ ] `[Common]` **`draco::WaitGroup`** — Go `sync.WaitGroup` 등가
  - [ ] `wg.add(n)` / `wg.done()` / `co_await wg.wait()`
  - [ ] 여러 coroutine 완료 대기
- [ ] `[Common]` **`draco::select(awaiters...)`** — 가장 먼저 완료되는 awaiter 선택
  - [ ] `auto [idx, val] = co_await draco::select(ch1.recv(), ch2.recv(), timer)`
- [ ] `[Common]` **`draco::gather(tasks...)`** — 모든 coroutine 완료 후 결과 튜플 반환
  - [ ] `auto [a, b, c] = co_await draco::gather(task_a, task_b, task_c)`
- [ ] `[Common]` **`draco::race(tasks...)`** — 가장 빠른 하나만 취하고 나머지 취소

### 취소 & 타임아웃

- [ ] `[Common]` **`draco::CancellationToken`** — 협력적 취소
  - [ ] `token.cancel()` → 대상 coroutine에 취소 신호
  - [ ] `co_await token.wait()` — 취소 대기
  - [ ] `token.is_cancelled()` — 폴링
- [ ] `[Common]` **`draco::with_timeout(task, duration)`** — deadline 초과 시 자동 취소
- [ ] `[Common]` **`draco::with_cancel(task, token)`** — 외부 토큰 기반 취소

### 동기화 프리미티브

- [ ] `[Common]` **`draco::AsyncSemaphore`** — `co_await sem.acquire()` / `sem.release()`
  - [ ] DB 동시 쿼리 수 제한, 파일 동시 쓰기 수 제한 등 활용
- [ ] `[Common]` **`draco::AsyncMutex`** — `co_await mutex.lock()` / `mutex.unlock()`
  - [ ] 코루틴 컨텍스트에서 안전한 상호 배제 (OS mutex보다 가벼움)
- [ ] `[Common]` **`draco::AsyncRwLock`** — 다중 읽기 / 단일 쓰기 lock (`co_await rw.read_lock()`)
- [ ] `[Common]` **`draco::Once`** — 한 번만 실행 보장 (`co_await once.do_once(fn)`)

### Lock-free 자료구조

- [ ] `[Common]` **`draco::SPSC<T, N>`** — lock-free single-producer single-consumer 링 큐
- [ ] `[Common]` **`draco::MPSC<T>`** — lock-free multi-producer single-consumer 큐 (io_uring 완료 큐)
- [ ] `[Common]` **`draco::MPMC<T, N>`** — lock-free multi-producer multi-consumer 큐
- [ ] `[Common]` **`draco::AtomicHashMap<K, V>`** — 읽기 lock-free 해시맵 (RCU 패턴)
  - [ ] 라우팅 테이블, 세션 맵 등 읽기 다수 / 쓰기 희소 워크로드

---

## 🟡 Phase 29: 최대 동시접속 한계 & 수평 확장

> 목표: 버티는 수준에서 최대 동시 접속 — OS 한도까지 쥐어짜고 초과 시 우아하게 거부

### 동시접속 한계 최적화

- [ ] `[Common]` **연결당 메모리 예산 분석 & 측정**
  - [ ] `Connection` 구조체 크기 측정 (`sizeof` + cache-line padding)
  - [ ] Arena per-connection 메모리 추적 (목표: < 1KB)
  - [ ] 최대 연결 수 = `min(ulimit -n, available_mem / conn_budget)` 자동 계산
- [ ] `[Common]` **Admission Control** — 허용 가능 연결 수 도달 시 신규 연결 즉시 503
  - [ ] `503 Service Unavailable` + `Retry-After` 헤더 반환
  - [ ] Accept 큐에서 드롭 (SYN 단계 거부 vs TCP 이후 거부 선택)
  - [ ] `SO_RCVBUF` 조정으로 accept 큐 백프레셔
- [ ] `[Linux]`  **`SO_REUSEPORT` + BPF 소켓 배분** — 다중 프로세스 accept, CPU 낭비 없음
- [ ] `[Linux]`  **파일 디스크립터 한도 자동 조정** — `setrlimit(RLIMIT_NOFILE)` 시작 시 최대값 설정
- [ ] `[Linux]`  **`net.core.somaxconn`** / **`net.ipv4.tcp_max_syn_backlog`** 튜닝 스크립트
- [ ] `[Common]` **Graceful Degradation** — 메모리/CPU 임계치 초과 시 비필수 미들웨어 비활성화

### 부하 측정 & 자동 조절

- [ ] `[Common]` **실시간 부하 지표** — `active_connections`, `queue_depth`, `cpu_usage` 실시간 측정
- [ ] `[Common]` **자동 backpressure 피드백** — 부하 지표 기반 Rate Limit 임계치 동적 조정
- [ ] `[Common]` **슬로우 클라이언트 감지** — 읽기/쓰기 속도 < 임계치 연결 강제 종료
- [ ] `[Common]` **요청 우선순위** — API 키 등급 기반 처리 우선순위 (`co_await` 재개 순서)

### 클러스터 & 수평 확장

- [ ] `[Common]` **멀티 프로세스 클러스터 모드**
  - [ ] `SO_REUSEPORT` 기반 워커 프로세스 분산 (Nginx worker 방식)
  - [ ] Master → Worker 시그널 기반 롤링 재시작 (Zero-downtime deploy)
  - [ ] Worker 크래시 시 Master 자동 재시작
- [ ] `[Common]` **헬스체크 & 로드밸런서 연동**
  - [ ] `GET /health` — `{"status":"ok","connections":N,"uptime":T}` (상세 상태)
  - [ ] `GET /ready` — 트래픽 수신 준비 여부 (DB 연결 등 체크)
  - [ ] `GET /live` — 프로세스 생존 여부 (Kubernetes liveness probe)
- [ ] `[Common]` **서비스 디스커버리 클라이언트**
  - [ ] DNS SRV 레코드 주기적 갱신
  - [ ] Kubernetes Endpoint Watch (kube API)
  - [ ] Consul 헬스체크 등록 / 디스커버리
- [ ] `[Common]` **일관된 해싱 (Consistent Hashing)** — Sticky session 라우팅 (Rendezvous hash)
- [ ] `[Common]` **분산 Rate Limiting** — Redis `INCR` 기반 전역 카운터 (Phase 25 RedisCache 활용)

---

## 🔴 Phase 30: 외부 의존성 관리 & 빌드 시스템 통합

> 목표: 모든 외부 의존성을 FetchContent / find_package / vcpkg 로 **버전 고정 + 라이선스 검증 + optional 처리** 완료

### 의존성 전체 목록 & CMake 통합 계획

> 표기: ✅ 완료 | 🔧 `FetchContent` | 🔍 `find_package` | 📦 `vcpkg/conan` | ⚠️ 주의사항

#### 🟥 필수 의존성 (always required)

| 라이브러리 | 최소 버전 | 라이선스 | 통합 방법 | 사용처 | 비고 |
|-----------|---------|---------|---------|-------|------|
| **Boost.JSON** (Beast JSON) | 1.85+ | BSL-1.0 | 🔧 FetchContent ✅ | HTTP JSON 파싱 | 이미 통합 완료 |
| **GoogleTest** | 1.14+ | BSD-3 | 🔧 FetchContent ✅ | 단위/통합 테스트 | 이미 통합 완료 |
| **zlib** | 1.3+ | zlib/libpng | 🔍 `find_package(ZLIB)` | gzip 압축(Phase 8), 로그 아카이브(Phase 24) | 거의 모든 시스템에 존재 |

#### 🟧 조건부 필수 의존성 (feature flag로 제어)

| 라이브러리 | 최소 버전 | 라이선스 | 통합 방법 | 사용처 | CMake 옵션 |
|-----------|---------|---------|---------|-------|-----------|
| **liburing** | 2.5+ | LGPL-2.1 | 🔍 `find_package(Liburing)` [Linux] | io_uring Reactor (Phase 5) | `DRACO_USE_IO_URING=ON` |
| **OpenSSL** | 3.0+ | Apache-2.0 | 🔍 `find_package(OpenSSL 3.0)` | TLS(Phase 10), crypto util(Phase 27) | `DRACO_USE_TLS=ON` |
| **nghttp2** | 1.61+ | MIT | 🔧 FetchContent or 🔍 find_package | HTTP/2 (Phase 11) | `DRACO_USE_HTTP2=ON` |
| **protobuf** | 26+ | BSD-3 | 🔧 FetchContent or 📦 vcpkg | gRPC(Phase 15), MessagePack 대안 | `DRACO_USE_GRPC=ON` |
| **libpq** | 16+ | PostgreSQL(BSD-like) | 🔍 `find_package(PostgreSQL)` | PostgreSQL 드라이버(Phase 23) | `DRACO_USE_DB_POSTGRES=ON` |
| **libmariadb** | 3.3+ | LGPL-2.1 | 🔍 `find_package(libmariadb)` | MySQL/MariaDB 드라이버(Phase 23) | `DRACO_USE_DB_MYSQL=ON` |
| **sqlite3** | 3.45+ | Public Domain | 🔧 FetchContent (amalgamation) | SQLite 드라이버(Phase 23) | `DRACO_USE_DB_SQLITE=ON` |
| **librdkafka** | 2.4+ | BSD-2 | 🔧 FetchContent or 📦 vcpkg | Kafka 드라이버(Phase 25) | `DRACO_USE_KAFKA=ON` |
| **opentelemetry-cpp** | 1.14+ | Apache-2.0 | 🔧 FetchContent or 📦 vcpkg | OTel 트레이싱(Phase 17) | `DRACO_USE_OTEL=ON` |
| **brotli** | 1.1+ | MIT | 🔧 FetchContent | Brotli 압축(Phase 8) | `DRACO_USE_BROTLI=ON` |

#### 🟨 헤더 전용 / 초경량 의존성 (항상 FetchContent로 번들)

| 라이브러리 | 버전 | 라이선스 | 통합 방법 | 사용처 |
|-----------|-----|---------|---------|-------|
| **xxHash** | 0.8.2+ | BSD-2 | 🔧 FetchContent (단일 헤더) | 비암호 해시(Phase 27) |
| **msgpack-cxx** | 6.1+ | BSL-1.0 | 🔧 FetchContent (헤더 전용) | MessagePack 직렬화(Phase 27) |
| **tinycbor** | 0.6.0+ | MIT | 🔧 FetchContent | CBOR 직렬화(Phase 27) |
| **nlohmann/json** | — | MIT | ❌ 불필요 | Beast JSON으로 대체 |

#### 🟦 선택적 성능 의존성 (opt-in, 없어도 빌드 성공)

| 라이브러리 | 버전 | 라이선스 | 통합 방법 | 사용처 | CMake 옵션 |
|-----------|-----|---------|---------|-------|-----------|
| **BoringSSL** | latest commit | BSD | git submodule + ExternalProject | OpenSSL 대안 TLS(Phase 10) | `DRACO_USE_BORINGSSL=ON` |
| **jemalloc** | 5.3+ | BSD-2 | 🔍 find_package (optional) | 전역 allocator 교체(Phase 6) | `DRACO_USE_JEMALLOC=ON` |
| **tcmalloc** (gperftools) | 2.15+ | BSD-3 | 🔍 find_package (optional) | 전역 allocator 교체(Phase 6) | `DRACO_USE_TCMALLOC=ON` |
| **libnuma** | 2.0+ | LGPL-2.1 | 🔍 find_package [Linux] | NUMA 배치(Phase 16) | `DRACO_USE_NUMA=ON` |
| **libbpf** | 1.3+ | LGPL-2.1 | 🔍 find_package [Linux] | AF_XDP(Phase 5) | `DRACO_USE_XDP=ON` |

#### 🟪 HTTP/3 / QUIC 의존성 (별도 빌드 파이프라인 필요)

| 라이브러리 | 버전 | 라이선스 | 통합 방법 | 주의사항 |
|-----------|-----|---------|---------|---------|
| **msquic** | 2.4+ | MIT | 🔧 FetchContent (CMake 지원) | `DRACO_USE_QUIC_MSQUIC=ON` |
| **quiche** (Cloudflare) | latest | BSD-2 | ExternalProject + cargo | Rust 툴체인 필요 — CI에 `rustup` 추가 |
| **lsquic** (LiteSpeed) | 4.0+ | MIT | 🔧 FetchContent | msquic 보다 C 친화적 |

> HTTP/3 기본값: `msquic` (순수 C, CMake 지원). `quiche`는 Rust 필요 → 선택적 대안.

### CMake 빌드 시스템 통합 작업

- [ ] `[Common]` **`cmake/FindLiburing.cmake`** — liburing custom finder (pkg-config 없을 때 대비)
- [ ] `[Common]` **`cmake/deps/core.cmake`** — 필수 의존성 FetchContent 선언 (Boost.JSON, GTest)
- [ ] `[Common]` **`cmake/deps/optional.cmake`** — 선택 의존성 find_package + FetchContent fallback
- [ ] `[Common]` **`cmake/deps/drivers.cmake`** — DB/Kafka 드라이버 per-flag 조건부 로드
- [ ] `[Common]` **`cmake/deps/perf.cmake`** — jemalloc / tcmalloc / BOLT 조건부 설정
- [ ] `[Common]` **버전 고정 전략**
  - [ ] FetchContent: `GIT_TAG` 로 SHA 고정 (branch 금지)
  - [ ] find_package: `VERSION 3.0 REQUIRED` 최소 버전 명시
  - [ ] vcpkg baseline: `vcpkg.json` + `builtin-baseline` SHA 고정
- [ ] `[Common]` **`vcpkg.json` 매니페스트 작성** — vcpkg 사용자 지원
  ```json
  {
    "name": "draco-was",
    "version": "0.2.0",
    "dependencies": [
      { "name": "openssl", "version>=": "3.0" },
      { "name": "nghttp2" },
      { "name": "protobuf" },
      { "name": "librdkafka" },
      { "name": "opentelemetry-cpp" }
    ],
    "features": {
      "postgres": { "description": "PostgreSQL driver", "dependencies": ["libpq"] },
      "mysql":    { "description": "MySQL driver",    "dependencies": ["libmariadb"] },
      "sqlite":   { "description": "SQLite driver",   "dependencies": ["sqlite3"] },
      "kafka":    { "description": "Kafka driver",    "dependencies": ["librdkafka"] }
    }
  }
  ```
- [ ] `[Common]` **`cmake/DracoFeatures.cmake`** — 활성 feature 목록 출력 (`cmake --preset release -LAH` 용)
- [ ] `[Common]` **CMake Presets** (`CMakePresets.json`)
  - [ ] `debug` — ASan + UBSan, no LTO
  - [ ] `release` — `-O3 -march=native`, LTO=thin
  - [ ] `sanitize-thread` — TSan
  - [ ] `sanitize-mem` — MSan
  - [ ] `pgo-instrument` — PGO 1차 계측 빌드
  - [ ] `pgo-optimize` — PGO 2차 최적화 빌드
  - [ ] `benchmark` — `-O3 -march=native -DNDEBUG`, no sanitizers

### 라이선스 호환성 검증

- [ ] `[Common]` **라이선스 호환성 매트릭스 문서화** (`LICENSES.md`)
  - [ ] LGPL-2.1 라이브러리 (liburing, libmariadb, libnuma, libbpf) — **동적 링크 권장**
    - 정적 링크 시 사용자에게 재링크 허용 필요 (LGPL 조항)
    - CMake: `set_target_properties(draco_db_mysql PROPERTIES POSITION_INDEPENDENT_CODE ON)` + SHARED 권장
  - [ ] GPL 라이브러리 — **사용 금지** (wrk/perf는 런타임 도구, 링크 안 함)
  - [ ] Apache-2.0 (OpenSSL 3.x, OTel) — MIT/BSD Draco와 호환
  - [ ] Public Domain (sqlite3) — 제한 없음
  - [ ] Rust 라이브러리 (quiche) — MIT/Apache-2.0 듀얼 라이선스, FFI 바인딩 → 호환
- [ ] `[Common]` **`scripts/check_licenses.sh`** — CMake FetchContent 소스 대상 라이선스 헤더 자동 검사
- [ ] `[Common]` **CI 라이선스 게이트** — GitHub Actions에서 `reuse lint` 실행, 라이선스 위반 시 PR 차단

### 빌드 환경 요구사항 문서화

- [ ] `[Common]` **시스템 패키지 설치 가이드** (`docs/build-deps.md`)
  ```bash
  # Ubuntu 24.04
  apt install -y \
    liburing-dev          \  # io_uring (Phase 5)
    libssl-dev            \  # OpenSSL (Phase 10)
    libpq-dev             \  # PostgreSQL (Phase 23)
    libmariadb-dev        \  # MySQL/MariaDB (Phase 23)
    librdkafka-dev        \  # Kafka (Phase 25)
    libnuma-dev           \  # NUMA (Phase 16)
    libbpf-dev xdp-tools  \  # AF_XDP (Phase 5)
    zlib1g-dev            \  # gzip (Phase 8)
    libbrotli-dev         \  # Brotli (Phase 8)
    valgrind              \  # 메모리 검사 (Phase 20)
    lcov                     # Coverage (Phase 20)
  ```
  ```bash
  # macOS (Homebrew)
  brew install openssl@3 nghttp2 protobuf librdkafka sqlite zlib brotli
  ```
- [ ] `[Common]` **Docker 개발 컨테이너** (`devcontainer/Dockerfile`) — 모든 의존성 사전 설치된 Ubuntu 24.04 이미지
- [ ] `[Common]` **GitHub Actions `setup-deps` 재사용 워크플로** — 매트릭스별 의존성 캐싱 (`actions/cache` + `ccache`)

### 의존성 보안 & 업데이트 관리

- [ ] `[Common]` **Dependabot 설정** (`.github/dependabot.yml`) — vcpkg / GitHub Actions 자동 업데이트 PR
- [ ] `[Common]` **CVE 스캔** — `trivy` 또는 `grype` 로 의존성 취약점 주간 스캔 (GitHub Actions scheduled)
- [ ] `[Common]` **FetchContent SHA 자동 갱신 스크립트** (`scripts/update_deps.sh`)
  - 각 라이브러리 latest release tag → SHA 갱신 → PR 생성

---

## 📌 Phase 우선순위 요약

| Phase | 핵심 내용 | 플랫폼 | 중요도 |
|-------|----------|--------|--------|
| **5**  | io_uring 완성 + Zero-Copy I/O (MSG_ZEROCOPY, SQPOLL, AF_XDP) | Linux / macOS | 🔴 Critical |
| **6**  | Zero-Cost C++ (CRTP, SIMD 파서, LTO/PGO, Cache-line 정렬) | Common | 🔴 Critical |
| **7**  | HTTP/1.1 완전 준수 (Keep-Alive, Chunked, Timeout, TCP 튜닝) | Common | 🔴 Critical |
| **8**  | 핵심 미들웨어 (CORS, Body Parser, Rate Limit, 비동기 로깅) | Common | 🔴 Critical |
| **9**  | 보안 하드닝 (Smuggling 방지, HSTS, CSP, CSRF, 상수시간 비교) | Common | 🔴 Critical |
| **10** | TLS/SSL (HTTPS, SNI, ALPN, TLS 1.3, KTLS) | Common + Linux | 🔴 Critical |
| **11** | HTTP/2 (nghttp2, HPACK, Server Push, Flow Control) | Common | 🟠 High |
| **12** | HTTP/3 / QUIC (0-RTT, 연결 마이그레이션, Alt-Svc) | Common | 🟠 High |
| **13** | WebSocket RFC 6455 (per-message deflate, Broadcast) | Common | 🟠 High |
| **14** | Streaming / SSE / Backpressure | Common | 🟠 High |
| **15** | gRPC (Protobuf, 4가지 스트리밍 모드, gRPC-Web) | Common | 🟠 High |
| **16** | 네트워크 심화 (IPv6, UDS, 비동기 HTTP 클라이언트, NUMA) | Common + Linux + macOS | 🟠 High |
| **17** | Observability (Prometheus, OpenTelemetry, 비동기 로거) | Common | 🟠 High |
| **18** | URL / Request 유틸리티 | Common | 🟡 Medium |
| **19** | 성능 & 벤치마킹 (C10M, TechEmpower, PGO, BOLT) | Common + Linux + macOS | 🟡 Medium |
| **20** | 테스트 & CI/CD (Fuzzing, MSan, Nightly Regression) | Common + Linux + macOS | 🟡 Medium |
| **21** | 문서화 & DX (API Ref, OS 튜닝 스크립트, 보안 가이드) | Common | 🟢 Low |
| **22** | SOLID 설계 & 확장 인터페이스 (ISP, DIP, DI Container, CMake 모듈 분리) | Common | 🔴 Critical |
| **23** | DB 추상화 & 드라이버 (PostgreSQL, MySQL, SQLite, Redis) | Common | 🔴 Critical |
| **24** | 로깅 시스템 심화 (ILogSink, FileSink/Loki/Fluentd, 비동기 로테이션) | Common | 🔴 Critical |
| **25** | 캐시 & 메시지 브로커 (LruCache, RedisPubSub, Kafka, TaskQueue) | Common | 🟠 High |
| **26** | 커넥션 풀 & 복원력 (CircuitBreaker, RetryPolicy, Bulkhead, Timeout) | Common | 🟠 High |
| **27** | WAS 공용 유틸리티 (UUID, SIMD Base64, HMAC, Result<T,E>, Msgpack) | Common | 🟠 High |
| **28** | 고급 동시성 (Channel, WaitGroup, Select, SPSC/MPSC, AtomicHashMap) | Common | 🟡 Medium |
| **29** | 최대 동시접속 & 수평 확장 (Admission Control, Cluster, K8s probe, 서비스 디스커버리) | Common + Linux | 🟡 Medium |
| **30** | 외부 의존성 관리 (FetchContent/vcpkg 버전 고정, LGPL 동적 링크, CVE 스캔, devcontainer) | Common | 🔴 Critical |

---

## 📝 Contribution & Issues

- 버그는 재현 가능한 C++ 예제와 함께 GitHub Issue 로 제출
- 성능 프로파일링: `[Linux]` `perf record -g` / `[macOS]` Instruments Time Profiler
- PR 제출 전 `clang-tidy`, `AddressSanitizer`, `ThreadSanitizer` 통과 필수
- 성능 PR은 `wrk` 벤치마크 before/after 수치 첨부

---

*Created by The LKB Innovations.*
